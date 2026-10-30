/* Headless PC test harness: boots the recompiled game with no window or audio device and
 * reports progress (frames presented, audio sub-frames, threads), like MKDD-Recomp's preflight.
 *
 * Usage: mkdd_pc <disc image> [seconds] [pad script]
 *   pad script: "frames:hexmask,..." holds buttons on port 0 for that many 1/60 s steps
 *   (e.g. "300:0,10:1000,120:0,10:100" = wait 5 s, Start, wait 2 s, A); bits above the buttons
 *   push the main stick: 0x10000 up, 0x20000 down, 0x40000 left, 0x80000 right.
 * MKDD_CHARS=<folder>: driver variants (character folders), as the Switch's mods/characters. */
#include <stdlib.h>
#include <string.h>
#include "../runtime/gx.h"
#include "../runtime/rt.h"
#include "../switch/tev_gen.h"
#include "../runtime/route.h"

/* no built-in battle routes here (the Switch build embeds them): routes/<key>.txt only */
const struct route_file route_builtin[] = {{0}};

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
    /* MKDD_CHARS=<folder>: driver variants from it, as the Switch's mods/characters */
    if (getenv("MKDD_CHARS")) charpack_scan(getenv("MKDD_CHARS"));
    char err[512];
    if (!machine_boot(argv[1], &s_boot, err, sizeof err)) {
        fprintf(stderr, "boot failed: %s\n", err);
        return 1;
    }
    diag_start_watchdog("mkdd_trace.log");
    if (getenv("MKDD_WIDE")) g_aspect_scale = 4.0f / 3.0f;
    if (getenv("MKDD_HUD_HIDE")) g_hud_hide = (uint32_t)strtoul(getenv("MKDD_HUD_HIDE"), NULL, 0);
    /* MKDD_FP=<eye height>,<eye forward>: first-person camera */
    if (getenv("MKDD_FP")) {
        g_first_person = 1;
        sscanf(getenv("MKDD_FP"), "%f,%f", &g_fp_up, &g_fp_fwd);
    }
    if (getenv("MKDD_HUD_MODERN")) g_hud_modern = 1;
    if (getenv("MKDD_KMH")) g_speed_kmh = 1;
    if (getenv("MKDD_VSPLIT")) g_vsplit = 1;
    if (getenv("MKDD_LAN")) g_lan_on = 1; /* a Broadband Adapter (eth.c) */
    if (getenv("MKDD_LAN_PORT")) g_lan_port = (uint32_t)atoi(getenv("MKDD_LAN_PORT")); /* two on one PC: one each */
    if (getenv("MKDD_LAN_PEER")) snprintf(g_lan_peer, sizeof g_lan_peer, "%s", getenv("MKDD_LAN_PEER"));
    if (getenv("MKDD_HUD_LOG")) { extern volatile uint32_t g_hud_log; g_hud_log = (uint32_t)atoi(getenv("MKDD_HUD_LOG")); }
    if (getenv("MKDD_LOOK_BACK")) g_look_back = 1; /* the race camera turned around throughout */
    PadStatus pad = {0};
    pad.connected = true;
    pad_set(0, &pad);
    if (getenv("MKDD_PAD2")) pad_set(1, &pad); /* connected from the start, as port 1 */
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
        /* X / Y pressed: the select screen's next / previous page of characters, as the Switch's front end */
        if (seg_mask & ~pad.button & 0x0400) __atomic_or_fetch(&g_char_cycle, 1u, __ATOMIC_RELEASE);
        if (seg_mask & ~pad.button & 0x0800) __atomic_or_fetch(&g_char_cycle, 1u << 4, __ATOMIC_RELEASE);
        pad.button = (uint16_t)seg_mask;
        pad.stick_y = seg_mask & 0x10000 ? 100 : seg_mask & 0x20000 ? -100 : 0;
        pad.stick_x = seg_mask & 0x80000 ? 100 : seg_mask & 0x40000 ? -100 : 0;
        pad_set(0, &pad);
        /* LAN play's state each second (retail singletons: AppMgr's apps, NetGateApp, NetGameMgr, BBAMgr) */
        if (g_lan_on && step % 60 == 0) {
            uint32_t gate = (uint32_t)m_read_be(0x803CBE28, 4), net = (uint32_t)m_read_be(0x803CBE18, 4), bba = (uint32_t)m_read_be(0x803CBE68, 4);
            rt_log("lan: app %u next %u, gate %#x state %d menu %d, net %#x state %d, bba %#x state %d/%d", (unsigned)m_read_be(0x803CBDA0, 4),
                   (unsigned)m_read_be(0x803CBDA4, 4), gate, gate ? (int)m_read_be(gate + 0x10, 4) : -1, gate ? (int)m_read_be(gate + 0x14, 4) : -1,
                   net, net ? (int)m_read_be(net + 0x96C, 4) : -1, bba, bba ? (int)m_read_be(bba, 4) : -1, bba ? (int)m_read_be(bba + 4, 4) : -1);
            rt_log("lan: ssdp state %d queue %#x", (int)m_read_be(0x803CB5A0, 4), (unsigned)m_read_be(0x803CB5A4, 4));
        }
        if (getenv("MKDD_PAD2")) pad_set(1, &pad); /* a second pad pressing the same buttons (two players) */
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
        /* MKDD_DUMP_FRAME=<seconds>[,<seconds>...]: the frame at each time as frame.txt, then
         * frame_<seconds>.txt for the later ones (gx_dump_frame) */
        if (getenv("MKDD_DUMP_FRAME") && step % 60 == 0) {
            const char *d = getenv("MKDD_DUMP_FRAME");
            for (bool first = true; *d; first = false) {
                int s = atoi(d);
                if (step == s * 60) {
                    char path[64];
                    if (first) snprintf(path, sizeof path, "frame.txt");
                    else snprintf(path, sizeof path, "frame_%d.txt", s);
                    gx_dump_frame(path);
                }
                while (*d && *d != ',') d++;
                if (*d) d++;
            }
        }
        /* MKDD_WIDE_AT=<seconds>: switch to 16:9 mid-run (the widescreen change while playing) */
        if (getenv("MKDD_WIDE_AT") && step == atoi(getenv("MKDD_WIDE_AT")) * 60) g_aspect_scale = 4.0f / 3.0f;
        /* MKDD_MODERN_OFF=<race frame>: MODERN off from then (its slide back, as after the finish) */
        extern volatile uint32_t g_race_frames;
        if (getenv("MKDD_MODERN_OFF") && g_race_frames >= (uint32_t)atoi(getenv("MKDD_MODERN_OFF"))) g_hud_modern = 0;
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
