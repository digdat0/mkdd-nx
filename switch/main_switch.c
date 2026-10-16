/* Nintendo Switch front end: SD folder + logs, crash.log, loading screen, disc lookup, boot,
 * render loop (GLES via SDL2 + Mesa), controller input (libnx HID) and audio (SDL2).
 * Platform glue adapted from the gcNX template (gc-decomp-switch-port skill). */
#include <dirent.h>
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

/* First .iso / .gcm / .ciso in the app folder. */
static bool find_disc(char *out, size_t n) {
    DIR *d = opendir(APP_DIR);
    if (!d) return false;
    struct dirent *e;
    bool found = false;
    while ((e = readdir(d))) {
        const char *dot = strrchr(e->d_name, '.');
        if (dot && (!strcasecmp(dot, ".iso") || !strcasecmp(dot, ".gcm") || !strcasecmp(dot, ".ciso"))) {
            snprintf(out, n, APP_DIR "/%s", e->d_name);
            found = true;
            break;
        }
    }
    closedir(d);
    return found;
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
            if (src_of[p] < 0) rt_log("pad: player %d disconnected (was %s)", p + 1, style_name(s_player_style[p]));
            else rt_log("pad: player %d connected: %s (source %d)", p + 1, style_name(st), src_of[p]);
            s_player_src[p] = src_of[p];
            s_player_style[p] = st;
        }
    }

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
    (void)argc; (void)argv;
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
    s_window = SDL_CreateWindow(APP_TITLE, 0, 0, 1280, 720, SDL_WINDOW_OPENGL);
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
    if (!find_disc(disc, sizeof disc)) fatal("NO DISC IMAGE FOUND", "PUT YOUR GM4E01 .ISO IN /SWITCH/MKDD-NX/");
    if (disc_open(disc, err, sizeof err)) {
        menu_font_load();  /* the loading screen's letters */
        pause_art_init();  /* the pause screen's controller picture, drawn as a Switch */
        render_set_subst(pause_art_texture);
    }
    screen("STARTING", NULL, 0.3f);
    if (!render_init()) fatal("GRAPHICS INIT FAILED", "SEE MKDD.LOG");
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
    }

    update_pads();
    /* An empty file "cpu_xform" in the app folder switches the XF stage back to the decoder (CPU). */
    g_gx_cpu_xform = access(CFG_DIR "/cpu_xform", F_OK) == 0;
    rt_log("xf: vertex transform on the %s", g_gx_cpu_xform ? "CPU (cpu_xform file)" : g_gx_cpu_xform_forced ? "CPU (shader failed)" : "GPU");
    /* Sampling profiler (mkdd_prof.bin, tools/prof_report.py); an empty file "noprof" turns it off. */
    if (access(CFG_DIR "/noprof", F_OK) != 0) {
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
        }
        /* SequenceApp::msScene (r13 0x803D1420 - 0x7740): 0 = SceneTitle, the "press start"
         * screen. Its artwork is one 4:3 picture that 16:9 only stretched, so it stays 4:3. */
        bool wide = g_set.wide && m_read_be(0x803C9CE0u, 4) != 0;
        g_aspect_scale = wide ? 4.0f / 3.0f : 1.0f;
        render_set_hud43(wide);
        u64 r0 = armGetSystemTick();
        bool got = render_update(1);
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
        int w, h;
        SDL_GL_GetDrawableSize(s_window, &w, &h);
        u64 p0 = armGetSystemTick();
        if (total_rendered) {
            float adj[3];
            color_adjust(adj);
            render_present(w, h, adj, wide);
            menu_draw(w, h, s_fps, perf);
        } else screen("BOOTING", NULL, 0.8f);
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
