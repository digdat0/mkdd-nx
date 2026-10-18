/* Nintendo Switch front end: SD folder + logs, crash.log, loading screen, disc lookup, boot,
 * render loop (GLES via SDL2 + Mesa), controller input (libnx HID) and audio (SDL2).
 * Platform glue adapted from the gcNX template (gc-decomp-switch-port skill). */
#include <dirent.h>
#include <malloc.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include <switch.h>
#include <SDL.h>
#include <GLES3/gl32.h>

#include "../runtime/gx.h"
#include "../runtime/rt.h"
#include "render_gl.h"
#include "prof_switch.h"
#include "menu.h"
#include "pause_art.h"
#include "hdtex.h"
#include "update.h"
const char g_build_stamp[] = __DATE__ " " __TIME__;
#include "../runtime/native.h"

#define APP_DIR "sdmc:/switch/MKDD-NX"
/* The app folder holds only the .nro, the disc image and these three folders. */
#define LOG_DIR APP_DIR "/logs"
#define CFG_DIR APP_DIR "/config"
#define SAVE_DIR APP_DIR "/save"
#define APP_TITLE "MARIO KART DOUBLE DASH"

static SDL_Window *s_window;
static u64 s_boot_tick;
static Ctx s_boot;

/* ---- crash.log: registers + frame-pointer backtrace as image offsets (scripts/symbolize.py) ---- */
u32 __nx_exception_ignoredebug = 1;
alignas(16) u8 __nx_exception_stack[0x8000];
u64 __nx_exception_stack_size = sizeof(__nx_exception_stack);

static void image_range(u64 *base, u64 *end) {
    extern char __end__[];
    MemoryInfo mi;
    u32 pi;
    *base = 0;
    if (R_SUCCEEDED(svcQueryMemory(&mi, &pi, (u64)(uintptr_t)&image_range))) *base = mi.addr;
    *end = (u64)(uintptr_t)__end__;
}

void __libnx_exception_handler(ThreadExceptionDump *ctx) {
    u64 base, end;
    image_range(&base, &end);
    FILE *f = fopen(LOG_DIR "/crash.log", "w");
    if (!f) return;
#define OFF(v) ((v) >= base && (v) < end ? (v) - base : 0)
    fprintf(f, "MKDD-NX crash\nexception 0x%x, esr 0x%x\nimage: 0x%llx-0x%llx\nlast guest fn: %#010x %s\n", ctx->error_desc,
            ctx->esr, (unsigned long long)base, (unsigned long long)end, g_last_fn, m_name(g_last_fn));
    fprintf(f, "PC  0x%016llx  image+0x%llx\nLR  0x%016llx  image+0x%llx\nSP  0x%016llx\nFP  0x%016llx\nFAR 0x%016llx\n",
            (unsigned long long)ctx->pc.x, (unsigned long long)OFF(ctx->pc.x), (unsigned long long)ctx->lr.x,
            (unsigned long long)OFF(ctx->lr.x), (unsigned long long)ctx->sp.x, (unsigned long long)ctx->fp.x,
            (unsigned long long)ctx->far.x);
    for (int i = 0; i < 29; i++) fprintf(f, "X%-2d 0x%016llx%s", i, (unsigned long long)ctx->cpu_gprs[i].x, (i % 2) ? "\n" : "   ");
    {
        /* what the faulting address is: unmapped, or which kind of memory */
        MemoryInfo mi = {0};
        u32 pi = 0;
        if (R_SUCCEEDED(svcQueryMemory(&mi, &pi, ctx->far.x)))
            fprintf(f, "\nFAR region 0x%llx+0x%llx type 0x%x attr 0x%x perm 0x%x", (unsigned long long)mi.addr,
                    (unsigned long long)mi.size, mi.type, mi.attr, mi.perm);
        fprintf(f, "\nguest RAM 0x%llx ARAM 0x%llx", (unsigned long long)(uintptr_t)g_ram, (unsigned long long)(uintptr_t)g_aram);
    }
    fprintf(f, "\nbacktrace (image offsets):\n");
    u64 *fp = (u64 *)ctx->fp.x;
    for (int i = 0; i < 48 && fp && ((u64)fp & 7) == 0 && (u64)fp >= ctx->sp.x && (u64)fp < ctx->sp.x + 0x4000000; i++) {
        fprintf(f, "  #%-2d image+0x%llx\n", i, (unsigned long long)OFF(fp[1]));
        if ((u64 *)fp[0] <= fp) break;
        fp = (u64 *)fp[0];
    }
#undef OFF
    fclose(f);
}

