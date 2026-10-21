/* Headless PC test harness: boots the recompiled game with no window or audio device and
 * reports progress (frames presented, audio sub-frames, threads), like MKDD-Recomp's preflight.
 *
 * Usage: mkdd_pc <disc image> [seconds] [pad script]
 *   pad script: "frames:hexmask,..." holds buttons on port 0 for that many 1/60 s steps
 *   (e.g. "300:0,10:1000,120:0,10:100" = wait 5 s, Start, wait 2 s, A). */
#include <stdlib.h>
#include <string.h>
#include "../runtime/gx.h"
#include "../runtime/rt.h"
#include "../switch/tev_gen.h"

extern float g_aspect_scale;
static Ctx s_boot;
void prof_start(void);
void prof_enable(bool on);
void prof_dump(const char *path);

static void game_thread(void *arg) {
    (void)arg;
    machine_run(&s_boot);
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s <disc image> [seconds] [pad script]\n", argv[0]);
        return 2;
    }
    int secs = argc > 2 ? atoi(argv[2]) : 60;
    const char *script = argc > 3 ? argv[3] : "";
    diag_set_log_file("mkdd_pc.log");
    g_log_boot = false; /* logs everything anyway; no write-through per line */
    /* MKDD_MOD=<folder>: play a mod (machine_boot), with its own saves */
    memcard_set_path(getenv("MKDD_MOD") ? "memcard_mod/slotA.bin" : "memcard/slotA.bin");
    char err[512];
    if (!machine_boot(argv[1], &s_boot, err, sizeof err)) {
        fprintf(stderr, "boot failed: %s\n", err);
        return 1;
    }
    diag_start_watchdog("mkdd_trace.log");
    if (getenv("MKDD_WIDE")) g_aspect_scale = 4.0f / 3.0f;
    if (getenv("MKDD_HUD_HIDE")) g_hud_hide = (uint32_t)strtoul(getenv("MKDD_HUD_HIDE"), NULL, 0);
    PadStatus pad = {0};
    pad.connected = true;
    pad_set(0, &pad);
    /* MKDD_PROF=<seconds>: sample all threads from then on, write prof.txt at exit */
    const char *pe = getenv("MKDD_PROF");
    int prof_at = pe ? atoi(pe) : -1;
    bool prof_on = false;
    if (prof_at >= 0) prof_start();
    pthread_start("game", game_thread, NULL, 32u << 20, -1, 0);

    uint64_t start = plat_now_ns();
    const char *sp = script;
    long seg_left = 0;
    unsigned seg_mask = 0;
    int16_t silence[2 * 1024];
    for (int step = 0;; step++) {
        plat_sleep_ns(1000000000ull / 60);
        /* drain audio so the jitter buffer behaves like a real consumer */
        audio_pop(silence, audio_rate() / 60);
        if (seg_left <= 0 && *sp) {
            seg_left = strtol(sp, (char **)&sp, 10);
            if (*sp == ':') seg_mask = (unsigned)strtoul(sp + 1, (char **)&sp, 16);
            if (*sp == ',') sp++;
            rt_log("pad script: step %d mask %#06x for %ld", step, seg_mask, seg_left);
        }
        if (seg_left > 0) seg_left--;
        else seg_mask = 0;
        pad.button = (uint16_t)seg_mask;
        pad_set(0, &pad);
        /* MKDD_DUMP_TEV=dir: write the specialised shader of every TEV configuration seen */
        static U64Map seen;
        static unsigned nseen;
        const char *dump = getenv("MKDD_DUMP_TEV");
        if (dump && step % 30 == 0) {
            Frame *f = gx_frame_acquire();
            for (size_t i = 0; f && i < f->batches.len; i++) {
                uint64_t k = tev_shader_key(&f->batches.data[i].tev);
                if (u64map_get(&seen, k, NULL)) continue;
                u64map_put(&seen, k, 1);
                static char src[65536];
                size_t len = tev_gen_fs(&f->batches.data[i].tev, src, sizeof src);
                char path[512];
                snprintf(path, sizeof path, "%s/tev_%04u.frag", dump, nseen++);
                plat_write_file_atomic(path, src, len);
            }
            gx_frame_release(f);
        }
        /* MKDD_DUMP_FRAME=<seconds>: the frame at that time as frame.txt (gx_dump_frame) */
        if (getenv("MKDD_DUMP_FRAME") && step == atoi(getenv("MKDD_DUMP_FRAME")) * 60) gx_dump_frame("frame.txt");
        /* MKDD_WIDE_AT=<seconds>: switch to 16:9 mid-run (the widescreen change while playing) */
        if (getenv("MKDD_WIDE_AT") && step == atoi(getenv("MKDD_WIDE_AT")) * 60) g_aspect_scale = 4.0f / 3.0f;
        if (step % 300 == 0) {
            Frame *f = gx_frame_acquire();
            fprintf(stderr, "[t=%5.1fs] frames %llu  batches %zu  verts %zu  dsp subframes %llu  voices %llu  AI buffers %llu  fn entries %llu  last %s\n",
                    (double)(plat_now_ns() - start) / 1e9, (unsigned long long)gx_frame_count(), f ? f->batches.len : 0,
                    f ? (size_t)f->nverts : 0, (unsigned long long)g_dsp_subframes, (unsigned long long)g_dsp_voices_last,
                    (unsigned long long)g_ai_buffers, (unsigned long long)g_fn_entries, m_name(g_last_fn));
            gx_frame_release(f);
        }
        if (prof_at >= 0 && !prof_on && (plat_now_ns() - start) / 1000000000ull >= (uint64_t)prof_at) {
            prof_on = true;
            prof_enable(true);
            rt_log("profiling from here");
        }
        if ((plat_now_ns() - start) / 1000000000ull >= (uint64_t)secs) break;
    }
    if (prof_at >= 0) prof_dump("prof.txt");
    diag_flush();
    char xr[128];
    gx_xf_report(xr, sizeof xr);
    fprintf(stderr, "%s\n", xr);
    fprintf(stderr, "done: %llu frames presented\n", (unsigned long long)gx_frame_count());
    fflush(stderr);
    _Exit(0);
}
