/* Windows x64 front end: the same game, menu and renderer logic as switch/main_switch.c, on Win32.
 * The .exe sits next to the disc image (.iso / .gcm / .ciso / .rvz) and keeps the Switch build's
 * folders beside it: logs/, config/, save/, mods/ (the same files: a Switch SD card's MKDD-NX folder
 * can be copied over as it is).
 *
 * Window: Win32 + an OpenGL 4.5 core context (render_gl.c). F11 or Alt+Enter: borderless full screen.
 * Input: up to 4 controllers through SDL2 (pad_sdl.c; players in the order they connected) plus
 * the keyboard (wincfg.c keys) as player 1, as Switch button bits (nx_compat.h) so the menu's
 * BUTTON MAPPING works the same. Before the game: the launcher (launcher.c). Audio: waveOut, 48 kHz stereo, resampled from the game rate as on the Switch. */
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <mmsystem.h>
#include <dirent.h>
#include <direct.h>
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include "nx_compat.h"
#include "gl.h"
#include "win.h"
#include "pad.h"
#include "wincfg.h"
#include "../runtime/gx.h"
#include "../runtime/rt.h"
#include "../runtime/route.h"
#include "../switch/gfx.h"
#include "../switch/render.h"
#include "../switch/menu.h"
#include "../switch/pause_art.h"
#include "../switch/charpack_art.h"
#include "../switch/hdtex.h"
#include "../switch/update.h"
#include "../switch/mods_setup.h"
#include "../runtime/native.h"
#include "../runtime/table.h"
const char g_build_stamp[] = __DATE__ " " __TIME__;

#define HD_PATH MODS_DIR "/hd/" HD_FILE

static const char *s_mod_key;
static u64 s_boot_tick;
static Ctx s_boot;
static bool s_quit;
static float s_fps;
uint64_t g_audio_late_cb;
extern volatile uint32_t g_race_frames, g_race_phase, g_race_drive;

/* ---- time ---- */
u64 armGetSystemTick(void) {
    static LARGE_INTEGER f;
    LARGE_INTEGER c;
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    /* 19.2 MHz ticks, as on the Switch */
    return (u64)((double)c.QuadPart * 19200000.0 / (double)f.QuadPart);
}

/* ---- nifm stand-in: this PC's IPv4 address on the network ---- */
Result nifmInitialize(int type) {
    (void)type;
    return plat_net_up() ? 0 : 1;
}
Result nifmGetCurrentIpAddress(u32 *ip) {
    /* the address a UDP socket "connected" outward would send from (nothing is sent) */
    SOCKET s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s == INVALID_SOCKET) return 1;
    struct sockaddr_in to = {0}, me = {0};
    to.sin_family = AF_INET;
    to.sin_port = htons(9);
    to.sin_addr.s_addr = htonl(0xC0A80001u); /* 192.168.0.1: any private address picks the LAN adapter */
    int len = sizeof me;
    bool ok = connect(s, (struct sockaddr *)&to, sizeof to) == 0 && getsockname(s, (struct sockaddr *)&me, &len) == 0;
    closesocket(s);
    if (!ok || !me.sin_addr.s_addr) return 1;
    *ip = me.sin_addr.s_addr;
    return 0;
}

/* ---- crash.log ---- */
static LONG WINAPI crash_handler(EXCEPTION_POINTERS *ep) {
    HMODULE mod = GetModuleHandleA(NULL);
    u64 base = (u64)(uintptr_t)mod, pc = (u64)(uintptr_t)ep->ExceptionRecord->ExceptionAddress;
    FILE *f = fopen(LOG_DIR "/crash.log", "w");
    if (f) {
        fprintf(f, "MKDD-NX (Windows) crash, build %s\nexception 0x%08lx at 0x%016llx (image+0x%llx)\nlast guest fn: %#010x %s\n",
                g_build_stamp, ep->ExceptionRecord->ExceptionCode, (unsigned long long)pc, (unsigned long long)(pc - base), g_last_fn,
                m_name(g_last_fn));
        if (ep->ExceptionRecord->NumberParameters >= 2)
            fprintf(f, "access %s 0x%016llx\n", ep->ExceptionRecord->ExceptionInformation[0] ? "write" : "read",
                    (unsigned long long)ep->ExceptionRecord->ExceptionInformation[1]);
        CONTEXT *c = ep->ContextRecord;
        fprintf(f, "RAX %016llx RBX %016llx RCX %016llx RDX %016llx\nRSI %016llx RDI %016llx RBP %016llx RSP %016llx\n",
                c->Rax, c->Rbx, c->Rcx, c->Rdx, c->Rsi, c->Rdi, c->Rbp, c->Rsp);
        fprintf(f, "guest RAM 0x%llx ARAM 0x%llx\nstack (values inside the image):\n", (unsigned long long)(uintptr_t)g_ram,
                (unsigned long long)(uintptr_t)g_aram);
        u64 *sp = (u64 *)(uintptr_t)c->Rsp;
        MEMORY_BASIC_INFORMATION mi;
        if (VirtualQuery(mod, &mi, sizeof mi)) {
            u64 end = base + 64ull * 1024 * 1024;
            for (int i = 0, n = 0; i < 2048 && n < 32; i++)
                if (sp[i] >= base && sp[i] < end) fprintf(f, "  image+0x%llx\n", (unsigned long long)(sp[i] - base)), n++;
        }
        fclose(f);
    }
    diag_flush_crash();
    return EXCEPTION_EXECUTE_HANDLER;
}

/* ---- window and OpenGL context ---- */
static HWND s_wnd;
static HDC s_dc;
static HGLRC s_rc;
static bool s_fullscreen, s_closed, s_focus = true;
static WINDOWPLACEMENT s_place = {sizeof s_place};
static bool s_key[256], s_key_hit[256]; /* held; pressed since the last poll (a tap shorter than a frame still counts) */