/* ---- clocks (CPU / GPU / memory, MHz), for the log ---- */
static bool s_clk_ok;
static ClkrstSession s_clk[3];
static void clocks_init(void) {
    if (!hosversionAtLeast(8, 0, 0) || R_FAILED(clkrstInitialize())) return;
    static const PcvModuleId ids[3] = {PcvModuleId_CpuBus, PcvModuleId_GPU, PcvModuleId_EMC};
    s_clk_ok = true;
    for (int i = 0; i < 3; i++) s_clk_ok &= R_SUCCEEDED(clkrstOpenSession(&s_clk[i], ids[i], 3));
}
static void clocks_log(const char *why) {
    if (!s_clk_ok) return;
    u32 hz[3] = {0};
    for (int i = 0; i < 3; i++) clkrstGetClockRate(&s_clk[i], &hz[i]);
    rt_log("clocks (%s): CPU %u MHz, GPU %u MHz, memory %u MHz", why, hz[0] / 1000000, hz[1] / 1000000, hz[2] / 1000000);
}

/* ---- loading / message screen (menu.c draws it in the menu's style) ---- */
static void screen(const char *line1, const char *line2, float progress) {
    int w = 0, h = 0;
    SDL_GL_GetDrawableSize(s_window, &w, &h);
    menu_loading(w, h, line1, line2, progress);
    SDL_GL_SwapWindow(s_window);
    SDL_PumpEvents(); /* runs appletMainLoop: HOME during a loading screen must not hang the console */
}

/* Show a message until + is pressed, then exit. */
static void shader_progress(unsigned done, unsigned total) {
    char line[64];
    snprintf(line, sizeof line, "PREPARING SHADERS %u / %u", done, total);
    screen(line, NULL, 0.3f + 0.25f * (float)done / (float)(total ? total : 1));
    SDL_PumpEvents(); /* HOME during a loading screen must not hang the console */
}

static void fatal(const char *line1, const char *line2) {
    printf("[NX] fatal: %s / %s\n", line1, line2 ? line2 : "");
    PadState pad;
    padInitializeDefault(&pad);
    while (appletMainLoop()) {
        padUpdate(&pad);
        if (padGetButtonsDown(&pad) & HidNpadButton_Plus) break;
        screen(line1, line2 ? line2 : "PRESS + TO EXIT", -1);
    }
    SDL_Quit();
    exit(0);
}

/* The disc image in the app folder. With several, the fastest to read wins: .iso / .gcm, then
 * .ciso, then .rvz (the same game either way: each is checked against the supported main.dol). */
static bool find_disc(char *out, size_t n) {
    static const char *EXT[] = {".iso", ".gcm", ".ciso", ".rvz"};
    static const int RANK[] = {0, 0, 1, 2};
    DIR *d = opendir(APP_DIR);
    if (!d) return false;
    struct dirent *e;
    int best = 99;
    while ((e = readdir(d))) {
        const char *dot = strrchr(e->d_name, '.');
        for (int i = 0; dot && i < 4; i++)
            if (!strcasecmp(dot, EXT[i]) && RANK[i] < best) {
                snprintf(out, n, APP_DIR "/%s", e->d_name);
                best = RANK[i];
            }
    }
    closedir(d);
    if (best < 99) rt_log("disc: using %s", out);
    return best < 99;
}

/* ---- audio: SDL pulls from the AI queue, resampled from the game rate (32/48 kHz) ---- */
static double s_apos;
static int16_t s_aprev[2], s_acur[2];
uint64_t g_audio_late_cb; /* SDL callbacks more than half a buffer late (the device ran short) */
static void audio_cb(void *ud, Uint8 *stream, int len) {
    (void)ud;
    int16_t *out = (int16_t *)stream;
    int frames = len / 4;
    /* SDL starts this thread on the main (render) core at its priority, so heavy frames (item
     * effects) delayed it and the device played gaps. Above the renderer and the guest threads;
     * a callback takes microseconds. */
    static u64 last;
    if (!last) {
        Result rc = svcSetThreadPriority(CUR_THREAD_HANDLE, 0x29);
        rt_log("audio: callback thread on core %d, priority 0x29 %s, %d frames", svcGetCurrentProcessorNumber(),
               R_SUCCEEDED(rc) ? "set" : "refused", frames);
    }
    u64 now = armGetSystemTick();
    if (last && armTicksToNs(now - last) > (u64)frames * 1000000000ull / 48000 * 3 / 2) g_audio_late_cb++;
    last = now;
    double step = (double)audio_rate() / 48000.0 * audio_rate_trim();
    for (int i = 0; i < frames; i++) {
        s_apos += step;
        while (s_apos >= 1.0) {
            s_apos -= 1.0;
            s_aprev[0] = s_acur[0];
            s_aprev[1] = s_acur[1];
            audio_pop(s_acur, 1);
        }
        out[2 * i] = (int16_t)(s_aprev[0] + (s_acur[0] - s_aprev[0]) * s_apos);
        out[2 * i + 1] = (int16_t)(s_aprev[1] + (s_acur[1] - s_aprev[1]) * s_apos);
    }
}

/* ---- input: up to 4 Switch controllers -> GameCube pads 1-4 ----
 * Sources in a fixed order (handheld, then controllers 1-4 as the system numbers them); the
 * connected ones become players 1, 2, ... so handheld + a Pro Controller is a 2-player game.
 * Full controllers (handheld, Pro, Joy-Con pair) use the mapping from the - menu; a single
 * Joy-Con held sideways and a GameCube controller (official adapter) have fixed layouts. */
