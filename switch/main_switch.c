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
const char g_build_stamp[] = __DATE__ " " __TIME__;
#include "../runtime/native.h"

#define APP_DIR "sdmc:/switch/MKDD-NX"
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
    FILE *f = fopen(APP_DIR "/crash.log", "w");
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

/* ---- loading / message screen: scissored glClear text, works before the renderer ---- */
static void fill_rect(int x, int y, int w, int h, int fb_h) { glScissor(x, fb_h - y - h, w, h); glClear(GL_COLOR_BUFFER_BIT); }
static void draw_text(const char *s, int y, int px, int fb_w, int fb_h) {
    int x = (fb_w - (int)strlen(s) * 6 * px) / 2;
    for (; *s; s++, x += 6 * px) {
        const char *g = font_glyph(*s);
        if (!g) continue;
        for (int i = 0; i < 35; i++)
            if (g[i] == '1') fill_rect(x + (i % 5) * px, y + (i / 5) * px, px, px, fb_h);
    }
}
/* Loading-screen logo (switch/logo.S): uploaded once into a texture behind a read framebuffer,
 * then blitted (flipped: the data is top row first). */
extern const uint8_t mkdd_logo_rgba[];
#define LOGO_SIZE 512
static void draw_logo(int x, int y, int size, int fb_h) {
    static GLuint tex, fbo;
    if (!fbo) {
        glGenTextures(1, &tex);
        glBindTexture(GL_TEXTURE_2D, tex);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, LOGO_SIZE, LOGO_SIZE, 0, GL_RGBA, GL_UNSIGNED_BYTE, mkdd_logo_rgba);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glGenFramebuffers(1, &fbo);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo);
        glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
    }
    glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    glBlitFramebuffer(0, 0, LOGO_SIZE, LOGO_SIZE, x, fb_h - y, x + size, fb_h - y - size, GL_COLOR_BUFFER_BIT, GL_LINEAR);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

