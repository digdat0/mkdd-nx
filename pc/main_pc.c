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
    /* MKDD_MOD=<folder>: play a mod (machine_boot), with its own saves */
    memcard_set_path(getenv("MKDD_MOD") ? "memcard_mod/slotA.bin" : "memcard/slotA.bin");
    char err[512];
    if (!machine_boot(argv[1], &s_boot, err, sizeof err)) {
        fprintf(stderr, "boot failed: %s\n", err);
        return 1;
    }
    diag_start_watchdog("mkdd_trace.log");
    if (getenv("MKDD_WIDE")) g_aspect_scale = 4.0f / 3.0f;
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
        /* MKDD_RAW=path: the popped audio as raw s16 stereo (TEMP, for click analysis) */
        static FILE *raw;
        if (!raw && getenv("MKDD_RAW")) raw = fopen(getenv("MKDD_RAW"), "wb");
        if (raw) fwrite(silence, 4, audio_rate() / 60, raw);
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
        /* MKDD_DUMP4P=n: dump n frames of the 4-player demo (45 EFB copies a frame) as frame4p_k.txt */
        if (getenv("MKDD_DUMP4P") && step % 120 == 0) {
            static int done;
            Frame *f = gx_frame_acquire();
            bool four = f && f->copies.len == 45;
            if (f) gx_frame_release(f);
            if (four && done < atoi(getenv("MKDD_DUMP4P"))) { char p[64]; snprintf(p, sizeof p, "frame4p_%d.txt", ++done); gx_dump_frame(p); }
        }
        if (getenv("MKDD_WIDE_AT") && step == atoi(getenv("MKDD_WIDE_AT")) * 60) g_aspect_scale = 4.0f / 3.0f;
        if (getenv("MKDD_ALPHASTAT") && step % 300 == 0) {
            Frame *f = gx_frame_acquire();
            unsigned fmt[8] = {0}, dsta = 0, dstw = 0, cdst = 0, aw = 0;
            for (size_t i = 0; f && i < f->batches.len; i++) {
                const Batch *b = &f->batches.data[i];
                uint32_t bl = b->tev.pipe[1], p3 = b->tev.pipe[3];
                fmt[p3 & 7]++;
                unsigned sf = (bl >> 8) & 7, df = (bl >> 5) & 7;
                if ((bl & 1) && (sf >= 6 || df >= 6)) { dsta++; if (dsta < 4) rt_log("ALPHASTAT dst-alpha batch: blend %#x fmt %u z %#x verts %u", bl, p3 & 7, b->tev.pipe[0], b->vert_count); }
                if ((bl >> 4) & 1) aw++;
                if (p3 & 0x10000) { cdst++; if (bl & 1) dstw++; }
            }
            for (size_t i = 0; f && i < f->copies.len && i < 8; i++) {
                const EfbCopy *e = &f->copies.data[i];
                uint32_t cmd = e->bp52;
                rt_log("ALPHASTAT copy %zu at batch %u: fmt %u xfb %u clear %u src %ux%u pixfmt %u", i, e->at_batch,
                       ((cmd >> 4) & 7) | ((cmd >> 3) & 1) << 3, (cmd >> 14) & 1, (cmd >> 11) & 1,
                       (e->size & 0x3ff) + 1, ((e->size >> 10) & 0x3ff) + 1, e->regs[0] & 7);
            }
            {
                unsigned zf[8] = {0};
                for (size_t i = 0; f && i < f->batches.len; i++) {
                    uint32_t z = f->batches.data[i].tev.pipe[0];
                    if (z & 1) zf[(z >> 1) & 7]++;
                }
                rt_log("ALPHASTAT zfunc never %u less %u equal %u lequal %u greater %u notequal %u gequal %u always %u",
                       zf[0], zf[1], zf[2], zf[3], zf[4], zf[5], zf[6], zf[7]);
                for (size_t i = 0, k = 0; f && i < f->batches.len && k < 6; i++) {
                    const Batch *bb = &f->batches.data[i];
                    uint32_t z = bb->tev.pipe[0], zf2 = (z >> 1) & 7;
                    if ((z & 1) && (zf2 == 2 || zf2 == 4 || zf2 == 5 || zf2 == 6)) {
                        k++;
                        rt_log("ALPHASTAT special z batch %zu: z %#x blend %#x verts %u stages %u konst0 %.2f %.2f %.2f %.2f", i, z, bb->tev.pipe[1], bb->vert_count,
                               bb->tev.params[0], bb->tev.konst[0][0], bb->tev.konst[0][1], bb->tev.konst[0][2], bb->tev.konst[0][3]);
                    }
                }
            }
            {
                static int dumped;
                if (f && f->batches.len > 250 && !dumped++) {
                    FILE *o = fopen("race_batches.txt", "w");
                    for (size_t i = 0; o && i < f->batches.len; i++) {
                        const Batch *bb = &f->batches.data[i];
                        float x0 = 9, x1 = -9, y0 = 9, y1 = -9;
                        for (uint32_t t = 0; bb->hud && t < bb->index_count; t++) {
                            GpuVertex e, in;
                            gx_vertex_unpack(f, bb, f->indices.data[bb->index_first + t], &in);
                            gx_xf_eval(f->xf.data, &in, &e);
                            float x = e.pos[0] / e.pos[3], y = e.pos[1] / e.pos[3];
                            if (x < x0) x0 = x;
                            if (x > x1) x1 = x;
                            if (y < y0) y0 = y;
                            if (y > y1) y1 = y;
                        }
                        if (bb->index_count) {
                            GpuVertex v0, *gv = &v0;
                            gx_vertex_unpack(f, bb, f->indices.data[bb->index_first], gv);
                            if (gv->rec != XV_PRE) {
                                const float *R = (const float *)(f->xf.data + 4 * (gv->rec & XV_REC_MASK));
                                fprintf(o, "     proj %s sx %.4f sy %.4f\n",(((const uint32_t *)R)[12] & 1) ? "ortho" : "persp", R[0], R[2]);
                            }
                        }
                        fprintf(o, "%4zu z %#4x blend %#6x cull %u fmt %#x verts %5u stages %u hud %u x %.2f..%.2f y %.2f..%.2f\n", i, bb->tev.pipe[0], bb->tev.pipe[1],
                                bb->tev.pipe[2], bb->tev.pipe[3], bb->vert_count, bb->tev.params[0], bb->hud, x0, x1, y0, y1);
                    }
                    if (o) fclose(o);
                }
            }
            for (size_t i = 0; f && i < f->batches.len; i++) {
                const Batch *bb = &f->batches.data[i];
                if (!bb->index_count) continue;
                GpuVertex v0, *gv = &v0;
                gx_vertex_unpack(f, bb, f->indices.data[bb->index_first], gv);
                if (gv->rec == XV_PRE) continue;
                const float *R = (const float *)(f->xf.data + 4 * (gv->rec & XV_REC_MASK));
                if (((const uint32_t *)R)[12] & 1) continue;
                rt_log("ALPHASTAT step %d aspect scale %.3f first persp sx %.4f sy %.4f", step, g_aspect_scale, R[0], R[2]);
                break;
            }
            rt_log("ALPHASTAT batches %zu: fmt0 %u fmt1 %u fmt2 %u fmt other %u; dst-alpha blends %u; alpha writes %u; const dst alpha %u (blended %u)",
                   f ? f->batches.len : 0, fmt[0], fmt[1], fmt[2], fmt[3] + fmt[4] + fmt[5] + fmt[6] + fmt[7], dsta, aw, cdst, dstw);
            gx_frame_release(f);
        }
        /* MKDD_CULLSTAT: screen-space winding of culled batches' triangles (area-weighted, y up) */
        if (getenv("MKDD_CULLSTAT") && step % 300 == 0) {
            Frame *f = gx_frame_acquire();
            double area[3][2] = {{0}};
            unsigned cnt[3][2] = {{0}};
            for (size_t i = 0; f && i < f->batches.len; i++) {
                const Batch *b = &f->batches.data[i];
                unsigned cm = b->tev.pipe[2] & 3;
                if (cm == 3) continue;
                for (uint32_t t = 0; t + 2 < b->index_count; t += 3) {
                    GpuVertex e[3];
                    bool ok = true;
                    for (int k = 0; k < 3; k++) {
                        GpuVertex in;
                        gx_vertex_unpack(f, b, f->indices.data[b->index_first + t + k], &in);
                        gx_xf_eval(f->xf.data, &in, &e[k]);
                        ok &= e[k].pos[3] > 1e-6f;
                    }
                    if (!ok) continue;
                    float x[3], y[3];
                    for (int k = 0; k < 3; k++) { x[k] = e[k].pos[0] / e[k].pos[3]; y[k] = e[k].pos[1] / e[k].pos[3]; }
                    bool on = true;
                    for (int k = 0; k < 3; k++) on &= x[k] > -1.2f && x[k] < 1.2f && y[k] > -1.2f && y[k] < 1.2f;
                    if (!on) continue;
                    double a = (double)(x[1] - x[0]) * (y[2] - y[0]) - (double)(x[2] - x[0]) * (y[1] - y[0]);
                    int ccw = a > 0;
                    area[cm][ccw] += a > 0 ? a : -a;
                    cnt[cm][ccw]++;
                }
            }
            if (f) fprintf(stderr, "cullstat: none cw %u/%.2f ccw %u/%.2f | back cw %u/%.2f ccw %u/%.2f | front cw %u/%.2f ccw %u/%.2f\n",
                           cnt[0][0], area[0][0], cnt[0][1], area[0][1], cnt[1][0], area[1][0], cnt[1][1], area[1][1],
                           cnt[2][0], area[2][0], cnt[2][1], area[2][1]);
            gx_frame_release(f);
        }
        if (step % 300 == 0) {
            Frame *f = gx_frame_acquire();
            fprintf(stderr, "[t=%5.1fs] frames %llu  batches %zu  verts %zu  dsp subframes %llu  voices %llu  AI buffers %llu  fn entries %llu  last %s\n",
                    (double)(plat_now_ns() - start) / 1e9, (unsigned long long)gx_frame_count(), f ? f->batches.len : 0,
                    f ? (size_t)f->nverts : 0, (unsigned long long)g_dsp_subframes, (unsigned long long)g_dsp_voices_last,
                    (unsigned long long)g_ai_buffers, (unsigned long long)g_fn_entries, m_name(g_last_fn));
            gx_frame_release(f);
        }
        if (prof_at >= 0 && !prof_on && (plat_now_ns() - start) / 1000000000ull >= (uint64_t)prof_at) { prof_on = true; prof_enable(true); rt_log("profiling from here"); } if (0) { prof_enable(true); rt_log("profiling from here"); }
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