static void toggle_fullscreen(void) {
    DWORD st = (DWORD)GetWindowLongPtrA(s_wnd, GWL_STYLE);
    if (!s_fullscreen) {
        MONITORINFO mi = {sizeof mi};
        if (GetWindowPlacement(s_wnd, &s_place) && GetMonitorInfoA(MonitorFromWindow(s_wnd, MONITOR_DEFAULTTOPRIMARY), &mi)) {
            SetWindowLongPtrA(s_wnd, GWL_STYLE, st & ~WS_OVERLAPPEDWINDOW);
            SetWindowPos(s_wnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left,
                         mi.rcMonitor.bottom - mi.rcMonitor.top, SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
            s_fullscreen = true;
        }
    } else {
        SetWindowLongPtrA(s_wnd, GWL_STYLE, st | WS_OVERLAPPEDWINDOW);
        SetWindowPlacement(s_wnd, &s_place);
        SetWindowPos(s_wnd, NULL, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
        s_fullscreen = false;
    }
    g_win.fullscreen = s_fullscreen; /* remembered for the next start */
    wincfg_save();
}

static LRESULT CALLBACK wnd_proc(HWND w, UINT m, WPARAM wp, LPARAM lp) {
    switch (m) {
    case WM_CLOSE: s_closed = true; return 0;
    case WM_SETFOCUS: s_focus = true; return 0;
    case WM_KILLFOCUS: s_focus = false; memset(s_key, 0, sizeof s_key); return 0;
    case WM_SYSKEYDOWN:
        if (wp == VK_RETURN && (lp & (1 << 29))) { toggle_fullscreen(); return 0; }
        /* fall through */
    case WM_KEYDOWN:
        if (wp == VK_F11 && !(lp & (1 << 30))) toggle_fullscreen();
        if (wp < 256) s_key[wp] = s_key_hit[wp] = true;
        if (m == WM_SYSKEYDOWN && wp == VK_F4) s_closed = true;
        return 0;
    case WM_SYSKEYUP:
    case WM_KEYUP:
        if (wp < 256) s_key[wp] = false;
        return 0;
    case WM_SYSCOMMAND:
        if ((wp & 0xFFF0) == SC_KEYMENU) return 0; /* Alt alone doesn't open the window menu */
        break;
    }
    return DefWindowProcA(w, m, wp, lp);
}

void win_pump(void) {
    MSG msg;
    while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
}

static void *gl_proc(const char *name) {
    void *p = (void *)wglGetProcAddress(name);
    if (!p || p == (void *)1 || p == (void *)2 || p == (void *)3 || p == (void *)-1) {
        static HMODULE gl;
        if (!gl) gl = LoadLibraryA("opengl32.dll");
        p = (void *)GetProcAddress(gl, name);
    }
    return p;
}

bool win_gl_create(int w, int h) {
    HINSTANCE inst = GetModuleHandleA(NULL);
    WNDCLASSA wc = {0};
    wc.style = CS_OWNDC;
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hIcon = LoadIconA(inst, MAKEINTRESOURCEA(1));
    wc.lpszClassName = "MKDDNX";
    RegisterClassA(&wc);
    RECT r = {0, 0, w, h};
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    s_wnd = CreateWindowA("MKDDNX", APP_TITLE, WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left, r.bottom - r.top,
                          NULL, NULL, inst, NULL);
    if (!s_wnd) { rt_log("window: CreateWindow failed (%lu)", GetLastError()); return false; }
    s_dc = GetDC(s_wnd);
    PIXELFORMATDESCRIPTOR pfd = {sizeof pfd, 1, PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER, PFD_TYPE_RGBA, 32};
    pfd.cDepthBits = 0;
    int pf = ChoosePixelFormat(s_dc, &pfd);
    if (!pf || !SetPixelFormat(s_dc, pf, &pfd)) { rt_log("window: no pixel format (%lu)", GetLastError()); return false; }
    /* a legacy context first: wglCreateContextAttribsARB comes from it */
    HGLRC tmp = wglCreateContext(s_dc);
    if (!tmp || !wglMakeCurrent(s_dc, tmp)) { rt_log("window: no OpenGL driver"); return false; }
    typedef HGLRC(WINAPI * CreateAttribs)(HDC, HGLRC, const int *);
    CreateAttribs create = (CreateAttribs)(void *)wglGetProcAddress("wglCreateContextAttribsARB");
    static const int attr[] = {0x2091, 4, 0x2092, 5, /* WGL_CONTEXT_MAJOR / MINOR_VERSION_ARB */
                               0x9126, 0x00000001,   /* WGL_CONTEXT_PROFILE_MASK_ARB: core */
                               0};
    s_rc = create ? create(s_dc, NULL, attr) : NULL;
    wglMakeCurrent(NULL, NULL);
    wglDeleteContext(tmp);
    if (!s_rc || !wglMakeCurrent(s_dc, s_rc)) {
        MessageBoxA(NULL, "MKDD-NX needs a graphics driver with OpenGL 4.5.\nUpdate the GPU driver and try again.", APP_TITLE, MB_ICONERROR);
        rt_log("window: no OpenGL 4.5 core context");
        return false;
    }
    if (!gl_load(gl_proc)) { rt_log("window: OpenGL functions missing: %s", gl_missing()); return false; }
    typedef BOOL(WINAPI * SwapInterval)(int);
    SwapInterval si = (SwapInterval)(void *)wglGetProcAddress("wglSwapIntervalEXT");
    if (si) si(1);
    ShowWindow(s_wnd, SW_SHOW);
    if (g_win.fullscreen) toggle_fullscreen();
    win_pump();
    return true;
}
void win_gl_destroy(void) {
    if (s_rc) { wglMakeCurrent(NULL, NULL); wglDeleteContext(s_rc); s_rc = NULL; }
    if (s_wnd) { ReleaseDC(s_wnd, s_dc); DestroyWindow(s_wnd); s_wnd = NULL; }
}
void win_client_size(int *w, int *h) {
    RECT r;
    GetClientRect(s_wnd, &r);
    *w = r.right > 0 ? r.right : 1;
    *h = r.bottom > 0 ? r.bottom : 1;
}
/* SwapBuffers with vsync on; when the driver does not wait (vsync forced off in its settings, some
 * windowed setups), the loop is held to the monitor's refresh rate here instead of spinning */
void win_swap(void) {
    SwapBuffers(s_dc);
    static u64 last, period;
    if (!period) {
        DEVMODEA dm = {.dmSize = sizeof dm};
        DWORD hz = EnumDisplaySettingsA(NULL, ENUM_CURRENT_SETTINGS, &dm) && dm.dmDisplayFrequency > 1 ? dm.dmDisplayFrequency : 60;
        period = 1000000000ull / hz;
        rt_log("display: %lu Hz", hz);
    }
    u64 now = armTicksToNs(armGetSystemTick());
    if (last && now - last < period * 19 / 20) {
        u64 target = last + period;
        while ((now = armTicksToNs(armGetSystemTick())) < target) {
            if (target - now > 2000000) Sleep(1);
            else SwitchToThread();
        }
    }
    last = now;
}

/* ---- input: SDL2 controllers (pad_sdl.c) + the keyboard -> Switch button bits ---- */
static Pad s_kbd; /* the keyboard, merged into player 1 */
static int s_kbd_players = 1; /* --kbd-players N (testing split screen): the keyboard drives players 1-N */
static int s_player_src[4] = {-1, -1, -1, -1}; /* g_pads slot of each player, -1 none */

static void stick_bits(u64 *b, s32 x, s32 y, bool left) {
    const s32 T = 16000;
    if (x < -T) *b |= left ? HidNpadButton_StickLLeft : HidNpadButton_StickRLeft;
    if (x > T) *b |= left ? HidNpadButton_StickLRight : HidNpadButton_StickRRight;
    if (y > T) *b |= left ? HidNpadButton_StickLUp : HidNpadButton_StickRUp;
    if (y < -T) *b |= left ? HidNpadButton_StickLDown : HidNpadButton_StickRDown;
}
/* The keyboard as a controller (player 1, with or without a pad): the launcher's KEYBOARD page
 * (config/windows.ini; wincfg.c KB_DEFAULT: arrows / WASD stick, X = A, Z = B, Enter = +, Esc = -) */
static void poll_keyboard(void) {
    Pad *p = &s_kbd;
    p->prev = p->b;
    u64 b = 0;
    bool k[256];
    for (int i = 0; i < 256; i++) k[i] = s_key[i] || s_key_hit[i], s_key_hit[i] = false;
    bool row[KB_COUNT];
    for (int r = 0; r < KB_COUNT; r++) {
        row[r] = false;
        for (int j = 0; j < KB_KEYS; j++) {
            int vk = g_win.keys[r][j];
            if (vk > 0 && vk < 256 && k[vk]) row[r] = true;
        }
    }
    static const u64 BIT[KB_LUP] = {HidNpadButton_A, HidNpadButton_B, HidNpadButton_X, HidNpadButton_Y, HidNpadButton_L, HidNpadButton_R,
                                    HidNpadButton_ZL, HidNpadButton_ZR, HidNpadButton_Plus, HidNpadButton_Minus,
                                    HidNpadButton_StickL, HidNpadButton_StickR};
    for (int r = 0; r < KB_LUP; r++)
        if (row[r]) b |= BIT[r];
    if (row[KB_DUP]) b |= HidNpadButton_Up;
    if (row[KB_DDOWN]) b |= HidNpadButton_Down;
    if (row[KB_DLEFT]) b |= HidNpadButton_Left;
    if (row[KB_DRIGHT]) b |= HidNpadButton_Right;
    int dx = row[KB_LRIGHT] - row[KB_LLEFT], dy = row[KB_LUP] - row[KB_LDOWN];
    int rx = row[KB_RRIGHT] - row[KB_RLEFT], ry = row[KB_RUP] - row[KB_RDOWN];
    p->lx = dx * 32767, p->ly = dy * 32767, p->rx = rx * 32767, p->ry = ry * 32767;
    if (dx && dy) p->lx = dx * 23170, p->ly = dy * 23170;
    p->lt = b & HidNpadButton_ZL ? 255 : 0, p->rt = b & HidNpadButton_ZR ? 255 : 0;
    stick_bits(&b, p->lx, p->ly, true);
    stick_bits(&b, p->rx, p->ry, false);
    p->b = s_focus ? b : 0;
    p->on = true;
}
/* one player's controller state (player 1: their pad and the keyboard together) */
static void player_state(int pl, u64 *held, u64 *down, s32 *lx, s32 *ly, s32 *rx, s32 *ry) {
    *held = *down = 0, *lx = *ly = *rx = *ry = 0;
    int src = s_player_src[pl];
    if (src >= 0) {
        Pad *p = &g_pads[src];
        *held = p->b, *down = p->b & ~p->prev, *lx = p->lx, *ly = p->ly, *rx = p->rx, *ry = p->ry;
    }
    if (pl < s_kbd_players) {
        Pad *k = &s_kbd;
        *held |= k->b, *down |= k->b & ~k->prev;
        if (k->lx || k->ly) *lx = k->lx, *ly = k->ly;
        if (k->rx || k->ry) *rx = k->rx, *ry = k->ry;
    }
}
static void input_poll(void) {
    win_pump();
    pad_poll();
    poll_keyboard();
}

static int8_t stick(s32 v) {
    int x = v * 100 / 32767;
    return (int8_t)(x > 127 ? 127 : x < -128 ? -128 : x);
}

/* rumble: PADControlMotor -> the player's controller */
static void rumble_update(bool paused) {
    static const uint16_t AMP[3] = {0, 22000, 45000};
    unsigned motors = pad_motor_mask();
    for (int p = 0; p < 4; p++) {
        int src = s_player_src[p];
        if (src < 0) continue;
        bool on = !paused && g_set.rumble && (motors >> p & 1);
        pad_rumble(src, on ? AMP[g_set.rumble > 2 ? 2 : g_set.rumble] : 0);
    }
}
static void rumble_off(void) {
    for (int i = 0; i < PAD_SLOTS; i++) pad_rumble(i, 0);
}

/* ---- loading / message screens ---- */
static void screen(const char *line1, const char *line2, float progress) {
    int w = 0, h = 0;
    win_client_size(&w, &h);
    gfx_resize(w, h);
    gfx_size(&w, &h);
    menu_loading(w, h, line1, line2, progress);
    gfx_swap();
    win_pump();
}
static void shader_progress(unsigned done, unsigned total) {
    char line[64];
    snprintf(line, sizeof line, "PREPARING SHADERS %u / %u", done, total);
    screen(line, NULL, 0.3f + 0.25f * (float)done / (float)(total ? total : 1));
}
static __attribute__((noreturn)) void fatal(const char *line1, const char *line2) {
    rt_log("fatal: %s / %s", line1, line2 ? line2 : "");
    diag_flush();
    while (!s_closed) {
        input_poll();
        if ((s_kbd.b & ~s_kbd.prev & (HidNpadButton_Plus | HidNpadButton_Minus)) ||
            (g_pads[0].b & ~g_pads[0].prev & HidNpadButton_Plus))
            break;
        screen(line1, line2 ? line2 : "PRESS ENTER OR START TO EXIT", -1);
    }
    gfx_close();
    exit(0);
}

static int choose(const char *title, const char *a, const char *b, int sel, const char *hint) {
    const char *opts[2] = {a, b};
    input_poll();
    while (!s_closed) {
        input_poll();
        u64 held, down;
        s32 x, y, rx, ry;
        player_state(0, &held, &down, &x, &y, &rx, &ry);
        if (down & HidNpadButton_AnyUp) sel = 0;
        if (down & HidNpadButton_AnyDown) sel = 1;
        if (down & HidNpadButton_A) break;
        int w = 0, h = 0;
        win_client_size(&w, &h);
        gfx_resize(w, h);
        gfx_size(&w, &h);
        menu_choice(w, h, title, opts, 2, sel, hint);
        gfx_swap();
    }
    return sel;
}
#define SYSTEM_GCI "/card_a/01-GM4E-MarioKart Double Dash!!.gci"
static bool has_gci(const char *dir) {
    DIR *d = opendir(dir);
    if (!d) return false;
    struct dirent *e;
    bool found = false;
    while (!found && (e = readdir(d))) {
        const char *dot = strrchr(e->d_name, '.');
        found = dot && !strcasecmp(dot, ".gci");
    }
    closedir(d);
    return found;
}
static void offer_save_copy(const char *save_dir, const char *key) {
    struct stat st;
    char card[300], gci[400], title[64];
    snprintf(card, sizeof card, "%s/card_a", save_dir);
    if (has_gci(card) || stat(SAVE_DIR SYSTEM_GCI, &st) != 0) return;
    snprintf(title, sizeof title, "%s HAS NO SAVE YET", key);
    for (char *t = title; *t; t++) *t = (char)toupper((unsigned char)*t);
    if (choose(title, "COPY MY MARIO KART SAVE", "START A NEW SAVE", 0, "UNLOCKS AND RECORDS; YOUR SAVE STAYS AS IT IS") != 0) return;
    _mkdir(card);
    snprintf(gci, sizeof gci, "%s" SYSTEM_GCI, save_dir);
    size_t size;
    void *d = plat_read_file(SAVE_DIR SYSTEM_GCI, &size);
    bool ok = d && plat_write_file_atomic(gci, d, size);
    free(d);
    rt_log("mod: copying the standard save %s", ok ? "done" : "FAILED");
}

/* ---- audio: waveOut, pulled from the AI queue and resampled as on the Switch ---- */
#define AUDIO_BUFS_MAX 6
#define AUDIO_FRAMES_MAX 1024
static int s_abufs = 4, s_aframes = 1024; /* windows.ini latency: low 4 x 512, normal 4 x 1024, high 6 x 1024 */
static int s_avol = 256;                  /* volume, 256 = 100% */
static double s_apos;
static int16_t s_aprev[2], s_acur[2];
static void audio_fill(int16_t *out, int frames) {
    double step = (double)audio_rate() / 48000.0 * audio_rate_trim();
    for (int i = 0; i < frames; i++) {
        s_apos += step;
        while (s_apos >= 1.0) {
            s_apos -= 1.0;
            s_aprev[0] = s_acur[0], s_aprev[1] = s_acur[1];
            audio_pop(s_acur, 1);
        }
        int l = (int)(s_aprev[0] + (s_acur[0] - s_aprev[0]) * s_apos), r = (int)(s_aprev[1] + (s_acur[1] - s_aprev[1]) * s_apos);
        out[2 * i] = (int16_t)(l * s_avol >> 8), out[2 * i + 1] = (int16_t)(r * s_avol >> 8);
    }
}
static void audio_thread(void *arg) {
    (void)arg;
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    HANDLE ev = CreateEventA(NULL, FALSE, FALSE, NULL);
    WAVEFORMATEX fmt = {WAVE_FORMAT_PCM, 2, 48000, 48000 * 4, 4, 16, 0};
    HWAVEOUT wo;
    s_avol = g_win.volume * 256 / 100;
    if (g_win.latency == 0) s_abufs = 4, s_aframes = 512;
    else if (g_win.latency == 2) s_abufs = 6, s_aframes = 1024;
    /* the launcher's output device, by name (its number changes as devices come and go) */
    UINT dev = WAVE_MAPPER;
    for (UINT i = 0, n = waveOutGetNumDevs(); g_win.audio_dev[0] && i < n; i++) {
        WAVEOUTCAPSA caps;
        if (waveOutGetDevCapsA(i, &caps, sizeof caps) == MMSYSERR_NOERROR && !strcmp(caps.szPname, g_win.audio_dev)) dev = i;
    }
    if (g_win.audio_dev[0] && dev == WAVE_MAPPER) rt_log("audio: \"%s\" not found, using the default device", g_win.audio_dev);
    MMRESULT rc = waveOutOpen(&wo, dev, &fmt, (DWORD_PTR)ev, 0, CALLBACK_EVENT);
    if (rc != MMSYSERR_NOERROR && dev != WAVE_MAPPER) rc = waveOutOpen(&wo, WAVE_MAPPER, &fmt, (DWORD_PTR)ev, 0, CALLBACK_EVENT);
    if (rc != MMSYSERR_NOERROR) { rt_log("audio: waveOutOpen failed (%u)", rc); return; }
    static int16_t buf[AUDIO_BUFS_MAX][AUDIO_FRAMES_MAX * 2];
    static WAVEHDR hdr[AUDIO_BUFS_MAX];
    for (int i = 0; i < s_abufs; i++) {
        hdr[i].lpData = (LPSTR)buf[i];
        hdr[i].dwBufferLength = (DWORD)(s_aframes * 4);
        waveOutPrepareHeader(wo, &hdr[i], sizeof hdr[i]);
        audio_fill(buf[i], s_aframes);
        waveOutWrite(wo, &hdr[i], sizeof hdr[i]);
    }
    rt_log("audio: waveOut 48 kHz, %d buffers of %d frames, volume %d%%", s_abufs, s_aframes, g_win.volume);
    u64 last = 0;
    for (;;) {
        WaitForSingleObject(ev, 100);
        for (int i = 0; i < s_abufs; i++) {
            if (!(hdr[i].dwFlags & WHDR_DONE)) continue;
            u64 now = armGetSystemTick();
            if (last && armTicksToNs(now - last) > (u64)s_aframes * 1000000000ull / 48000 * 3 / 2) g_audio_late_cb++;
            last = now;
            audio_fill(buf[i], s_aframes);
            waveOutWrite(wo, &hdr[i], sizeof hdr[i]);
        }
    }
}

/* ---- pads -> the game (the parts of main_switch.c update_pads that apply to these controllers) ---- */
static void update_pads(void) {
    input_poll();
    /* players: each controller's number (the first free one when it connected, or the launcher's
     * choice); player 1 also has the keyboard */
    int src_of[4] = {-1, -1, -1, -1};
    for (int i = 0; i < PAD_SLOTS; i++)
        if (g_pads[i].on && g_pads[i].player >= 0 && g_pads[i].player < 4 && src_of[g_pads[i].player] < 0) src_of[g_pads[i].player] = i;
    for (int p = 0; p < 4; p++) {
        if (src_of[p] == s_player_src[p]) continue;
        if (src_of[p] < 0 && s_player_src[p] >= 0 && p > 0) {
            char note[64];
            snprintf(note, sizeof note, "PLAYER %d CONTROLLER DISCONNECTED", p + 1);
            menu_open(note);
        }
        rt_log("pad: player %d is %s", p + 1, src_of[p] >= 0 ? g_pads[src_of[p]].name : p == 0 ? "the keyboard" : "not connected");
        s_player_src[p] = src_of[p];
    }
    g_p1_gamecube = s_player_src[0] >= 0 && g_pads[s_player_src[0]].gc;
    s32 lx, ly, rx, ry;
    /* each player's - opens the menu for them (players 2-4: their camera and buttons); a GameCube
     * controller has no -: Z (ZR here) + Start, as on the Switch */
    uint64_t mdown[4] = {0, 0, 0, 0}, mheld[4] = {0, 0, 0, 0};
    for (int p = 0; p < 4; p++) {
        if (p >= s_kbd_players && s_player_src[p] < 0) continue;
        u64 h, d;
        player_state(p, &h, &d, &lx, &ly, &rx, &ry);
        bool gc = s_player_src[p] >= 0 && g_pads[s_player_src[p]].gc;
        if (gc && (h & HidNpadButton_ZR) && (h & HidNpadButton_Plus)) {
            if (d & HidNpadButton_Plus) d |= HidNpadButton_Minus;
            h |= HidNpadButton_Minus;
        }
        mdown[p] = d, mheld[p] = h;
    }
    bool menu = menu_input(mdown, mheld, &s_quit);
    uint32_t jump = 0, cam_reset = 0, trick = 0;
    uint32_t drive = g_race_drive;
    uint64_t std = 0;
    for (int i = 0; i < GC_COUNT; i++) std |= g_map_default[i];
    static const uint16_t GC_BITS[GC_COUNT] = {PAD_A, PAD_B, PAD_X, PAD_Y, PAD_TRIGGER_Z, PAD_TRIGGER_L, PAD_TRIGGER_R, PAD_START};
    for (int p = 0; p < 4; p++) {
        PadStatus s = {0};
        u64 b, down;
        player_state(p, &b, &down, &lx, &ly, &rx, &ry);
        /* each player's own driving buttons (their - menu, the launcher), else player 1's */
        PlayerSet ps;
        player_set(p, &ps);
        const uint64_t *map = drive == 1 ? ps.map : g_map_default;
        uint64_t resume = drive == 2 ? ps.map[GC_START] & ~std : 0;
        bool have = s_player_src[p] >= 0 || p < s_kbd_players;
        s.connected = have;
        if (have && !menu) {
            b &= ~menu_blocked(p);
            if (drive == 1 && (b & map[MAP_JUMP])) jump |= 1u << p;
            if (drive == 1 && (b & map[MAP_CAM_RESET])) cam_reset |= 1u << p;
            /* TRICKS (native.c): the D-pad as the controller has it (it still steers too) */
            if (drive == 1)
                trick |= (uint32_t)((b & HidNpadButton_Up ? TRICK_UP : 0) | (b & HidNpadButton_Down ? TRICK_DOWN : 0) |
                                    (b & HidNpadButton_Left ? TRICK_LEFT : 0) | (b & HidNpadButton_Right ? TRICK_RIGHT : 0)) << 4 * p;
            s.stick_x = stick(lx), s.stick_y = stick(ly);
            s.substick_x = stick(rx), s.substick_y = stick(ry);
            int src = s_player_src[p];
            const Pad *pd = src >= 0 ? &g_pads[src] : NULL;
            /* TRICKS by stick too (main_switch.c): a flick from the middle past 80% within a few
             * frames, controllers only (the keyboard's stick keys jump straight out) */
            if (drive == 1 && pd) {
                static int since_mid[4];
                static uint32_t flick[4];
                int ax = lx < 0 ? -lx : lx, ay = ly < 0 ? -ly : ly, m = ax > ay ? ax : ay;
                if (m < 32767 * 3 / 10) since_mid[p] = 0, flick[p] = 0;
                else if (since_mid[p] < 100) since_mid[p]++;
                if (!flick[p] && m > 32767 * 8 / 10 && since_mid[p] <= 5)
                    flick[p] = ax > ay ? (lx < 0 ? TRICK_LEFT : TRICK_RIGHT) : (ly > 0 ? TRICK_UP : TRICK_DOWN);
                if (m < 32767 * 6 / 10) flick[p] = 0;
                trick |= flick[p] << 4 * p;
            }
            /* the signature: ZL + ZR held (a GameCube pad's L + R; main_switch.c) */
            {
                u64 sig2 = pd && pd->gc ? HidNpadButton_L | HidNpadButton_R : HidNpadButton_ZL | HidNpadButton_ZR;
                if (drive == 1 && ((pd && pd->gc ? pd->b : b) & sig2) == sig2) trick |= 1u << (TRICK_SIG_BIT + p);
            }
            if (pd && pd->gc) {
                /* GameCube controller: its own layout and analog L / R (main_switch.c map_gc) */
                static const struct { u64 bit; uint16_t gc; } GCL[] = {
                    {HidNpadButton_A, PAD_A}, {HidNpadButton_B, PAD_B}, {HidNpadButton_X, PAD_X}, {HidNpadButton_Y, PAD_Y},
                    {HidNpadButton_ZR, PAD_TRIGGER_Z}, {HidNpadButton_L, PAD_TRIGGER_L}, {HidNpadButton_R, PAD_TRIGGER_R}, {HidNpadButton_Plus, PAD_START},
                };
                for (size_t i = 0; i < sizeof GCL / sizeof *GCL; i++)
                    if (pd->b & GCL[i].bit) s.button |= GCL[i].gc;
                s.trigger_l = pd->lt, s.trigger_r = pd->rt;
                b &= ~(pd->b & (HidNpadButton_A | HidNpadButton_B | HidNpadButton_X | HidNpadButton_Y | HidNpadButton_ZR | HidNpadButton_L | HidNpadButton_R | HidNpadButton_Plus));
            }
            for (int i = 0; i < GC_COUNT; i++)
                if (b & map[i]) s.button |= GC_BITS[i];
            int dx = (b & HidNpadButton_Right ? 1 : 0) - (b & HidNpadButton_Left ? 1 : 0);
            int dy = (b & HidNpadButton_Up ? 1 : 0) - (b & HidNpadButton_Down ? 1 : 0);
            if (dx || dy) {
                int m = dx && dy ? 72 : 100;
                s.stick_x = (int8_t)(dx * m), s.stick_y = (int8_t)(dy * m);
            }
            if (b & resume) s.button |= PAD_START;
            if (MOD_IS_RTGP(g_mod) && mod_key(g_mod)[4] && !charpack_select_active())
                s.button |= (s.button & PAD_X ? PAD_DOWN : 0) | (s.button & PAD_Y ? PAD_UP : 0);
            static bool start_was[4];
            bool start = s.button & PAD_START;
            if (start && !start_was[p]) g_pause_kind = PAUSE_FULL;
            start_was[p] = start;
            float pan = s.substick_x / 100.0f;
            g_cam_pan[p] = ps.free_cam && (pan > 0.3f || pan < -0.3f) ? (pan > 1 ? 1 : pan < -1 ? -1 : pan) : 0;
            if (charpack_select_active() || pause_art_random_shown()) {
                if (b & HidNpadButton_L) s.button |= PAD_TRIGGER_L;
                if (b & HidNpadButton_R) s.button |= PAD_TRIGGER_R;
            }
            static u64 page_was[4];
            u64 page = b & (HidNpadButton_X | HidNpadButton_Y), pdown = page & ~page_was[p];
            page_was[p] = page;
            if (charpack_pages() > 1 && charpack_select_active()) {
                if (pdown & HidNpadButton_X) __atomic_or_fetch(&g_char_cycle, 1u << p, __ATOMIC_RELEASE);
                else if (pdown & HidNpadButton_Y) __atomic_or_fetch(&g_char_cycle, 1u << (4 + p), __ATOMIC_RELEASE);
            }
            /* analog triggers: a GameCube pad's own, else full press from a mapped button */
            if ((s.button & PAD_TRIGGER_R) && !s.trigger_r) s.trigger_r = 255;
            if ((s.button & PAD_TRIGGER_L) && !s.trigger_l) s.trigger_l = 255;
        }
        /* DRIVER FPV / KART FPV buttons (as main_switch.c) */
        {
            static bool was_fp[4], was_kfp[4];
            u64 pb = have && !menu && drive == 1 ? b : 0;
            bool on = pb & map[MAP_FP], kon = pb & map[MAP_KFP];
            u32 bit = 1u << p;
            bool fp = g_first_person & bit, kart = g_fp_kart & bit;
            if (on && !was_fp[p]) {
                if (p == 0) g_set.camera = g_set.camera == 1 ? 0 : 1;
                else if (fp && !kart) __atomic_and_fetch(&g_first_person, ~bit, __ATOMIC_RELAXED);
                else __atomic_or_fetch(&g_first_person, bit, __ATOMIC_RELAXED), __atomic_and_fetch(&g_fp_kart, ~bit, __ATOMIC_RELAXED);
            } else if (kon && !was_kfp[p]) {
                if (p == 0) g_set.camera = g_set.camera == 2 ? 0 : 2;
                else if (fp && kart) __atomic_and_fetch(&g_first_person, ~bit, __ATOMIC_RELAXED), __atomic_and_fetch(&g_fp_kart, ~bit, __ATOMIC_RELAXED);
                else __atomic_or_fetch(&g_first_person, bit, __ATOMIC_RELAXED), __atomic_or_fetch(&g_fp_kart, bit, __ATOMIC_RELAXED);
            }
            was_fp[p] = on, was_kfp[p] = kon;
        }
        /* REAR VIEW (GameCube Y while the race camera runs) */
        {
            static uint32_t last_frames[4], idle[4];
            static bool ours[4], theirs[4];
            uint32_t f = g_cam_frames[p];
            idle[p] = f != last_frames[p] ? 0 : idle[p] < 100 ? idle[p] + 1 : idle[p];
            last_frames[p] = f;
            bool steal = idle[p] < 4;
            if (!(s.button & PAD_Y)) ours[p] = theirs[p] = false;
            else if (!ours[p] && !theirs[p]) ours[p] = steal, theirs[p] = !steal;
            uint32_t bit = 1u << p;
            g_look_back = ours[p] && steal ? g_look_back | bit : g_look_back & ~bit;
            if (ours[p]) s.button &= ~PAD_Y;
        }
        pad_set(p, &s);
    }
    if (g_padplay) jump = (jump & ~1u) | (g_jump_held & 1u), trick = (trick & ~(15u | 1u << TRICK_SIG_BIT)) | (g_trick_pad & (15u | 1u << TRICK_SIG_BIT)); /* padrec.c's */
    g_jump_held = jump;
    g_trick_pad = trick;
    g_cam_reset = cam_reset;
}

static void game_thread(void *arg) {
    (void)arg;
    sched_note_host();
    machine_run(&s_boot);
}

/* The game's render scale: DOCKED RESOLUTION (the menu's TV setting); AUTO fits the window height */
static float render_scale(int h) {
    if (g_set.docked_res == DOCKED_AUTO) {
        float s = floorf((float)h / 480.0f * 4.0f) / 4.0f;
        return s < 1.0f ? 1.0f : s > 4.0f ? 4.0f : s;
    }
    static const float SCALE[] = {[DOCKED_NATIVE] = 1.0f, [DOCKED_2X] = 2.0f, [DOCKED_1_5X] = 1.5f, [DOCKED_3X] = 3.0f, [DOCKED_4X] = 4.0f};
    return SCALE[g_set.docked_res];
}

static unsigned subst_texture(unsigned id) { return id >= GX_SUBST_CHARPACK ? charpack_art_texture(id) : pause_art_texture(id); }

/* the folder of the .exe becomes the working directory (the disc and the folders are there) */
static void go_home(void) {
    char p[MAX_PATH];
    DWORD n = GetModuleFileNameA(NULL, p, sizeof p);
    if (!n || n >= sizeof p) return;
    char *s = strrchr(p, '\\');
    if (s) *s = 0, SetCurrentDirectoryA(p);
}

/* RESTART NOW (MODS, LAN): menu.c quits the game, and once everything is closed and flushed main()
 * starts this .exe again with the same arguments plus --play (no launcher), from the same folder */
static bool s_relaunch;
bool update_relaunch(void) { s_relaunch = true; return true; }
static void relaunch(void) {
    wchar_t exe[MAX_PATH], cmd[32768];
    DWORD n = GetModuleFileNameW(NULL, exe, MAX_PATH);
    const wchar_t *line = GetCommandLineW();
    if (!n || n >= MAX_PATH || !line || wcslen(line) + 8 >= sizeof cmd / sizeof *cmd) { rt_log("restart: no command line"); return; }
    swprintf(cmd, sizeof cmd / sizeof *cmd, L"%ls --play", line);
    STARTUPINFOW si = {.cb = sizeof si};
    PROCESS_INFORMATION pi;
    if (!CreateProcessW(exe, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) { rt_log("restart: CreateProcess failed (%lu)", GetLastError()); return; }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
}

int main(int argc, char **argv) {
    /* MKDD-NX.exe <disc image>: that one instead of the one next to the .exe (made absolute first) */
    /* --launcher: show the launcher (as holding Shift at start does), --play: skip it,
     * --kbd-players N: the keyboard plays as players 1-N too (testing split screen),
     * --hud-log N: log the race HUD's pictures at race frame N (mkdd.log, "hudlog") */
    static char arg_disc[MAX_PATH];
    bool want_launcher = GetAsyncKeyState(VK_SHIFT) < 0, no_launcher = false;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--launcher")) want_launcher = true;
        else if (!strcmp(argv[i], "--play")) no_launcher = true;
        else if (!strcmp(argv[i], "--kbd-players") && i + 1 < argc) {
            int n = atoi(argv[++i]);
            s_kbd_players = n < 1 ? 1 : n > 4 ? 4 : n;
        }
        else if (!strcmp(argv[i], "--hud-log") && i + 1 < argc) {
            int n = atoi(argv[++i]);
            g_hud_log = n > 0 ? (uint32_t)n : 0;
        }
        else if (!GetFullPathNameA(argv[i], sizeof arg_disc, arg_disc, NULL)) arg_disc[0] = 0;
    }
    go_home();
    SetUnhandledExceptionFilter(crash_handler);
    timeBeginPeriod(1);
    update_init(NULL);
    _mkdir(LOG_DIR), _mkdir(CFG_DIR), _mkdir(SAVE_DIR), _mkdir(MODS_DIR), _mkdir(MODS_DIR "/characters"), _mkdir(MODS_DIR "/karts");
    s_boot_tick = armGetSystemTick();
    static const char *const LOGS[][2] = {
        {LOG_DIR "/stderr.log", LOG_DIR "/stderr.prev.log"}, {LOG_DIR "/crash.log", LOG_DIR "/crash.prev.log"},
        {LOG_DIR "/mkdd.log", LOG_DIR "/mkdd.prev.log"}, {LOG_DIR "/mkdd_trace.log", LOG_DIR "/mkdd_trace.prev.log"},
    };
    for (size_t i = 0; i < sizeof LOGS / sizeof *LOGS; i++) {
        remove(LOGS[i][1]);
        rename(LOGS[i][0], LOGS[i][1]);
    }
    FILE *bt = fopen(LOG_DIR "/boot.txt", "w");
    if (bt) { fprintf(bt, "main() reached, build " __DATE__ " " __TIME__ "\n"); fclose(bt); }
    freopen(LOG_DIR "/stderr.log", "w", stderr);
    setvbuf(stderr, NULL, _IOFBF, 64 << 10);
    diag_set_log_file(LOG_DIR "/mkdd.log");
    rt_log("MKDD-NX %s (Windows x64), build %s", update_version(), g_build_stamp);
    memcard_set_path(SAVE_DIR "/slotA.bin");
    settings_load(CFG_DIR "/settings.ini");
    color_load(CFG_DIR "/color.ini");
    wincfg_load();
    if (g_win.auto_update) update_start(); /* only looks: the launcher or the menu's UPDATE installs */
    /* players 2-4's driving buttons from 1.8.4's launcher (windows.ini p2_map ..): theirs in settings.ini now */
    for (int i = 0; i < 3; i++)
        if (g_win.pmap_set[i]) {
            if (!g_set.pl[i].own) {
                player_set(0, &g_set.pl[i]);
                g_set.pl[i].own = true;
                memcpy(g_set.pl[i].map, g_win.pmap[i], sizeof g_set.pl[i].map);
                rt_log("settings: player %d's buttons moved from windows.ini", i + 2);
            }
            g_win.pmap_set[i] = false;
            settings_save(), wincfg_save();
        }
    pad_init();
    if ((g_win.launcher || want_launcher) && !no_launcher && !launcher_run()) {
        pad_quit();
        diag_flush();
        return 0;
    }
    g_lan_on = g_set.lan;
    snprintf(g_lan_peer, sizeof g_lan_peer, "%s", g_set.lan_peer);

    if (!gfx_open(g_win.win_w, g_win.win_h)) {
        MessageBoxA(NULL, "Could not open the window (see logs/mkdd.log).", APP_TITLE, MB_ICONERROR);
        return 1;
    }
    rt_log("%s", gfx_describe());

    char disc[512], err[512];
    menu_font_load(); /* (nothing until the disc is open: the loading screen draws its own letters) */
    screen("LOOKING FOR THE DISC IMAGE", NULL, 0.05f);
    if (arg_disc[0]) snprintf(disc, sizeof disc, "%s", arg_disc), rt_log("disc: using %s (command line)", disc);
    else if (g_win.disc[0] && access(g_win.disc, R_OK) == 0) snprintf(disc, sizeof disc, "%s", g_win.disc), rt_log("disc: using %s (launcher)", disc);
    else if (!find_disc(".", disc, sizeof disc)) fatal("NO DISC IMAGE FOUND", "PUT YOUR GM4E01 .ISO OR .RVZ NEXT TO MKDD-NX.EXE");
    if (disc_open(disc, err, sizeof err)) {
        menu_font_load();
        trickpak_install(); /* the Trick Style files, on the first start of a new pack (runtime/trickpak.c) */
        static const ModsHost host = {MODS_DIR, SAVE_DIR, screen, offer_save_copy, NULL};
        s_mod_key = mods_setup(&host); /* ../switch/mods_setup.c */
        render_set_subst(subst_texture);
        render_set_below(pause_art_below);
        render_set_shift(pause_art_shift);
    } else {
        rt_log("disc: %s", err);
        fatal("COULD NOT READ THE DISC IMAGE", strstr(err, "compression") ? "RVZ: CONVERT IT AGAIN IN DOLPHIN WITH ZSTANDARD"
                                                                         : "THE FILE IS DAMAGED: SEE LOGS/MKDD.LOG");
    }
    screen("STARTING", NULL, 0.3f);
    if (!render_init()) fatal("GRAPHICS INIT FAILED", "SEE LOGS/MKDD.LOG");
    hdtex_open(HD_PATH);
    render_shader_cache(CFG_DIR "/shaders.bin", shader_progress);
    if (!machine_boot(disc, &s_boot, err, sizeof err)) {
        rt_log("boot failed: %s", err);
        if (disc_overlay_active()) fatal("THIS VERSION OF THE MOD IS NOT SUPPORTED", "SEE LOGS/MKDD.LOG FOR THE SUPPORTED VERSIONS");
        fatal("COULD NOT START THE GAME", "WRONG DISC: NEEDS MARIO KART DOUBLE DASH USA (GM4E01)");
    }
    if (s_mod_key && (!g_mod || strcmp(MODS[g_mod - 1].key, s_mod_key) != 0)) fatal("THE MOD FOLDER HOLDS ANOTHER MOD", "SEE LOGS/MKDD.LOG");
    diag_start_watchdog(LOG_DIR "/mkdd_trace.log");
    cheats_set(g_set.unlock_all);
    pthread_start("audio", audio_thread, NULL, 256u << 10, -1, 0);
    update_pads();
    g_gx_cpu_xform = access(CFG_DIR "/cpu_xform", F_OK) == 0;
    g_gx_vcache = access(CFG_DIR "/novcache", F_OK) != 0;
    g_gx_vjobs = access(CFG_DIR "/novjobs", F_OK) != 0;
    rt_log("xf: vertex transform on the %s", g_gx_cpu_xform ? "CPU (cpu_xform file)" : g_gx_cpu_xform_forced ? "CPU (shader failed)" : "GPU");
    pthread_start("game", game_thread, NULL, 32u << 20, -1, 0);
    screen("BOOTING", NULL, 0.6f);

    unsigned frames = 0, total_rendered = 0, fps_frames = 0;
    u64 fps_tick = armGetSystemTick(), stat_tick = fps_tick;
    char perf[96] = "";
    uint64_t pf_idle = g_guest_idle_ns, pf_gx = g_gx_busy_ns, pf_main = 0, pf_loops = 0;
    while (!s_closed) {
        update_pads();
        if (s_quit) break;
        bool paused = menu_is_open();
        g_pad_hold = paused && !g_lan_play;
        rumble_update(paused);
        if (menu_take_peer()) menu_open("LAN PEER: SET IT IN THE LAUNCHER (HOLD SHIFT AT START)");
        (void)menu_take_controllers(); /* (no controller screen on Windows) */
        if (menu_take_fullscreen()) toggle_fullscreen();
        bool title = m_read_be(0x803C9CE0u, 4) == 0, wide = g_set.wide && !title;
        g_aspect_scale = wide ? 4.0f / 3.0f : 1.0f;
        render_set_backdrop(g_set.wide && title);
        static const float FOV_SCALE[5] = {1.0f, 1.15f, 1.3f, 1.45f, 2.0f};
        g_fov_scale = FOV_SCALE[g_set.fov[g_set.wide] % 5];
        g_hud_hide = g_set.hud_hide & (HUD_ALL | HUD_COINS);
        g_hud_modern = g_set.hud_style >= 1;
        g_hud_pills = g_set.hud_style >= 2;
        g_hud_mirror = g_set.hud_style == 2 && !(g_set.hud_hide & HUD_MIRROR);
        g_speed_kmh = g_set.speed_kmh;
        g_vsplit = g_set.vsplit;
        g_any_kart = g_lan_play ? 0 : g_set.any_kart;
        g_laps = g_lan_play ? 0 : (uint32_t)g_set.laps;
        cheats_apply();
        camera_apply(); /* each player's CAMERA (menu.c) */
        g_fp_up = (float)g_set.drv_up, g_fp_fwd = (float)g_set.drv_forward;
        g_fp_tilt = (float)(g_set.drv_tilt < -45 ? -45 : g_set.drv_tilt > 45 ? 45 : g_set.drv_tilt) * 0.017453293f;
        g_kart_eye_on = 1;
        g_kart_up = (float)g_set.kart_up, g_kart_fwd = (float)g_set.kart_forward;
        g_kart_tilt = (float)(g_set.kart_tilt < -45 ? -45 : g_set.kart_tilt > 45 ? 45 : g_set.kart_tilt) * 0.017453293f;
        for (int i = 0; i < 21; i++) {
            g_kart_add[i][0] = (float)g_set.kart_add[i][0], g_kart_add[i][1] = (float)g_set.kart_add[i][1];
            g_kart_add[i][2] = (float)g_set.kart_add[i][2] * 0.017453293f;
        }
        render_set_hud43(wide);
        g_hud_wide = wide;
        render_set_hd(g_set.hd_textures);
        static const float ANISO[5] = {1, 2, 4, 8, 16};
        render_set_aniso(ANISO[g_set.aniso % 5]);
        /* the screen follows the window */
        int ww, wh;
        win_client_size(&ww, &wh);
        gfx_resize(ww, wh);
        int w, h;
        gfx_size(&w, &h);
        float scale = render_scale(h);
        {
            static int last_w, last_h, last_scale;
            if (w != last_w || h != last_h || (int)(scale * 100) != last_scale) {
                rt_log("display: window %dx%d, game rendered at %.2fx", w, h, (double)scale);
                last_w = w, last_h = h, last_scale = (int)(scale * 100);
            }
        }
        u64 r0 = armGetSystemTick();
        if (g_log_mask) render_gpu_timer_begin();
        bool got = render_update(scale);
        if (got) {
            fps_frames++;
            if (++total_rendered == 1) {
                rt_log("first game frame at %.1f s", (double)armTicksToNs(armGetSystemTick() - s_boot_tick) / 1e9);
                g_log_boot = false;
                diag_flush();
            }
        }
        u64 fps_ns = armTicksToNs(armGetSystemTick() - fps_tick);
        if (fps_ns >= 500000000ull) {
            s_fps = (float)((double)fps_frames * 1e9 / (double)fps_ns);
            if (fps_frames) {
                uint64_t idle = g_guest_idle_ns - pf_idle, gx = g_gx_busy_ns - pf_gx;
                double game = idle < fps_ns ? (double)(fps_ns - idle) : 0.0;
                snprintf(perf, sizeof perf, "GAME %.1f  GX %.1f  RENDER %.1f MS", game / fps_frames / 1e6, (double)gx / fps_frames / 1e6,
                         pf_loops ? (double)pf_main / pf_loops / 1e6 : 0.0);
            }
            pf_idle = g_guest_idle_ns, pf_gx = g_gx_busy_ns, pf_main = pf_loops = 0, fps_frames = 0;
            fps_tick = armGetSystemTick();
        }
        render_gpu_timer_mid();
        if (total_rendered) {
            float adj[3];
            color_adjust(adj);
            render_present(w, h, adj, wide);
            static uint32_t race_seen;
            static u64 race_tick;
            if (g_race_frames != race_seen) { race_seen = g_race_frames; race_tick = armGetSystemTick(); }
            static bool racing;
            if (!menu_is_open()) racing = race_tick && armTicksToNs(armGetSystemTick() - race_tick) < 500000000ull;
            if (g_set.border && !wide && !title && racing) menu_border(w, h);
            if (!menu_is_open() && charpack_select_active()) menu_char_hint(w, h);
            menu_trick_pick(w, h);
            menu_trick_guide(w, h);
            menu_draw(w, h, s_fps, perf);
        } else menu_loading(w, h, "BOOTING", NULL, 0.8f);
        render_gpu_timer_end();
        pf_main += armTicksToNs(armGetSystemTick() - r0);
        pf_loops++;
        gfx_swap();
        if (++frames % 300 == 0) {
            u64 now = armGetSystemTick();
            uint32_t nb, nv, nt;
            render_stats(&nb, &nv, &nt);
            unsigned progs;
            double cms;
            render_shader_stats(&progs, &cms);
            rt_log("present: %.1f fps, last frame %u batches %u verts, %u textures; %u TEV programs (%.0f ms compiling), late audio buffers %llu",
                   300.0 * 1e9 / (double)armTicksToNs(now - stat_tick), nb, nv, nt, progs, cms, (unsigned long long)g_audio_late_cb);
            double g_avg, g_max, g_img, g_out;
            if (render_gpu_timer_take(&g_avg, &g_max, &g_img, &g_out)) rt_log("gpu: %.1f ms per loop (game image %.1f, output %.1f; max %.1f)", g_avg, g_img, g_out, g_max);
            stat_tick = now;
            /* (the watchdog thread writes and flushes the log) */
        }
    }
    rt_log("exit: %s", s_relaunch ? "menu RESTART NOW" : s_quit ? "menu EXIT GAME" : "window closed");
    rumble_off();
    padrec_close();
    pad_quit();
    gx_stop_gpu();
    diag_flush();
    gfx_close();
    eth_exit();
    plat_net_exit();
    if (s_relaunch) relaunch(); /* (logs it, so before the last flush) */
    diag_flush();
    fflush(stderr);
    _exit(0);
}