#define NSRC 5
static PadState s_src[NSRC];
static const HidNpadIdType SRC_ID[NSRC] = {HidNpadIdType_Handheld, HidNpadIdType_No1, HidNpadIdType_No2,
                                          HidNpadIdType_No3, HidNpadIdType_No4};
static int s_player_src[4] = {-1, -1, -1, -1}; /* source of each player, -1 = none */
static int s_pad_quiet; /* frames to ignore disconnects for (the controller screen reassigns players) */
/* runtime/native.c: Race2D::draw calls (the race HUD, once per race frame). Declared here, not in
 * native.h, which every generated file includes. */
extern volatile uint32_t g_race_frames;
static u32 s_player_style[4];
static bool s_quit;
static float s_fps;

static int8_t stick(s32 v) { /* +-32767 -> +-100 (a GameCube stick's usable range) */
    int x = v * 100 / 32767;
    return (int8_t)(x > 127 ? 127 : x < -128 ? -128 : x);
}
static const char *style_name(u32 st) {
    if (st & HidNpadStyleTag_NpadHandheld) return "handheld";
    if (st & HidNpadStyleTag_NpadGc) return "GameCube controller";
    if (st & HidNpadStyleTag_NpadFullKey) return "Pro Controller";
    if (st & HidNpadStyleTag_NpadJoyDual) return "Joy-Con pair";
    if (st & HidNpadStyleTag_NpadJoyLeft) return "left Joy-Con (sideways)";
    if (st & HidNpadStyleTag_NpadJoyRight) return "right Joy-Con (sideways)";
    return "controller";
}

static void input_init(void) {
    padConfigureInput(4, HidNpadStyleSet_NpadStandard | HidNpadStyleTag_NpadGc);
    hidSetNpadJoyHoldType(HidNpadJoyHoldType_Horizontal); /* single Joy-Cons are held sideways */
    for (int i = 0; i < NSRC; i++) padInitialize(&s_src[i], SRC_ID[i]);
}

/* ---- rumble (PADControlMotor -> HD Rumble of that player's controller) ---- */
static HidVibrationDeviceHandle s_vib[4][2];
static int s_vib_n[4];
static u32 s_vib_style[4];
static int s_vib_src[4] = {-1, -1, -1, -1};
static float s_vib_amp[4];

static bool s_vib_gc[4]; /* GameCube controller: its motor takes on/off commands */
static void rumble_send(int p, float amp) {
    if (s_vib_n[p] <= 0 || amp == s_vib_amp[p]) return;
    if (s_vib_gc[p]) {
        hidSendVibrationGcErmCommand(s_vib[p][0], amp > 0 ? HidVibrationGcErmCommand_Start : HidVibrationGcErmCommand_Stop);
        s_vib_amp[p] = amp;
        return;
    }
    HidVibrationValue v[2];
    for (int k = 0; k < 2; k++) v[k] = (HidVibrationValue){amp, 160.0f, amp, 320.0f};
    hidSendVibrationValues(s_vib[p], v, s_vib_n[p]);
    s_vib_amp[p] = amp;
}
static void rumble_update(bool paused) {
    static const float AMP[3] = {0.0f, 0.35f, 0.7f};
    unsigned motors = pad_motor_mask();
    for (int p = 0; p < 4; p++) {
        int src = s_player_src[p];
        u32 st = s_player_style[p];
        if (src != s_vib_src[p] || st != s_vib_style[p]) { /* (re)attach the player's motors */
            rumble_send(p, 0.0f);
            s_vib_n[p] = 0;
            s_vib_src[p] = src;
            s_vib_style[p] = st;
            s_vib_amp[p] = 0.0f;
            u32 tag = st & HidNpadStyleTag_NpadGc ? HidNpadStyleTag_NpadGc
                      : st & HidNpadStyleTag_NpadHandheld ? HidNpadStyleTag_NpadHandheld
                      : st & HidNpadStyleTag_NpadFullKey ? HidNpadStyleTag_NpadFullKey
                      : st & HidNpadStyleTag_NpadJoyDual ? HidNpadStyleTag_NpadJoyDual
                      : st & HidNpadStyleTag_NpadJoyLeft ? HidNpadStyleTag_NpadJoyLeft
                      : st & HidNpadStyleTag_NpadJoyRight ? HidNpadStyleTag_NpadJoyRight : 0;
            s_vib_gc[p] = tag == HidNpadStyleTag_NpadGc;
            if (src >= 0 && tag) {
                int n = tag & (HidNpadStyleTag_NpadJoyLeft | HidNpadStyleTag_NpadJoyRight | HidNpadStyleTag_NpadGc) ? 1 : 2;
                if (R_SUCCEEDED(hidInitializeVibrationDevices(s_vib[p], n, SRC_ID[src], (HidNpadStyleTag)tag))) s_vib_n[p] = n;
            }
        }
        bool on = !paused && g_set.rumble && (motors >> p & 1);
        rumble_send(p, on ? AMP[g_set.rumble > 2 ? 2 : g_set.rumble] : 0.0f);
    }
}

