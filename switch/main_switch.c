/* Nintendo Switch front end: SD folder + logs, crash.log, loading screen, disc lookup, boot,
 * render loop (GLES via SDL2 + Mesa), controller input (libnx HID) and audio (SDL2).
 * Platform glue adapted from the gcNX template (gc-decomp-switch-port skill). */
#include <dirent.h>
#include <malloc.h>
#include <math.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
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
#include "charpack_art.h"
#include "hdtex.h"
#include "update.h"
const char g_build_stamp[] = __DATE__ " " __TIME__;
#include "../runtime/native.h"
#include "../runtime/table.h"

#define APP_DIR "sdmc:/switch/MKDD-NX"
/* The app folder holds only the .nro, the disc image and these three folders. */
#define LOG_DIR APP_DIR "/logs"
#define CFG_DIR APP_DIR "/config"
#define SAVE_DIR APP_DIR "/save"
#define MODS_DIR APP_DIR "/mods"
#define HD_PATH MODS_DIR "/hd/" HD_FILE
#define APP_TITLE "MARIO KART DOUBLE DASH"

static SDL_Window *s_window;
static const char *s_mod_key; /* the mods/ folder this session plays, NULL for the standard game */
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
static bool s_clk_ok, s_clk_init, s_clk_open[3];
static ClkrstSession s_clk[3];
static void clocks_init(void) {
    if (!hosversionAtLeast(8, 0, 0) || R_FAILED(clkrstInitialize())) return;
    s_clk_init = true;
    static const PcvModuleId ids[3] = {PcvModuleId_CpuBus, PcvModuleId_GPU, PcvModuleId_EMC};
    s_clk_ok = true;
    for (int i = 0; i < 3; i++) s_clk_ok &= (s_clk_open[i] = R_SUCCEEDED(clkrstOpenSession(&s_clk[i], ids[i], 3)));
}
static void clocks_log(const char *why) {
    if (!s_clk_ok) return;
    u32 hz[3] = {0};
    for (int i = 0; i < 3; i++) clkrstGetClockRate(&s_clk[i], &hz[i]);
    rt_log("clocks (%s): CPU %u MHz, GPU %u MHz, memory %u MHz", why, hz[0] / 1000000, hz[1] / 1000000, hz[2] / 1000000);
}
/* The .nro runs inside the loader's process, which lives on after exit: sessions left open stay
 * open. pcv has few of them, and leaked ones made it abort (2011-0102, out of session memory) and
 * take ptm and clock sysmodules with it, locking the console. */