static void screen(const char *line1, const char *line2, float progress) {
    int w = 0, h = 0;
    SDL_GL_GetDrawableSize(s_window, &w, &h);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, w, h);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glDisable(GL_SCISSOR_TEST);
    glClearColor(24 / 255.0f, 24 / 255.0f, 32 / 255.0f, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    int logo = h * 58 / 100;
    draw_logo((w - logo) / 2, h * 3 / 100, logo, h);
    glEnable(GL_SCISSOR_TEST);
    int px = h / 90 > 2 ? h / 90 : 2, small = px / 2 > 2 ? px / 2 : 2;
    glClearColor(1, 1, 1, 1);
    if (line1) draw_text(line1, h * 66 / 100, small, w, h);
    if (line2) draw_text(line2, h * 73 / 100, small, w, h);
    if (progress >= 0) {
        int bw = w / 2, bh = px * 3, bx = (w - bw) / 2, by = h * 82 / 100;
        glClearColor(10 / 255.0f, 10 / 255.0f, 14 / 255.0f, 1);
        fill_rect(bx, by, bw, bh, h);
        glClearColor(240 / 255.0f, 200 / 255.0f, 80 / 255.0f, 1);
        fill_rect(bx, by, (int)((float)bw * (progress > 1 ? 1 : progress)), bh, h);
    }
    glDisable(GL_SCISSOR_TEST);
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

/* ---- input: Switch controller -> GameCube pad on port 0 (mapping from the - menu) ---- */
static int8_t stick(s32 v) { /* +-32767 -> +-100 (a GameCube stick's usable range) */
    int x = v * 100 / 32767;
    return (int8_t)(x > 127 ? 127 : x < -128 ? -128 : x);
}
static bool s_quit;
static float s_fps;
static void update_pad(PadState *pad) {
    padUpdate(pad);
    u64 b = padGetButtons(pad);
    PadStatus p = {0};
    p.connected = true;
    if (menu_input(padGetButtonsDown(pad), b, &s_quit)) { pad_set(0, &p); return; }
    HidAnalogStickState l = padGetStickPos(pad, 0), r = padGetStickPos(pad, 1);
    static const uint16_t GC_BITS[GC_COUNT] = {PAD_A, PAD_B, PAD_X, PAD_Y, PAD_TRIGGER_Z, PAD_TRIGGER_L, PAD_TRIGGER_R, PAD_START};
    for (int i = 0; i < GC_COUNT; i++)
        if (b & g_set.map[i]) p.button |= GC_BITS[i];
    if (p.button & PAD_TRIGGER_R) p.trigger_r = 255;
    if (p.button & PAD_TRIGGER_L) p.trigger_l = 255;
    p.stick_x = stick(l.x);
    p.stick_y = stick(l.y);
    /* D-pad = control stick at full deflection; diagonals on the GameCube's octagonal gate */
    int dx = (b & HidNpadButton_Right ? 1 : 0) - (b & HidNpadButton_Left ? 1 : 0);
    int dy = (b & HidNpadButton_Up ? 1 : 0) - (b & HidNpadButton_Down ? 1 : 0);
    if (dx || dy) {
        int m = dx && dy ? 72 : 100;
        p.stick_x = (int8_t)(dx * m);
        p.stick_y = (int8_t)(dy * m);
    }
    p.substick_x = stick(r.x);
    p.substick_y = stick(r.y);
    pad_set(0, &p);
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
    chdir(APP_DIR);
    s_boot_tick = armGetSystemTick();
    /* Keep the previous session's logs (newlib's rename does not replace an existing file). */
    static const char *const LOGS[][2] = {
        {APP_DIR "/stderr.log", APP_DIR "/stderr.prev.log"},     {APP_DIR "/crash.log", APP_DIR "/crash.prev.log"},
        {APP_DIR "/mkdd.log", APP_DIR "/mkdd.prev.log"},         {APP_DIR "/mkdd_trace.log", APP_DIR "/mkdd_trace.prev.log"},
        {APP_DIR "/mkdd_prof.bin", APP_DIR "/mkdd_prof.prev.bin"},
    };
    for (size_t i = 0; i < sizeof LOGS / sizeof *LOGS; i++) {
        FILE *f = fopen(LOGS[i][0], "rb");
        if (!f) continue;
        fclose(f);
        remove(LOGS[i][1]);
        rename(LOGS[i][0], LOGS[i][1]);
    }
    FILE *bt = fopen(APP_DIR "/boot.txt", "w");
    if (bt) { fprintf(bt, "main() reached, build " __DATE__ " " __TIME__ "\n"); fclose(bt); }
    freopen(APP_DIR "/stderr.log", "w", stderr);
    setvbuf(stderr, NULL, _IOFBF, 64 << 10);
    diag_set_log_file(APP_DIR "/mkdd.log");
    memcard_set_path(APP_DIR "/save/slotA.bin");
    settings_load(APP_DIR "/settings.ini");
    color_load(APP_DIR "/save/color.ini");
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

    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    PadState pad;
    padInitializeDefault(&pad);

    char disc[512], err[512];
    screen("LOOKING FOR THE DISC IMAGE", NULL, 0.05f);
    if (!find_disc(disc, sizeof disc)) fatal("NO DISC IMAGE FOUND", "PUT YOUR GM4E01 .ISO IN /SWITCH/MKDD-NX/");
    screen("STARTING", NULL, 0.3f);
    if (!render_init()) fatal("GRAPHICS INIT FAILED", "SEE MKDD.LOG");
    render_shader_cache(APP_DIR "/shaders.bin", shader_progress);
    if (!machine_boot(disc, &s_boot, err, sizeof err)) {
        rt_log("boot failed: %s", err);
        fatal("COULD NOT START THE GAME", "WRONG DISC: NEEDS MARIO KART DOUBLE DASH USA (GM4E01)");
    }
    diag_start_watchdog(APP_DIR "/mkdd_trace.log");

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

    update_pad(&pad);
    /* An empty file "cpu_xform" in the app folder switches the XF stage back to the decoder (CPU). */
    g_gx_cpu_xform = access(APP_DIR "/cpu_xform", F_OK) == 0;
    rt_log("xf: vertex transform on the %s", g_gx_cpu_xform ? "CPU (cpu_xform file)" : g_gx_cpu_xform_forced ? "CPU (shader failed)" : "GPU");
    /* Sampling profiler (mkdd_prof.bin, tools/prof_report.py); an empty file "noprof" turns it off. */
    if (access(APP_DIR "/noprof", F_OK) != 0) {
        u64 ib, ie;
        image_range(&ib, &ie);
        prof_start(APP_DIR "/mkdd_prof.bin", ib, ie);
    }
    pthread_start("game", game_thread, NULL, 32u << 20, 1, 0);
    screen("BOOTING", NULL, 0.6f);

    bool boost = true;
    unsigned frames = 0;
    u64 stat_tick = armGetSystemTick();
    unsigned rendered = 0, total_rendered = 0, fps_frames = 0;
    u64 fps_tick = armGetSystemTick();
    uint64_t t_render = 0, t_swap = 0, t_present = 0;
    while (appletMainLoop()) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_QUIT) goto out;
        }
        /* Out of focus (HOME menu) vi stops returning buffers, so a swap would block and the
         * close request never reach appletMainLoop: idle until focus comes back. */
        if (appletGetFocusState() != AppletFocusState_InFocus) { svcSleepThread(16000000); continue; }
        update_pad(&pad);
        if (s_quit) break;
        g_aspect_scale = g_set.wide ? 4.0f / 3.0f : 1.0f;
        render_set_hud43(g_set.wide);
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
            fps_frames = 0;
            fps_tick = armGetSystemTick();
        }
        int w, h;
        SDL_GL_GetDrawableSize(s_window, &w, &h);
        u64 p0 = armGetSystemTick();
        if (total_rendered) {
            float adj[3];
            color_adjust(adj);
            render_present(w, h, adj, g_set.wide);
            menu_draw(w, h, s_fps);
        } else screen("BOOTING", NULL, 0.8f);
        u64 s0 = armGetSystemTick();
        t_present += armTicksToNs(s0 - p0);
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
    prof_stop();
    gx_stop_gpu();
    if (perf_set) apmSetPerformanceConfiguration(ApmPerformanceMode_Normal, perf_prev);
    diag_flush();
    SDL_Quit();
    /* The game, guest and decoder threads would keep running on memory the loader takes back. */
    plat_pause_threads();
    _Exit(0);
}