/* Single Joy-Con held sideways: its four face buttons where a GameCube pad's A (right), B
 * (bottom), X (top) and Y (left) are; SL = L, SR = R, the top shoulder or the stick = Z. */
static void map_sideways(u64 b, bool left, bool p1, PadStatus *p) {
    const u64 A = left ? HidNpadButton_Up : HidNpadButton_B, B = left ? HidNpadButton_Right : HidNpadButton_Y;
    const u64 X = left ? HidNpadButton_Left : HidNpadButton_A, Y = left ? HidNpadButton_Down : HidNpadButton_X;
    if (b & A) p->button |= PAD_A;
    if (b & B) p->button |= PAD_B;
    if (b & X) p->button |= PAD_X;
    if (b & Y) p->button |= PAD_Y;
    if (b & HidNpadButton_AnySL) p->button |= PAD_TRIGGER_L;
    if (b & HidNpadButton_AnySR) p->button |= PAD_TRIGGER_R;
    if (b & (HidNpadButton_L | HidNpadButton_R | HidNpadButton_StickL | HidNpadButton_StickR)) p->button |= PAD_TRIGGER_Z;
    /* Start: + on the right Joy-Con; on the left one - (player 1's - opens the menu, so ZL) */
    if (b & (HidNpadButton_Plus | HidNpadButton_ZL | (p1 ? 0 : HidNpadButton_Minus))) p->button |= PAD_START;
}

static void map_full(PadState *pad, u64 b, PadStatus *p) {
    static const uint16_t GC_BITS[GC_COUNT] = {PAD_A, PAD_B, PAD_X, PAD_Y, PAD_TRIGGER_Z, PAD_TRIGGER_L, PAD_TRIGGER_R, PAD_START};
    for (int i = 0; i < GC_COUNT; i++)
        if (b & g_set.map[i]) p->button |= GC_BITS[i];
    HidAnalogStickState r = padGetStickPos(pad, 1);
    p->substick_x = stick(r.x);
    p->substick_y = stick(r.y);
    /* D-pad = control stick at full deflection; diagonals on the GameCube's octagonal gate */
    int dx = (b & HidNpadButton_Right ? 1 : 0) - (b & HidNpadButton_Left ? 1 : 0);
    int dy = (b & HidNpadButton_Up ? 1 : 0) - (b & HidNpadButton_Down ? 1 : 0);
    if (dx || dy) {
        int m = dx && dy ? 72 : 100;
        p->stick_x = (int8_t)(dx * m);
        p->stick_y = (int8_t)(dy * m);
    }
}

/* GameCube controller (official adapter): its own layout, analog L/R. */
static void map_gc(PadState *pad, u64 b, PadStatus *p) {
    if (b & HidNpadButton_A) p->button |= PAD_A;
    if (b & HidNpadButton_B) p->button |= PAD_B;
    if (b & HidNpadButton_X) p->button |= PAD_X;
    if (b & HidNpadButton_Y) p->button |= PAD_Y;
    if (b & HidNpadButton_ZR) p->button |= PAD_TRIGGER_Z;
    if (b & HidNpadButton_L) p->button |= PAD_TRIGGER_L;
    if (b & HidNpadButton_R) p->button |= PAD_TRIGGER_R;
    if (b & HidNpadButton_Plus) p->button |= PAD_START;
    HidAnalogStickState r = padGetStickPos(pad, 1);
    p->substick_x = stick(r.x);
    p->substick_y = stick(r.y);
    p->trigger_l = (uint8_t)(padGetGcTriggerPos(pad, 0) * 255 / 0x7FFF);
    p->trigger_r = (uint8_t)(padGetGcTriggerPos(pad, 1) * 255 / 0x7FFF);
}