static void clocks_exit(void) {
    s_clk_ok = false;
    for (int i = 0; i < 3; i++)
        if (s_clk_open[i]) clkrstCloseSession(&s_clk[i]), s_clk_open[i] = false;
    if (s_clk_init) clkrstExit(), s_clk_init = false;
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

/* The mods this build carries code for, extracted to mods/<key> (sys/main.dol, sys/fst.bin,
 * files/): bit n - 1 for MODS[n - 1]. */
static unsigned find_mods(void) {
    unsigned found = 0;
    for (unsigned i = 0; i < MOD_COUNT && i < 32; i++) {
        char dir[256], p[300];
        struct stat st;
        snprintf(dir, sizeof dir, MODS_DIR "/%s", MODS[i].key);
        if (stat(dir, &st) != 0) continue;
        bool ok = true;
        snprintf(p, sizeof p, "%s/sys/main.dol", dir), ok = ok && stat(p, &st) == 0;
        snprintf(p, sizeof p, "%s/sys/fst.bin", dir), ok = ok && stat(p, &st) == 0;
        snprintf(p, sizeof p, "%s/files", dir), ok = ok && stat(p, &st) == 0 && S_ISDIR(st.st_mode);
        if (ok) found |= 1u << i, rt_log("mod: found %s (%s)", dir, MODS[i].name);
        else rt_log("mod: %s needs sys/main.dol, sys/fst.bin and files/", dir);
    }
    return found;
}

/* A choice of two on the loading screen: up/down, then A. */
static int choose(const char *title, const char *a, const char *b, int sel, const char *hint) {
    const char *opts[2] = {a, b};
    PadState pad;
    padInitializeDefault(&pad);
    padUpdate(&pad);
    while (appletMainLoop()) {
        padUpdate(&pad);
        u64 down = padGetButtonsDown(&pad);
        if (down & (HidNpadButton_AnyUp | HidNpadButton_StickLUp)) sel = 0;
        if (down & (HidNpadButton_AnyDown | HidNpadButton_StickLDown)) sel = 1;
        if (down & HidNpadButton_A) break;
        int w = 0, h = 0;
        SDL_GL_GetDrawableSize(s_window, &w, &h);
        menu_choice(w, h, title, opts, 2, sel, hint);
        SDL_GL_SwapWindow(s_window);
        SDL_PumpEvents();
    }
    return sel;
}

/* The standard game's save file (unlocks, records, options; ghosts are files of their own). */
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

static bool copy_file(const char *from, const char *to) {
    char tmp[600];
    snprintf(tmp, sizeof tmp, "%s.tmp", to);
    FILE *in = fopen(from, "rb"), *out = in ? fopen(tmp, "wb") : NULL;
    bool ok = in && out;
    char buf[16384];
    size_t n;
    while (ok && (n = fread(buf, 1, sizeof buf, in)) > 0) ok = fwrite(buf, 1, n, out) == n;
    ok = ok && !ferror(in);
    if (in) fclose(in);
    if (out && fclose(out) != 0) ok = false;
    if (ok) {
        remove(to);
        ok = rename(tmp, to) == 0;
    }
    if (!ok) remove(tmp);
    return ok;
}

/* A mod with no save yet (save_dir) and a standard save there: offer to start the mod from a copy
 * of it. The mods keep the game's save format, so the copy loads as it is; the standard save is
 * left alone. */
static void offer_save_copy(const char *save_dir, const char *key) {
    struct stat st;
    char card[300], gci[400], title[64];
    snprintf(card, sizeof card, "%s/card_a", save_dir);
    if (has_gci(card) || stat(SAVE_DIR SYSTEM_GCI, &st) != 0) return;
    snprintf(title, sizeof title, "%s HAS NO SAVE YET", key);
    for (char *t = title; *t; t++) *t = (char)toupper((unsigned char)*t);
    if (choose(title, "COPY MY MARIO KART SAVE", "START A NEW SAVE", 0,
               "UNLOCKS AND RECORDS; YOUR SAVE STAYS AS IT IS") != 0) {
        rt_log("mod: new save");
        return;
    }
    mkdir(card, 0777);
    snprintf(gci, sizeof gci, "%s" SYSTEM_GCI, save_dir);
    bool ok = copy_file(SAVE_DIR SYSTEM_GCI, gci);
    rt_log("mod: copying the standard save %s", ok ? "done" : "FAILED");
    if (!ok) screen("COULD NOT COPY THE SAVE", "STARTING WITH A NEW ONE", -1), svcSleepThread(2500000000ll);
}

/* ---- mod install: an xdelta patch or a patched disc image in mods/ becomes mods/<key> ---- */
#define MODS_SEEN CFG_DIR "/mods_seen.txt" /* "<name> <size> <mods_sig>": files in mods/ that are no mod this build knows */

/* The mods this build knows, as a word (a build that knows more checks those files again). */
static const char *mods_sig(void) {
    static char sig[8 * 32 + 1];
    size_t k = 0;
    for (unsigned i = 0; i < MOD_COUNT && k + 8 < sizeof sig; i++) k += (size_t)snprintf(sig + k, sizeof sig - k, "%.8s", MODS[i].dol_sha1);
    return sig;
}

static bool upper_cpy(char *dst, size_t size, const char *s) {
    size_t i = 0;
    for (; s[i] && i + 1 < size; i++) dst[i] = (char)toupper((unsigned char)s[i]);
    dst[i] = 0;
    return true;
}

/* "name\nsize" from an installed mod's source.txt, or a mods_seen.txt line, matches this file. */
static bool file_listed(const char *list, const char *name, long long size, bool lines) {
    FILE *f = fopen(list, "r");
    if (!f) return false;
    char a[256], b[64], want[64];
    snprintf(want, sizeof want, "%lld", size);
    bool hit = false;
    if (!lines) {
        hit = fgets(a, sizeof a, f) && fgets(b, sizeof b, f) && (a[strcspn(a, "\r\n")] = 0, b[strcspn(b, "\r\n")] = 0, 1) &&
              !strcmp(a, name) && !strcmp(b, want);
    } else {
        char line[400], key[400];
        snprintf(key, sizeof key, "%s %s %s", name, want, mods_sig());
        while (!hit && fgets(line, sizeof line, f)) line[strcspn(line, "\r\n")] = 0, hit = !strcmp(line, key);
    }
    fclose(f);
    return hit;
}

typedef struct { const char *title; PadState pad; u64 shown; } InstallUi;
static bool install_progress(void *ud, uint64_t done, uint64_t total) {
    InstallUi *ui = ud;
    padUpdate(&ui->pad);
    if (padGetButtonsDown(&ui->pad) & HidNpadButton_B) return false;
    u64 now = armGetSystemTick();
    if (now - ui->shown < armNsToTicks(100000000)) return true; /* 10 updates a second at most */
    ui->shown = now;
    char line[96];
    snprintf(line, sizeof line, "%u%%   B: CANCEL", total ? (unsigned)(done * 100 / total) : 0);
    screen(ui->title, line, total ? (float)done / (float)total : 0.f);
    return true;
}

/* Each mod file in mods/ (.xdelta / .xd3 / .vcdiff patch of the clean disc, or a patched .iso /
 * .gcm) not installed yet: say which mod it is, offer to install it, then to delete the file. */
static void install_mods(void) {
    DIR *d = opendir(MODS_DIR);
    if (!d) return;
    char names[16][256];
    int count = 0;
    struct dirent *e;
    while ((e = readdir(d)) && count < 16) {
        const char *dot = strrchr(e->d_name, '.');
        if (dot && (!strcasecmp(dot, ".xdelta") || !strcasecmp(dot, ".xd3") || !strcasecmp(dot, ".vcdiff") ||
                    !strcasecmp(dot, ".iso") || !strcasecmp(dot, ".gcm")))
            snprintf(names[count++], sizeof names[0], "%s", e->d_name);
    }
    closedir(d);
    for (int i = 0; i < count; i++) {
        char path[512], err[256], line1[96], line2[96];
        struct stat st;
        snprintf(path, sizeof path, MODS_DIR "/%s", names[i]);
        if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) continue;
        bool done = file_listed(MODS_SEEN, names[i], (long long)st.st_size, true);
        for (unsigned m = 0; m < MOD_COUNT && !done; m++) {
            char src[300];
            snprintf(src, sizeof src, MODS_DIR "/%s/source.txt", MODS[m].key);
            done = file_listed(src, names[i], (long long)st.st_size, false);
        }
        if (done) continue;
        rt_log("install: checking %s (%lld bytes)", path, (long long)st.st_size);
        upper_cpy(line2, sizeof line2, names[i]);
        screen("CHECKING A MOD FILE", line2, 0.05f);
        unsigned n = mod_identify(path, err, sizeof err);
        if (!n) {
            rt_log("install: %s: %s", names[i], err);
            FILE *f = fopen(MODS_SEEN, "a");
            if (f) fprintf(f, "%s %lld %s\n", names[i], (long long)st.st_size, mods_sig()), fclose(f);
            upper_cpy(line1, sizeof line1, err);
            for (int k = 0; k < 240 && appletMainLoop(); k++) screen(line2, line1, 0.05f); /* 4 s */
            continue;
        }
        const ModInfo *mi = &MODS[n - 1];
        struct statvfs vf;
        double free_gb = statvfs(APP_DIR, &vf) == 0 ? (double)vf.f_bavail * vf.f_frsize / 1e9 : -1;
        snprintf(line1, sizeof line1, "INSTALL %s?", mi->name);
        upper_cpy(line1, sizeof line1, line1);
        char hint[96];
        if (free_gb >= 0) snprintf(hint, sizeof hint, "TAKES A FEW MINUTES; SD CARD: %.1f GB FREE", free_gb);
        else snprintf(hint, sizeof hint, "TAKES A FEW MINUTES");
        if (choose(line1, "INSTALL NOW", "NOT NOW", 0, hint) != 0) {
            rt_log("install: %s: not now", names[i]);
            continue;
        }
        char out[300];
        snprintf(out, sizeof out, MODS_DIR "/%s", mi->key);
        snprintf(line1, sizeof line1, "INSTALLING %s", mi->name);
        upper_cpy(line1, sizeof line1, line1);
        InstallUi ui = {line1, {0}, 0};
        padInitializeDefault(&ui.pad);
        padUpdate(&ui.pad);
        if (!mod_install(path, out, install_progress, &ui, err, sizeof err)) {
            upper_cpy(line2, sizeof line2, err);
            for (int k = 0; k < 300 && appletMainLoop(); k++) screen("THE MOD WAS NOT INSTALLED", line2, -1); /* 5 s */
            continue;
        }
        snprintf(g_set.mod, sizeof g_set.mod, "%s", mi->key); /* play what was just installed */
        settings_save();
        snprintf(line1, sizeof line1, "%s IS INSTALLED", mi->name);
        upper_cpy(line1, sizeof line1, line1);
        snprintf(hint, sizeof hint, "THE MOD NO LONGER NEEDS IT: FREES %.1f GB", (double)st.st_size / 1e9);
        if (choose(line1, "KEEP THE MOD FILE", "DELETE THE MOD FILE", 0, hint) == 1) {
            bool ok = remove(path) == 0;
            rt_log("install: deleting %s %s", path, ok ? "done" : "FAILED");
        }
    }
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
extern volatile uint32_t g_race_frames, g_race_phase, g_race_drive;
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

/* map: BUTTON MAPPING's layout while driving, the standard one everywhere else (update_pads) */
static void map_full(PadState *pad, u64 b, const uint64_t *map, PadStatus *p) {
    static const uint16_t GC_BITS[GC_COUNT] = {PAD_A, PAD_B, PAD_X, PAD_Y, PAD_TRIGGER_Z, PAD_TRIGGER_L, PAD_TRIGGER_R, PAD_START};
    for (int i = 0; i < GC_COUNT; i++)
        if (b & map[i]) p->button |= GC_BITS[i];
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
    uint32_t jump = 0, cam_reset = 0; /* JUMP (menu.c ACTIONS, GAME > JUMP): the ports holding it, for the game thread */
    /* BUTTON MAPPING is for driving only: menus, the pause screen, the flyover and the results
     * keep the standard buttons (hle.c g_race_drive). Paused in a race, a PAUSE button the
     * standard layout leaves free resumes too. */
    uint32_t drive = g_race_drive;
    const uint64_t *map = drive == 1 ? g_set.map : g_map_default;
    uint64_t std = 0;
    for (int i = 0; i < GC_COUNT; i++) std |= g_map_default[i];
    uint64_t resume = drive == 2 ? g_set.map[GC_START] & ~std : 0;
    for (int p = 0; p < 4; p++) {
        PadStatus s = {0};
        int src = s_player_src[p];
        s.connected = src >= 0 || p == 0; /* port 1 is always there (the game expects a pad) */
        if (src >= 0 && !menu) {
            PadState *pad = &s_src[src];
            u64 b = padGetButtons(pad);
            if (pad == p1) b &= ~menu_blocked();
            if (drive == 1 && (b & map[MAP_JUMP])) jump |= 1u << p;
            if (drive == 1 && (b & map[MAP_CAM_RESET])) cam_reset |= 1u << p;
            u32 st = s_player_style[p];
            bool single = (st & (HidNpadStyleTag_NpadJoyLeft | HidNpadStyleTag_NpadJoyRight)) &&
                          !(st & (HidNpadStyleTag_NpadJoyDual | HidNpadStyleTag_NpadFullKey | HidNpadStyleTag_NpadHandheld));
            HidAnalogStickState l = padGetStickPos(pad, single && (st & HidNpadStyleTag_NpadJoyRight) ? 1 : 0);
            s.stick_x = stick(l.x);
            s.stick_y = stick(l.y);
            if (st & HidNpadStyleTag_NpadGc) map_gc(pad, b, &s);
            else if (single) map_sideways(b, (st & HidNpadStyleTag_NpadJoyLeft) != 0, p == 0, &s);
            else {
                map_full(pad, b, map, &s);
                if (b & resume) s.button |= PAD_START;
            }
            /* Start pressed: the pause screen shows this controller's buttons (pause_art.c) */
            static bool start_was[4];
            bool start = s.button & PAD_START;
            if (start && !start_was[p])
                g_pause_kind = st & HidNpadStyleTag_NpadGc ? PAUSE_GC
                               : single ? (st & HidNpadStyleTag_NpadJoyLeft ? PAUSE_LEFT : PAUSE_RIGHT) : PAUSE_FULL;
            start_was[p] = start;
            /* FREE CAMERA: the right stick (a GameCube pad's C stick) sideways; races never read it */
            float pan = s.substick_x / 100.0f;
            g_cam_pan[p] = g_set.free_cam && !single && (pan > 0.3f || pan < -0.3f) ? (pan > 1 ? 1 : pan < -1 ? -1 : pan) : 0;
            /* its RANDOM is GameCube L + R held, as the screen shows: L has no button of its own
             * by default (ZL jumps, L / R are Z), so there Switch L and R are GameCube L and R
             * too (Z only swaps drivers on the kart step); the same on the cup and course pages
             * while their "L + R: RANDOM" shows */
            if (charpack_select_active() || pause_art_random_shown()) {
                if (b & HidNpadButton_L) s.button |= PAD_TRIGGER_L;
                if (b & HidNpadButton_R) s.button |= PAD_TRIGGER_R;
            }
            /* and with character packs, X the next page of characters, Y the previous, as on the
             * cup page (native.c SceneMenu::character takes them) */
            static u64 page_was[4];
            u64 page = b & (HidNpadButton_X | HidNpadButton_Y), down = page & ~page_was[p];
            page_was[p] = page;
            if (charpack_pages() > 1 && charpack_select_active()) {
                if (down & HidNpadButton_X) __atomic_or_fetch(&g_char_cycle, 1u << p, __ATOMIC_RELEASE);
                else if (down & HidNpadButton_Y) __atomic_or_fetch(&g_char_cycle, 1u << (4 + p), __ATOMIC_RELEASE);
            }
            if ((s.button & PAD_TRIGGER_R) && !s.trigger_r) s.trigger_r = 255;
            if ((s.button & PAD_TRIGGER_L) && !s.trigger_l) s.trigger_l = 255;
        }
        /* FIRST PERSON (menu.c ACTIONS, a full controller's button): player 1's camera between
         * NORMAL and first person, DRIVER HIGH (CAMERA VIEW 3), saved with the next settings
         * change, not now (an SD card write mid-race can stall a frame); players 2-4 each their
         * own (bit p of g_first_person, this session only), player 1's CAMERA VIEW untouched */
        {
            static bool was_fp[4];
            bool was = was_fp[p];
            int src = s_player_src[p];
            u32 st = s_player_style[p];
            bool full = !(st & HidNpadStyleTag_NpadGc) &&
                        (!(st & (HidNpadStyleTag_NpadJoyLeft | HidNpadStyleTag_NpadJoyRight)) ||
                         (st & (HidNpadStyleTag_NpadJoyDual | HidNpadStyleTag_NpadFullKey | HidNpadStyleTag_NpadHandheld)));
            bool on = src >= 0 && !menu && full && drive == 1 &&
                      (padGetButtons(&s_src[src]) & map[MAP_FP] & (&s_src[src] == p1 ? ~menu_blocked() : ~0ull));
            if (on && !was && p == 0) g_set.camera = g_set.camera ? 0 : 3;
            else if (on && !was) __atomic_xor_fetch(&g_first_person, 1u << p, __ATOMIC_RELAXED);
            was_fp[p] = on;
        }
        /* REAR VIEW (GameCube Y, menu.c ACTIONS): while this player's race camera runs (its
         * view updated in the last few polls; view = player, any number of screens), their Y
         * turns that camera around instead of reaching the game, so it never throws an item
         * (X does). A press belongs to whoever had it when it began, until it is let go: one
         * held from a menu stays the game's, one held into the finish never becomes an item throw. */
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
    g_jump_held = jump;
    g_cam_reset = cam_reset;
    g_cam_back = g_set.free_cam == 2 ? CAM_EASE : g_set.free_cam == 3 ? CAM_STAY : CAM_SNAP;
}

static void game_thread(void *arg) {
    (void)arg;
    sched_note_host();
    machine_run(&s_boot);
}

/* At exit, after the threads are paused: every region of our address space that is not plain
 * heap / code / unmapped, logged. The loader (hbl) takes the heap back after we exit and aborts
 * with 2001-0106 when any of it is still mapped elsewhere, borrowed or attributed (thread stack
 * mirrors, transfer memory, GPU-mapped buffers): this names what is left. */
static void exit_memory_report(void) {
    u64 addr = 0;
    unsigned n = 0, odd = 0;
    for (int guard = 0; guard < 4096; guard++) {
        MemoryInfo mi;
        u32 pi;
        if (R_FAILED(svcQueryMemory(&mi, &pi, addr))) break;
        u32 type = mi.type & 0xFF;
        if (type != MemType_Unmapped) {
            n++;
            bool plain = (type == MemType_Heap || type == MemType_CodeStatic || type == MemType_CodeMutable ||
                          type == MemType_ModuleCodeStatic || type == MemType_ModuleCodeMutable || type == MemType_Io ||
                          type == MemType_ThreadLocal || type == MemType_Reserved) && mi.attr == 0;
            if (!plain && odd++ < 64)
                rt_log("exit memory: %#lx-%#lx (%lu KB) type %#x attr %#x perm %#x ipc %u device %u",
                       (unsigned long)mi.addr, (unsigned long)(mi.addr + mi.size), (unsigned long)(mi.size >> 10),
                       mi.type, mi.attr, mi.perm, mi.ipc_refcount, mi.device_refcount);
        }
        u64 next = mi.addr + mi.size;
        if (next <= addr) break;
        addr = next;
    }
    rt_log("exit memory: %u regions, %u not plain heap/code", n, odd);
    {
        extern int g_plat_stacks, g_plat_unmap_ok, g_plat_unmap_fail, g_plat_unmap_gone;
        extern unsigned g_plat_unmap_rc;
        extern uint64_t g_plat_unmap_addr[3];
        rt_log("exit memory: thread stacks recorded %d, unmapped %d, already gone %d, failed %d (first rc %#x: mirror %#llx mem %#llx size %#llx)",
               g_plat_stacks, g_plat_unmap_ok, g_plat_unmap_gone, g_plat_unmap_fail, g_plat_unmap_rc,
               (unsigned long long)g_plat_unmap_addr[0], (unsigned long long)g_plat_unmap_addr[1], (unsigned long long)g_plat_unmap_addr[2]);
    }
    diag_flush();
}

/* RESOLUTION AUTO (docked and handheld): render at the mode's top scale and step down while the
 * GPU is the limit, back up after a quiet spell. GPU-bound shows as a present + swap longer than
 * 1.25 refreshes (the driver blocks there until the GPU frees a screen buffer); a late game thread
 * does not do that, so its dropped frames don't lower the resolution. Slow frames within a minute
 * of a step up double the wait before the next try (15 s, up to 5 minutes): parts of a course can
 * be heavier than others, and every failed try costs dropped frames. The course flyover before
 * the countdown drops frames on the game thread at any resolution: its frames don't count. */
static const float AUTO_DOCKED[] = {2.0f, 1.75f, 1.5f, 1.25f}, AUTO_HANDHELD[] = {1.5f, 1.25f, 1.0f};
static struct { int level; bool docked; unsigned loops, slow, quiet_s, probe_s; u64 win, last_up, settle; } s_auto = {.probe_s = 15};

static bool auto_on(bool docked) { return docked ? g_set.docked_res == DOCKED_AUTO : g_set.handheld_res == HANDHELD_AUTO; }
static const float *auto_steps(bool docked, int *n) {
    *n = docked ? 4 : 3;
    return docked ? AUTO_DOCKED : AUTO_HANDHELD;
}

/* The game's render scale: DOCKED RESOLUTION on the TV, HANDHELD RESOLUTION on the screen. */
static float render_scale(bool docked) {
    int n;
    if (auto_on(docked)) return auto_steps(docked, &n)[s_auto.level < n ? s_auto.level : n - 1];
    if (docked) return g_set.docked_res == DOCKED_NATIVE ? 1.0f : g_set.docked_res == DOCKED_1_5X ? 1.5f : 2.0f;
    return g_set.handheld_res == HANDHELD_1_25X ? 1.25f : g_set.handheld_res == HANDHELD_1_5X ? 1.5f : 1.0f;
}

static void auto_res_update(bool docked, u64 p0, u64 s1, bool got, bool paused) {
    if (!auto_on(docked) || docked != s_auto.docked) {
        s_auto.level = 0; /* AUTO starts again from the top */
        s_auto.loops = s_auto.slow = s_auto.quiet_s = 0;
        s_auto.win = 0;
        s_auto.last_up = 0;
        s_auto.probe_s = 15;
        s_auto.docked = docked;
        return;
    }
    int steps;
    const float *scales = auto_steps(docked, &steps);
    if (!s_auto.win) s_auto.win = s1;
    if (got && !paused && s1 > s_auto.settle && g_race_phase != 1) {
        s_auto.loops++;
        if (armTicksToNs(s1 - p0) > 20800000ull) s_auto.slow++;
    }
    if (armTicksToNs(s1 - s_auto.win) < 1000000000ull) return;
    unsigned loops = s_auto.loops, slow = s_auto.slow;
    s_auto.win = s1;
    s_auto.loops = s_auto.slow = 0;
    if (loops < 30) return; /* menus, loads, pauses: no verdict */
    int was = s_auto.level;
    if (slow * 10 >= loops) {
        s_auto.quiet_s = 0;
        if (s_auto.level < steps - 1) {
            s_auto.level++;
            if (s_auto.last_up && armTicksToNs(s1 - s_auto.last_up) < 60000000000ull && s_auto.probe_s < 300) s_auto.probe_s *= 2;
            s_auto.last_up = 0;
        }
    } else if (slow == 0) {
        if (s_auto.level > 0 && ++s_auto.quiet_s >= s_auto.probe_s) {
            s_auto.level--;
            s_auto.quiet_s = 0;
            s_auto.last_up = s1;
        }
    } else s_auto.quiet_s = 0;
    if (s_auto.level != was) {
        s_auto.settle = s1 + armNsToTicks(500000000ull); /* new render targets: skip their first frames */
        rt_log("auto resolution: %.2fx -> %.2fx (%u of %u frames waited on the GPU; next step up after %u s quiet)",
               (double)scales[was], (double)scales[s_auto.level], slow, loops, s_auto.probe_s);
    }
}

/* Hitch log: a new game frame more than 1.5 refreshes after the last one (a dropped frame), with
 * what happened in between: the main loop's longest render / present / swap (r0 -> p0 -> s0 -> s1),
 * textures decoded and shader compiles there, the game thread's and the GX decoder's busy time,
 * disc reads and GPU drains. Called once per main loop; got = a new game frame was rendered.
 * At most 20 lines per 5 s. */
static void hitch_log(u64 r0, u64 p0, u64 s0, u64 s1, bool got, bool paused) {
    static struct {
        u64 tick;
        uint64_t ntex, tex_ns, cns, idle, gx, dvd, drains, drain_ns, win, max_r, max_p, max_s;
        unsigned progs, loops, lines;
    } h;
    uint64_t lr = armTicksToNs(p0 - r0), lp = armTicksToNs(s0 - p0), ls = armTicksToNs(s1 - s0);
    if (lr > h.max_r) h.max_r = lr;
    if (lp > h.max_p) h.max_p = lp;
    if (ls > h.max_s) h.max_s = ls;
    h.loops++;
    if (!got) return;
    uint64_t ntex, tex_ns, cns;
    unsigned progs;
    render_hitch_stats(&ntex, &tex_ns, &progs, &cns);
    uint64_t gap = h.tick ? armTicksToNs(s1 - h.tick) : 0, now_ns = armTicksToNs(s1);
    if (now_ns - h.win > 5000000000ull) { h.win = now_ns; h.lines = 0; }
    if (gap > 25000000ull && !paused && h.lines < 20) {
        h.lines++;
        uint64_t idle = g_guest_idle_ns - h.idle;
        rt_log("hitch: %.1f ms since the last game frame (%u loops; longest render %.1f, present %.1f, swap %.1f ms); "
               "%llu new textures (%.1f ms), %u shader compiles (%.1f ms); game thread busy %.1f ms, GX %.1f ms; "
               "%llu disc reads, %llu GPU drains (%.1f ms)",
               (double)gap / 1e6, h.loops, (double)h.max_r / 1e6, (double)h.max_p / 1e6, (double)h.max_s / 1e6,
               (unsigned long long)(ntex - h.ntex), (double)(tex_ns - h.tex_ns) / 1e6, progs - h.progs,
               (double)(cns - h.cns) / 1e6, idle < gap ? (double)(gap - idle) / 1e6 : 0.0,
               (double)(g_gx_busy_ns - h.gx) / 1e6, (unsigned long long)(g_dvd_reads - h.dvd),
               (unsigned long long)(g_gx_drains - h.drains), (double)(g_gx_drain_ns - h.drain_ns) / 1e6);
    }
    h.tick = s1;
    h.ntex = ntex; h.tex_ns = tex_ns; h.progs = progs; h.cns = cns;
    h.idle = g_guest_idle_ns; h.gx = g_gx_busy_ns; h.dvd = g_dvd_reads;
    h.drains = g_gx_drains; h.drain_ns = g_gx_drain_ns;
    h.loops = 0;
    h.max_r = h.max_p = h.max_s = 0;
}

/* The renderer's substituted pictures: the pause screen's, then the character packs' */
static unsigned subst_texture(unsigned id) { return id >= GX_SUBST_CHARPACK ? charpack_art_texture(id) : pause_art_texture(id); }

int main(int argc, char **argv) {
    update_init(argc > 0 ? argv[0] : NULL);
    mkdir("sdmc:/switch", 0777);
    mkdir(APP_DIR, 0777);
    mkdir(LOG_DIR, 0777);
    mkdir(CFG_DIR, 0777);
    mkdir(SAVE_DIR, 0777);
    mkdir(MODS_DIR, 0777);
    mkdir(MODS_DIR "/characters", 0777);
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
        /* A mod in mods/<key>: played when it loads and the menu's MODS page picks it (from here on
         * the disc is the mod's); otherwise, or if it can't be used, the standard game (the disc is
         * left as it was) */
        if (MOD_COUNT) install_mods();
        unsigned found = find_mods(), pick = mods_pick(found);
        if (found && !pick) rt_log("mod: none picked in the menu (MODS: %s); playing the standard game", g_set.mod);
        if (pick) {
            const ModInfo *mi = &MODS[pick - 1];
            char dir[256], line[96];
            snprintf(dir, sizeof dir, MODS_DIR "/%s", mi->key);
            snprintf(line, sizeof line, "LOADING %s", mi->name);
            for (char *t = line; *t; t++) *t = (char)toupper((unsigned char)*t);
            screen(line, NULL, 0.1f);
            if (disc_overlay(dir, err, sizeof err)) {
                rt_log("mod: playing %s", mi->name);
                s_mod_key = mi->key;
                snprintf(dir, sizeof dir, SAVE_DIR "/%s", mi->key);
                mkdir(dir, 0777);
                offer_save_copy(dir, mi->key);
                snprintf(line, sizeof line, "%s/slotA.bin", dir);
                memcard_set_path(line);
            } else {
                rt_log("mod: %s; playing the standard game", err);
                found &= ~(1u << (pick - 1)); /* failed to load: not offered */
                pick = 0;
                for (int i = 0; i < 180 && appletMainLoop(); i++) { /* 3 s */
                    screen("THE MOD COULD NOT BE LOADED: A FILE IS MISSING", "STARTING THE STANDARD GAME (SEE LOGS/MKDD.LOG)", 0.1f);
                }
            }
        }
        menu_set_mods(found, pick);
        pause_art_init();  /* the pause screen's controller picture, drawn as a Pro Controller */
        /* driver variants, unless EXTRA CHARACTERS is off (then nothing of them is read) */
        {
            struct stat st;
            bool found = stat(MODS_DIR "/characters", &st) == 0 && S_ISDIR(st.st_mode);
            if (found && g_set.characters) charpack_scan(MODS_DIR "/characters");
            menu_set_chars(found, found && g_set.characters);
        }
        charpack_art_init();
        render_set_subst(subst_texture);
        render_set_below(pause_art_below);
        render_set_shift(pause_art_shift);
    } else {
        rt_log("disc: %s", err);
        fatal("COULD NOT READ THE DISC IMAGE", strstr(err, "compression") ? "RVZ: CONVERT IT AGAIN IN DOLPHIN WITH ZSTANDARD"
                                                                         : "THE FILE IS DAMAGED: SEE LOGS/MKDD.LOG");
    }
    screen("STARTING", NULL, 0.3f);
    if (!render_init()) fatal("GRAPHICS INIT FAILED", "SEE MKDD.LOG");
    hdtex_open(HD_PATH);
    render_shader_cache(CFG_DIR "/shaders.bin", shader_progress);
    if (!machine_boot(disc, &s_boot, err, sizeof err)) {
        rt_log("boot failed: %s", err);
        if (disc_overlay_active()) fatal("THIS VERSION OF THE MOD IS NOT SUPPORTED", "SEE LOGS/MKDD.LOG FOR THE SUPPORTED VERSIONS");
        fatal("COULD NOT START THE GAME", "WRONG DISC: NEEDS MARIO KART DOUBLE DASH USA (GM4E01)");
    }
    if (s_mod_key && (!g_mod || strcmp(MODS[g_mod - 1].key, s_mod_key) != 0)) { /* another mod's main.dol in the folder */
        rt_log("boot: mods/%s holds %s", s_mod_key, g_mod ? MODS[g_mod - 1].name : "no mod");
        fatal("THE MOD FOLDER HOLDS ANOTHER MOD", "SEE LOGS/MKDD.LOG");
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
    /* Sampling profiler (mkdd_prof.bin, tools/prof_report.py): profiler=1 in settings.ini. Off by default:
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
         * screen. Its artwork is one 4:3 picture with the logo in it: in 16:9 it stays 4:3, whole,
         * with a blurred, dimmed copy of it filling the sides (render_set_backdrop). */
        bool title = m_read_be(0x803C9CE0u, 4) == 0, wide = g_set.wide && !title;
        g_aspect_scale = wide ? 4.0f / 3.0f : 1.0f;
        render_set_backdrop(g_set.wide && title);
        static const float FOV_SCALE[5] = {1.0f, 1.15f, 1.3f, 1.45f, 2.0f}; /* tan(fovy / 2) times: NORMAL, WIDE, WIDER, WIDEST, WIDEEE */
        g_fov_scale = FOV_SCALE[g_set.fov[g_set.wide] % 5];
        g_hud_hide = g_set.hud_hide;
        g_hud_modern = g_set.hud_style == 1;
        g_speed_kmh = g_set.speed_kmh;
        g_any_kart = g_set.any_kart;
        g_char_test = (uint32_t)g_set.char_test;
        g_laps = (uint32_t)g_set.laps;
        {
            static const float HEIGHTS[4] = {0.45f, 0.7f, 1.0f, 1.5f};
            static const float DISTS[5] = {0.55f, 0.75f, 1.0f, 1.3f, 1.6f};
            g_cam_height = HEIGHTS[g_set.cam_height & 3];
            g_cam_dist = DISTS[g_set.cam_dist % 5];
        }
        cheats_apply();
        g_hud_log = g_set.logging ? 600 : 0; /* the race HUD in the log too (bug reports) */
        static const float FP_UP[4] = {0, 70.0f, 100.0f, 140.0f}; /* CAMERA: eye height above the kart's centre */
        if (g_set.camera & 3) __atomic_or_fetch(&g_first_person, 1u, __ATOMIC_RELAXED);
        else __atomic_and_fetch(&g_first_person, ~1u, __ATOMIC_RELAXED);
        g_fp_up = FP_UP[g_set.camera & 3];
        g_fp_fwd = (float)g_set.fp_forward;
        render_set_hud43(wide);
        render_set_hd(g_set.hd_textures);
        static const float ANISO[5] = {1, 2, 4, 8, 16};
        render_set_aniso(ANISO[g_set.aniso % 5]);
        u64 r0 = armGetSystemTick();
        /* Docked: output 1920x1080, the game rendered at DOCKED RESOLUTION (2X: 1280x960 instead of
         * 640x480). Handheld: 1280x720, at HANDHELD RESOLUTION (NATIVE: the game's own size). */
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
            int scale = auto_on(docked) ? -1 : (int)(render_scale(docked) * 100.0f);
            if (cw != last_w || ch != last_h || scale != last_scale) {
                if (scale < 0) rt_log("display: %s, output %dx%d, game rendered at AUTO (%s and lower)", docked ? "docked" : "handheld", cw, ch, docked ? "2x" : "1.5x");
                else rt_log("display: %s, output %dx%d, game rendered at %.2fx", docked ? "docked" : "handheld", cw, ch, (double)scale / 100.0);
                clocks_pending = 120; /* clocks settle after a dock change: log them in 2 s */
                last_w = cw; last_h = ch; last_scale = scale;
            }
        }
        if (g_log_mask) render_gpu_timer_begin(); /* a timed frame makes the GPU drain first: only for the logs */
        bool got = render_update(render_scale(docked));
        t_render += armTicksToNs(armGetSystemTick() - r0);
        if (got) {
            rendered++;
            total_rendered++;
            fps_frames++;
            if (boost && total_rendered == 1) {
                appletSetCpuBoostMode(ApmCpuBoostMode_Normal); /* boost throttles the GPU */
                boost = false;
                rt_log("first game frame at %.1f s", (double)armTicksToNs(armGetSystemTick() - s_boot_tick) / 1e9);
                if (!g_log_mask) rt_log("logging is off (DEBUGGING > LOGGING): nothing more is logged");
                g_log_boot = false;
                diag_flush();
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
             * half second; kept while the menu pauses the game), never on the title screen (its
             * demo race: the 16:9 blurred sides, or plain bars in 4:3) */
            static uint32_t race_seen;
            static u64 race_tick;
            if (g_race_frames != race_seen) { race_seen = g_race_frames; race_tick = armGetSystemTick(); }
            static bool racing;
            if (!menu_is_open()) racing = race_tick && armTicksToNs(armGetSystemTick() - race_tick) < 500000000ull;
            if (g_set.border && !wide && !title && racing) menu_border(w, h);
            if (!menu_is_open() && charpack_select_active()) menu_char_hint(w, h);
            menu_draw(w, h, s_fps, perf);
        } else screen("BOOTING", NULL, 0.8f);
        render_gpu_timer_end();
        u64 s0 = armGetSystemTick();
        t_present += armTicksToNs(s0 - p0);
        pf_main += armTicksToNs(s0 - r0);
        pf_loops++;
        SDL_GL_SwapWindow(s_window);
        u64 s1 = armGetSystemTick();
        t_swap += armTicksToNs(s1 - s0);
        hitch_log(r0, p0, s0, s1, got, paused);
        auto_res_update(docked, p0, s1, got, paused);
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
    clocks_exit();
    apmExit();
    diag_flush();
    SDL_Quit();
    /* The game, guest and decoder threads would keep running on memory the loader takes back. */
    plat_pause_threads();
    exit_memory_report();
    _Exit(0);
}