static void update_pads(void) {
    int n = 0, src_of[4] = {-1, -1, -1, -1};
    for (int i = 0; i < NSRC; i++) {
        padUpdate(&s_src[i]);
        if (padIsConnected(&s_src[i]) && n < 4) src_of[n++] = i;
    }
    /* connect / disconnect / controller change, logged with the time (rt_log stamps it) */
    for (int p = 0; p < 4; p++) {
        u32 st = src_of[p] >= 0 ? padGetStyleSet(&s_src[src_of[p]]) : 0;
        if (src_of[p] != s_player_src[p] || st != s_player_style[p]) {
            if (src_of[p] < 0) {
                rt_log("pad: player %d disconnected (was %s)", p + 1, style_name(s_player_style[p]));
                /* a controller in use dropped: pause on the menu (always; reconnect, then close it) */
                if (s_player_src[p] >= 0 && !s_pad_quiet) {
                    char note[64];
                    snprintf(note, sizeof note, "PLAYER %d CONTROLLER DISCONNECTED", p + 1);
                    menu_open(note);
                }
            }
            else rt_log("pad: player %d connected: %s (source %d)", p + 1, style_name(st), src_of[p]);
            s_player_src[p] = src_of[p];
            s_player_style[p] = st;
        }
    }

    if (s_pad_quiet) s_pad_quiet--;
    /* The - menu belongs to player 1 (a single right Joy-Con: press its stick and +). */
    PadState *p1 = s_player_src[0] >= 0 ? &s_src[s_player_src[0]] : &s_src[0];
    u64 down1 = padGetButtonsDown(p1), held1 = padGetButtons(p1);
    if ((s_player_style[0] & HidNpadStyleTag_NpadJoyRight) && (held1 & HidNpadButton_StickR) && (down1 & HidNpadButton_Plus))
        down1 |= HidNpadButton_Minus;
    bool menu = menu_input(down1, held1, &s_quit);
    for (int p = 0; p < 4; p++) {
        PadStatus s = {0};
        int src = s_player_src[p];
        s.connected = src >= 0 || p == 0; /* port 1 is always there (the game expects a pad) */
        if (src >= 0 && !menu) {
            PadState *pad = &s_src[src];
            u64 b = padGetButtons(pad);
            u32 st = s_player_style[p];
            bool single = (st & (HidNpadStyleTag_NpadJoyLeft | HidNpadStyleTag_NpadJoyRight)) &&
                          !(st & (HidNpadStyleTag_NpadJoyDual | HidNpadStyleTag_NpadFullKey | HidNpadStyleTag_NpadHandheld));
            HidAnalogStickState l = padGetStickPos(pad, single && (st & HidNpadStyleTag_NpadJoyRight) ? 1 : 0);
            s.stick_x = stick(l.x);
            s.stick_y = stick(l.y);
            if (st & HidNpadStyleTag_NpadGc) map_gc(pad, b, &s);
            else if (single) map_sideways(b, (st & HidNpadStyleTag_NpadJoyLeft) != 0, p == 0, &s);
            else map_full(pad, b, &s);
            /* Start pressed: the pause screen shows this controller's buttons (pause_art.c) */
            static bool start_was[4];
            bool start = s.button & PAD_START;
            if (start && !start_was[p])
                g_pause_kind = st & HidNpadStyleTag_NpadGc ? PAUSE_GC
                               : single ? (st & HidNpadStyleTag_NpadJoyLeft ? PAUSE_LEFT : PAUSE_RIGHT) : PAUSE_FULL;
            start_was[p] = start;
            if ((s.button & PAD_TRIGGER_R) && !s.trigger_r) s.trigger_r = 255;
            if ((s.button & PAD_TRIGGER_L) && !s.trigger_l) s.trigger_l = 255;
        }
        pad_set(p, &s);
    }
}

static void game_thread(void *arg) {
    (void)arg;
    sched_note_host();
    machine_run(&s_boot);
}

int main(int argc, char **argv) {
    update_init(argc > 0 ? argv[0] : NULL);
    mkdir("sdmc:/switch", 0777);
    mkdir(APP_DIR, 0777);
    mkdir(LOG_DIR, 0777);
    mkdir(CFG_DIR, 0777);
    mkdir(SAVE_DIR, 0777);
    chdir(APP_DIR);
    s_boot_tick = armGetSystemTick();
    /* Keep the previous session's logs (newlib's rename does not replace an existing file). */
    static const char *const LOGS[][2] = {
        {LOG_DIR "/stderr.log", LOG_DIR "/stderr.prev.log"},     {LOG_DIR "/crash.log", LOG_DIR "/crash.prev.log"},
        {LOG_DIR "/mkdd.log", LOG_DIR "/mkdd.prev.log"},         {LOG_DIR "/mkdd_trace.log", LOG_DIR "/mkdd_trace.prev.log"},
        {LOG_DIR "/mkdd_prof.bin", LOG_DIR "/mkdd_prof.prev.bin"},
    };
    for (size_t i = 0; i < sizeof LOGS / sizeof *LOGS; i++) {
        FILE *f = fopen(LOGS[i][0], "rb");
        if (!f) continue;
        fclose(f);
        remove(LOGS[i][1]);
        rename(LOGS[i][0], LOGS[i][1]);
    }
    FILE *bt = fopen(LOG_DIR "/boot.txt", "w");
    if (bt) { fprintf(bt, "main() reached, build " __DATE__ " " __TIME__ "\n"); fclose(bt); }
    freopen(LOG_DIR "/stderr.log", "w", stderr);
    setvbuf(stderr, NULL, _IOFBF, 64 << 10);
    diag_set_log_file(LOG_DIR "/mkdd.log");
    memcard_set_path(SAVE_DIR "/slotA.bin");
    settings_load(CFG_DIR "/settings.ini");
    color_load(CFG_DIR "/color.ini");
    if (hosversionAtLeast(7, 0, 0)) appletSetCpuBoostMode(ApmCpuBoostMode_FastLoad);

    SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 2);
    /* 1920x1080 on the TV, 1280x720 in handheld; resizable so it follows docking (see the loop) */
    bool tv = appletGetOperationMode() == AppletOperationMode_Console;
    s_window = SDL_CreateWindow(APP_TITLE, 0, 0, tv ? 1920 : 1280, tv ? 1080 : 720, SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE);
    if (!s_window || !SDL_GL_CreateContext(s_window)) {
        fprintf(stderr, "[NX] window/GL context failed: %s\n", SDL_GetError());
        return 1;
    }
    SDL_GL_SetSwapInterval(1);
    rt_log("GL %s / %s / %s", glGetString(GL_VENDOR), glGetString(GL_RENDERER), glGetString(GL_VERSION));

    AppletType at = appletGetAppletType();
    if (at != AppletType_Application && at != AppletType_SystemApplication)
        fatal("APPLET MODE IS NOT SUPPORTED", "HOLD R WHILE STARTING A GAME (TITLE TAKEOVER)");

    input_init();

    char disc[512], err[512];
    screen("LOOKING FOR THE DISC IMAGE", NULL, 0.05f);
    if (!find_disc(disc, sizeof disc)) fatal("NO DISC IMAGE FOUND", "PUT YOUR GM4E01 .ISO OR .RVZ IN /SWITCH/MKDD-NX/");
    if (disc_open(disc, err, sizeof err)) {
        menu_font_load();  /* the loading screen's letters */
        pause_art_init();  /* the pause screen's controller picture, drawn as a Switch */
        render_set_subst(pause_art_texture);
    } else {
        rt_log("disc: %s", err);
        fatal("COULD NOT READ THE DISC IMAGE", strstr(err, "compression") ? "RVZ: CONVERT IT AGAIN IN DOLPHIN WITH ZSTANDARD"
                                                                         : "THE FILE IS DAMAGED: SEE LOGS/MKDD.LOG");
    }
    screen("STARTING", NULL, 0.3f);
    if (!render_init()) fatal("GRAPHICS INIT FAILED", "SEE MKDD.LOG");
    hdtex_open(APP_DIR "/" HD_FILE);
    render_shader_cache(CFG_DIR "/shaders.bin", shader_progress);
    if (!machine_boot(disc, &s_boot, err, sizeof err)) {
        rt_log("boot failed: %s", err);
        fatal("COULD NOT START THE GAME", "WRONG DISC: NEEDS MARIO KART DOUBLE DASH USA (GM4E01)");
    }
    diag_start_watchdog(LOG_DIR "/mkdd_trace.log");
    cheats_set(g_set.unlock_all); /* session only: the save never gets these bits */

    SDL_AudioSpec want = {0}, have;
    want.freq = 48000;
    want.format = AUDIO_S16SYS;
    want.channels = 2;
    want.samples = 1024;
    want.callback = audio_cb;
    SDL_AudioDeviceID adev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (adev) SDL_PauseAudioDevice(adev, 0);
    else rt_log("audio: %s", SDL_GetError());

    /* Handheld runs homebrew at the system default GPU clock (307 MHz); 0x92220008 is a retail
     * profile (CPU 1020, GPU 460.8, EMC 1331 MHz), 0x00020004 the fallback (GPU 384). Docked
     * already runs the GPU at 768 MHz. Restored at exit. */
    u32 perf_prev = 0;
    bool perf_set = false;
    if (R_SUCCEEDED(apmInitialize())) {
        if (R_SUCCEEDED(apmGetPerformanceConfiguration(ApmPerformanceMode_Normal, &perf_prev))) {
            perf_set = R_SUCCEEDED(apmSetPerformanceConfiguration(ApmPerformanceMode_Normal, 0x92220008)) ||
                       R_SUCCEEDED(apmSetPerformanceConfiguration(ApmPerformanceMode_Normal, 0x00020004));
        }
        rt_log("apm: handheld profile %s (was %#x)", perf_set ? "raised" : "unchanged", perf_prev);
        clocks_init();
    }

    update_pads();
    /* An empty file "cpu_xform" in the app folder switches the XF stage back to the decoder (CPU). */
    g_gx_cpu_xform = access(CFG_DIR "/cpu_xform", F_OK) == 0;
    rt_log("xf: vertex transform on the %s", g_gx_cpu_xform ? "CPU (cpu_xform file)" : g_gx_cpu_xform_forced ? "CPU (shader failed)" : "GPU");
    /* An empty file "novcache" in config/ turns the converted-vertex cache off (for comparisons). */
    g_gx_vcache = access(CFG_DIR "/novcache", F_OK) != 0;
    rt_log("gx: vertex cache %s", g_gx_vcache ? "on" : "off (novcache file)");
    /* Sampling profiler (mkdd_prof.bin, tools/prof_report.py): Debugging > Profiler. Off by default:
     * it pauses the game thread 500 times a second and shares the GX decoder's core. */
    if (g_set.profiler) {
        u64 ib, ie;
        image_range(&ib, &ie);
        prof_start(LOG_DIR "/mkdd_prof.bin", ib, ie);
    }
    pthread_start("game", game_thread, NULL, 32u << 20, 1, 0);
    screen("BOOTING", NULL, 0.6f);

    bool boost = true;
    unsigned frames = 0;
    u64 stat_tick = armGetSystemTick();
    unsigned rendered = 0, total_rendered = 0, fps_frames = 0;
    u64 fps_tick = armGetSystemTick();
    uint64_t t_render = 0, t_swap = 0, t_present = 0;
    /* performance overlay (- menu, SHOW FPS: FPS + TIMES): per game frame over the last 0.5 s */
    char perf[96] = "";
    uint64_t pf_idle = g_guest_idle_ns, pf_gx = g_gx_busy_ns, pf_main = 0, pf_loops = 0;
    while (appletMainLoop()) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_QUIT) goto out;
        }
        /* Out of focus (HOME menu) vi stops returning buffers, so a swap would block and the
         * close request never reach appletMainLoop: idle until focus comes back. */
        if (appletGetFocusState() != AppletFocusState_InFocus) {
            g_pad_hold = true; /* the game waits in PADRead until we're back */
            rumble_update(true);
            svcSleepThread(16000000);
            continue;
        }
        update_pads();
        if (s_quit) break;
        bool paused = menu_is_open();
        g_pad_hold = paused;
        rumble_update(paused);
        if (menu_take_controllers()) {
            /* the system's controller screen (pairing, player order, single Joy-Cons) */
            g_pad_hold = true;
            rumble_update(true);
            HidLaControllerSupportArg arg;
            hidLaCreateControllerSupportArg(&arg);
            arg.hdr.player_count_max = 4;
            HidLaControllerSupportResultInfo res;
            Result rc = hidLaShowControllerSupport(&res, &arg);
            rt_log("pad: controller screen closed (%#x), %d players", rc, R_SUCCEEDED(rc) ? res.player_count : -1);
            s_pad_quiet = 60;
        }
        /* SequenceApp::msScene (r13 0x803D1420 - 0x7740): 0 = SceneTitle, the "press start"
         * screen. Its artwork is one 4:3 picture that 16:9 only stretched, so it stays 4:3. */
        bool wide = g_set.wide && m_read_be(0x803C9CE0u, 4) != 0;
        g_aspect_scale = wide ? 4.0f / 3.0f : 1.0f;
        static const float FOV_SCALE[3] = {1.0f, 1.15f, 1.3f}; /* tan(fovy / 2) times: NORMAL, WIDE, WIDER */
        g_fov_scale = FOV_SCALE[g_set.fov[g_set.wide] % 3];
        render_set_hud43(wide);
        render_set_hd(g_set.hd_textures);
        static const float ANISO[5] = {1, 2, 4, 8, 16};
        render_set_aniso(ANISO[g_set.aniso % 5]);
        u64 r0 = armGetSystemTick();
        /* Docked: output 1920x1080 and (DOCKED RESOLUTION 2X) render the game at twice its size,
         * 1280x960 instead of 640x480. Handheld: 1280x720, the game's own size. */
        bool docked = appletGetOperationMode() == AppletOperationMode_Console;
        {
            static int last_w, last_h, last_scale, clocks_pending;
            if (clocks_pending && !--clocks_pending) clocks_log(docked ? "docked" : "handheld");
            int tw = docked ? 1920 : 1280, th = docked ? 1080 : 720, cw, ch;
            SDL_GL_GetDrawableSize(s_window, &cw, &ch);
            if (cw != tw || ch != th) {
                SDL_SetWindowSize(s_window, tw, th);
                SDL_GL_GetDrawableSize(s_window, &cw, &ch);
            }
            int scale = docked && g_set.docked_res ? 2 : 1;
            if (cw != last_w || ch != last_h || scale != last_scale) {
                rt_log("display: %s, output %dx%d, game rendered at %dx", docked ? "docked" : "handheld", cw, ch, scale);
                clocks_pending = 120; /* clocks settle after a dock change: log them in 2 s */
                last_w = cw; last_h = ch; last_scale = scale;
            }
        }
        if (g_log_mask) render_gpu_timer_begin(); /* a timed frame makes the GPU drain first: only for the logs */
        bool got = render_update(docked && g_set.docked_res ? 2 : 1);
        t_render += armTicksToNs(armGetSystemTick() - r0);
        if (got) {
            rendered++;
            total_rendered++;
            fps_frames++;
            if (boost && total_rendered == 1) {
                appletSetCpuBoostMode(ApmCpuBoostMode_Normal); /* boost throttles the GPU */
                boost = false;
                rt_log("first game frame at %.1f s", (double)armTicksToNs(armGetSystemTick() - s_boot_tick) / 1e9);
            }
        }
        u64 fps_ns = armTicksToNs(armGetSystemTick() - fps_tick);
        if (fps_ns >= 500000000ull) { /* game frames shown per second, updated twice a second */
            s_fps = (float)((double)fps_frames * 1e9 / (double)fps_ns);
            if (fps_frames) {
                /* GAME: the game thread's busy time, GX: the graphics thread, RENDER: GL work on
                 * this thread (submit + present, incl. any wait for the GPU), per game frame */
                uint64_t idle = g_guest_idle_ns - pf_idle, gx = g_gx_busy_ns - pf_gx;
                double game = idle < fps_ns ? (double)(fps_ns - idle) : 0.0;
                snprintf(perf, sizeof perf, "GAME %.1f  GX %.1f  RENDER %.1f MS", game / fps_frames / 1e6,
                         (double)gx / fps_frames / 1e6, pf_loops ? (double)pf_main / pf_loops / 1e6 : 0.0);
            }
            pf_idle = g_guest_idle_ns;
            pf_gx = g_gx_busy_ns;
            pf_main = pf_loops = 0;
            fps_frames = 0;
            fps_tick = armGetSystemTick();
        }
        render_gpu_timer_mid();
        int w, h;
        SDL_GL_GetDrawableSize(s_window, &w, &h);
        u64 p0 = armGetSystemTick();
        if (total_rendered) {
            float adj[3];
            color_adjust(adj);
            render_present(w, h, adj, wide);
            /* 4:3 border art only while a race is on screen (Race2D::draw ran within the last
             * half second; kept while the menu pauses the game) */
            static uint32_t race_seen;
            static u64 race_tick;
            if (g_race_frames != race_seen) { race_seen = g_race_frames; race_tick = armGetSystemTick(); }
            static bool racing;
            if (!menu_is_open()) racing = race_tick && armTicksToNs(armGetSystemTick() - race_tick) < 500000000ull;
            if (g_set.border && !wide && racing) menu_border(w, h);
            menu_draw(w, h, s_fps, perf);
        } else screen("BOOTING", NULL, 0.8f);
        render_gpu_timer_end();
        u64 s0 = armGetSystemTick();
        t_present += armTicksToNs(s0 - p0);
        pf_main += armTicksToNs(s0 - r0);
        pf_loops++;
        SDL_GL_SwapWindow(s_window);
        t_swap += armTicksToNs(armGetSystemTick() - s0);
        if (++frames % 300 == 0) {
            u64 now = armGetSystemTick();
            uint32_t nb, nv, nt;
            render_stats(&nb, &nv, &nt);
            rt_log("present: %.1f fps, %u game frames rendered, last frame %u batches %u verts, %u textures, late audio callbacks %llu",
                   300.0 * 1e9 / (double)armTicksToNs(now - stat_tick), rendered, nb, nv, nt, (unsigned long long)g_audio_late_cb);
            unsigned progs;
            double cms;
            render_shader_stats(&progs, &cms);
            /* render includes gpu wait (the fence of the frame NSLOT back); present = blit + menu,
             * where the driver blocks while the GPU still holds the next screen buffer */
            rt_log("timing: render %.1f ms/loop (gpu wait %.1f), present %.1f, swap wait %.1f ms/loop; %u TEV programs (%.0f ms compiling)",
                   (double)t_render / 300 / 1e6, (double)render_take_gpu_wait() / 300 / 1e6, (double)t_present / 300 / 1e6,
                   (double)t_swap / 300 / 1e6, progs, cms);
            double g_avg, g_max, g_img, g_out;
            unsigned g_n = render_gpu_timer_take(&g_avg, &g_max, &g_img, &g_out);
            if (g_n) rt_log("gpu: %.1f ms per loop (game image %.1f, output %.1f; max %.1f, %u samples; includes waits for the CPU's commands)",
                            g_avg, g_img, g_out, g_max, g_n);
            if (hdtex_available()) {
                unsigned loads, fails, queued;
                uint64_t read;
                hdtex_stats(&loads, &fails, &read, &queued);
                rt_log("hd: %u showing (%.0f MB on the GPU), %u loads (%.0f MB read), %u failed, %u queued", render_hd_in_use(),
                       (double)render_hd_bytes() / 1048576, loads, (double)read / 1048576, fails, queued);
            }
            /* malloc's own count: the process heap is reserved whole at start */
            extern char *fake_heap_start, *fake_heap_end;
            struct mallinfo mi = mallinfo();
            rt_log("heap: %.0f MB in use of %.0f MB", (double)mi.uordblks / 1048576, (double)(fake_heap_end - fake_heap_start) / 1048576);
            t_render = t_swap = t_present = 0;
            stat_tick = now;
            rendered = 0;
            diag_flush();
        }
    }
out:
    rt_log("exit: %s", s_quit ? "menu EXIT GAME" : "close request");
    for (int p = 0; p < 4; p++) rumble_send(p, 0.0f);
    prof_stop();
    gx_stop_gpu();
    if (perf_set) apmSetPerformanceConfiguration(ApmPerformanceMode_Normal, perf_prev);
    diag_flush();
    SDL_Quit();
    /* The game, guest and decoder threads would keep running on memory the loader takes back. */
    plat_pause_threads();
    _Exit(0);
}
