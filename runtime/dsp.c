/* High-level emulation of MKDD's audio DSP microcode (JAudio / "Zelda-family" ucode).
 * C port of gc-runtime's dsp.rs.
 *
 * JAudio talks to the DSP through DSPSendCommands2 (hooked in hle.c):
 *  0x81 DsetupTable: channel block array (64 x 0x180-byte JASDsp::TChannel), filter tables.
 *  0x82 DsyncFrame: render N sub-frames of 80 samples into left/right output buffers. Like the
 *       real DSP, each rendered sub-frame is reported with mail 0xF355FF00 and a DSP interrupt,
 *       which runs JAudio's DSP callback (captured from DspBoot); that paces the audio thread.
 *  Release halt (count 0, group in the high half): JASDSPChannel::updateAll releases the voice
 *       groups 0-3 as it finishes updating them. The DSP renders a sub-frame only after group 3,
 *       so it never runs ahead of the CPU's per-sub-frame channel updates (streams depend on it:
 *       JASAramStream moves its loop point a few sub-frames ahead and gives up for good,
 *       sFatalErrorFlag, when playback has already passed it).
 * Mixing: AFC 4-bit/2-bit ADPCM and 8/16-bit PCM from ARAM, pitch (4.12 fixed point), loops with
 * stored predictor history, main-bus mixer volumes or the auto-mixer, the finished flag. A wave a
 * character pack replaces plays the pack's samples instead (charvoice.c).
 * Not emulated: FX/reverb buses, IIR/FIR filters, oscillators, Dolby surround.
 *
 * Sub-frames render in the DSP interrupt. MKDD_DSP_ASYNC=1 (not the native verifier, which
 * snapshots RAM around calls) renders on a worker thread (core 2) instead: a release hands the
 * sub-frame to it, and its completion mail raises the DSP interrupt. Off by default: on the Switch
 * the audio crackled badly with it, although pacing counters looked normal, probably
 * because each sub-frame waits for a thread wake and an interrupt round trip, so a frame can be
 * incomplete when JAudio mixes it into the AI buffer. */
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "rt.h"

#define CH_SIZE 0x180
#define SUBFRAME 80
#define BUS_MAIN_L 0x0D00
#define BUS_MAIN_R 0x0D60
#define MAIL_FRAME 0xF355FF00u

typedef struct {
    double frac;          /* resampling position between prev and cur */
    int32_t prev, cur;
    int16_t samples[16];  /* decoded AFC block */
    unsigned idx, valid;  /* next sample to take, end of valid samples */
    unsigned skip;        /* samples to drop from the next decoded block */
    int32_t h0, h1;       /* ADPCM history */
    CharVoiceSound rep;   /* a character's own sound in place of this wave (charvoice.c), pcm NULL none */
    uint32_t rep_pos;
} Voice;

extern int g_native_verify; /* native.c: its RAM snapshots need the DSP synchronous */
uint32_t g_dsp_callback;
uint64_t g_dsp_subframes, g_dsp_voices_last;
static uint32_t s_ch_buf, s_channels, s_adpcm_table;
static Voice *s_voices;
static bool s_frame, s_released;
static uint32_t s_left, s_right, s_subframes, s_next;
static float s_level;
static uint32_t s_mail[256];
static unsigned s_mail_head, s_mail_len;
static bool s_active_seen;
/* Worker: s_dl guards the mail queue and the frame state (s_frame .. s_level) while it runs. */
static bool s_async, s_job;
static PMutex s_dl;
static PCond s_dcv;
static void lock(void) { if (s_async) pmutex_lock(&s_dl); }
static void unlock(void) { if (s_async) pmutex_unlock(&s_dl); }

bool dsp_mail_peek(uint32_t *m) {
    lock();
    bool any = s_mail_len != 0;
    if (any) *m = s_mail[s_mail_head];
    unlock();
    return any;
}
bool dsp_mail_pop(uint32_t *m) {
    lock();
    bool any = s_mail_len != 0;
    if (any) {
        *m = s_mail[s_mail_head];
        s_mail_head = (s_mail_head + 1) & 255;
        s_mail_len--;
    }
    unlock();
    return any;
}
static void mail_push(uint32_t m) {
    if (s_mail_len == 256) return;
    s_mail[(s_mail_head + s_mail_len) & 255] = m;
    s_mail_len++;
}

static void voice_reset(Voice *v) {
    memset(v, 0, sizeof *v);
    v->idx = 16;
    v->cur = INT32_MIN;
}

static bool step(void);
static void worker(void *arg) {
    (void)arg;
    pmutex_lock(&s_dl);
    for (;;) {
        while (!s_job) pcond_wait(&s_dcv, &s_dl);
        s_job = false;
        if (step()) { /* drops the lock while mixing */
            pmutex_unlock(&s_dl);
            sched_raise(IRQ_DSP_DSP);
            pmutex_lock(&s_dl);
        }
    }
}

/* Process a DSPSendCommands2(cmds, count, cb) call. True if the caller should raise the DSP
 * interrupt (a released sub-frame to render in it). */
bool dsp_command(uint32_t cmds, uint32_t count) {
    uint32_t cmd = (uint32_t)m_read_be(cmds, 4);
    if (!count) { /* release halt: render the pending sub-frame once the last group is updated */
        lock();
        bool go = (cmd >> 16) == 3 && s_frame;
        if (go) {
            s_released = true;
            if (s_async) {
                s_job = true;
                pcond_signal(&s_dcv);
            }
        }
        unlock();
        return go && !s_async;
    }
    switch (cmd >> 24) {
    case 0x81:
        if (!s_async && plat_getenv("MKDD_DSP_ASYNC") && !g_native_verify) {
            pmutex_init(&s_dl);
            pcond_init(&s_dcv);
            s_async = pthread_start("mkdd-dsp", worker, NULL, 256 << 10, 2, 0);
        }
        lock();
        s_channels = (cmd & 0xFFFF) > 256 ? 256 : cmd & 0xFFFF;
        s_ch_buf = (uint32_t)m_read_be(cmds + 4, 4);
        s_adpcm_table = (uint32_t)m_read_be(cmds + 12, 4);
        free(s_voices);
        s_voices = malloc(s_channels * sizeof *s_voices);
        for (uint32_t i = 0; i < s_channels; i++) voice_reset(&s_voices[i]);
        unlock();
        rt_log("DSP: setup %u channels at %#010x%s", s_channels, s_ch_buf, s_async ? " (rendering on a worker)" : "");
        return false;
    case 0x82:
        lock();
        s_subframes = (cmd >> 16) & 0xFF;
        s_level = (float)(cmd & 0xFFFF) / (float)0x4000;
        s_left = (uint32_t)m_read_be(cmds + 4, 4);
        s_right = (uint32_t)m_read_be(cmds + 8, 4);
        s_next = 0;
        s_frame = s_subframes > 0;
        s_released = false; /* sub-frame 0 waits for updateDSP's release, which follows */
        unlock();
        return false;
    default:
        rt_log("DSP: unhandled command %#04x (%#010x)", cmd >> 24, cmd);
        return false;
    }
}

static inline int32_t clampi(int32_t v, int32_t lo, int32_t hi) { return v < lo ? lo : v > hi ? hi : v; }

/* Decode one AFC block (9 bytes = 4-bit, 5 bytes = 2-bit) into 16 samples. */
static void decode_afc(uint32_t a, uint32_t bytes, int32_t *h0, int32_t *h1, int16_t out[16]) {
    memset(out, 0, 32);
    if ((uint64_t)a + bytes > ARAM_SIZE) return;
    const uint8_t *p = g_aram + a;
    uint8_t header = p[0];
    int32_t delta = 1 << ((header >> 4) & 0xF);
    uint32_t ci = s_adpcm_table + 4 * (header & 0xF);
    int32_t c0 = (int16_t)m_read_be(ci, 2), c1 = (int16_t)m_read_be(ci + 2, 2);
    for (int i = 0; i < 16; i++) {
        int32_t val, shift;
        if (bytes == 9) {
            uint8_t b = p[1 + i / 2];
            uint8_t nib = i & 1 ? b & 15 : b >> 4;
            val = (int32_t)((uint32_t)nib << 28) >> 28;
            shift = 11;
        } else {
            uint8_t b = p[1 + i / 4];
            uint8_t v = (b >> (6 - 2 * (i % 4))) & 3;
            val = (int32_t)((uint32_t)v << 30) >> 30;
            shift = 13;
        }
        int32_t s = (int32_t)((uint32_t)(delta * val) << shift);
        s += *h0 * c0 + *h1 * c1;
        s >>= 11;
        s = clampi(s, -32768, 32767);
        *h1 = *h0;
        *h0 = s;
        out[i] = (int16_t)s;
    }
}

typedef struct {
    bool afc, looped;
    uint32_t wave, block_bytes, pcm_bytes, loop_pos, loop_end;
    int32_t loop_h0, loop_h1;
} Src;

static uint32_t addr_of(const Src *s, uint32_t pos) {
    return s->afc ? s->wave + pos / 16 * s->block_bytes : s->wave + pos * s->pcm_bytes;
}

/* Next input sample, advancing the ARAM address / remaining count like the ucode (semantics as
 * in Dolphin's ZeldaAudioRenderer): AFC "remaining" counts samples decoded, not played; at the
 * end of a looping voice the source restarts at the loop block with the loop history. */
static bool fetch(const Src *s, Voice *v, uint32_t *addr, uint32_t *remaining, int32_t *out) {
    if (v->rep.pcm) { /* never loops, like the MKDD Patcher's */
        if (v->rep_pos >= v->rep.n) return false;
        *out = v->rep.pcm[v->rep_pos++];
        *remaining = v->rep.n - v->rep_pos;
        return true;
    }
    if (s->afc) {
        if (v->idx >= v->valid) {
            if (*remaining == 0) {
                if (!s->looped) return false;
                *addr = s->wave + (s->loop_pos >> 4) * s->block_bytes;
                v->h0 = s->loop_h0;
                v->h1 = s->loop_h1;
                decode_afc(*addr, s->block_bytes, &v->h0, &v->h1, v->samples);
                *addr += s->block_bytes;
                uint32_t cached = 16 - (s->loop_pos & 0xF);
                v->idx = s->loop_pos & 0xF;
                v->valid = 16;
                v->skip = 0;
                *remaining = s->loop_end - cached - s->loop_pos;
            } else {
                decode_afc(*addr, s->block_bytes, &v->h0, &v->h1, v->samples);
                *addr += s->block_bytes;
                unsigned start = v->skip < 15 ? v->skip : 15;
                v->skip = 0;
                uint32_t n = 16 - start < *remaining ? 16 - start : *remaining;
                v->idx = start;
                v->valid = start + n;
                *remaining -= n;
            }
        }
        *out = v->samples[v->idx++];
        return true;
    }
    if (*remaining == 0) {
        if (!s->looped) return false;
        *addr = addr_of(s, s->loop_pos);
        *remaining = s->loop_end - s->loop_pos;
    }
    (*remaining)--;
    uint32_t a = *addr;
    *addr += s->pcm_bytes;
    if (s->pcm_bytes == 1) *out = a < ARAM_SIZE ? (int32_t)(int8_t)g_aram[a] << 8 : 0;
    else *out = a + 1 < ARAM_SIZE ? (int16_t)(g_aram[a] << 8 | g_aram[a + 1]) : 0;
    return true;
}

/* Mix one channel into the sub-frame; true if it produced samples. */
static bool mix_channel(uint32_t ch, float *out_l, float *out_r) {
    uint32_t b = s_ch_buf + ch * CH_SIZE;
#define R16(o) ((uint16_t)m_read_be(b + (o), 2))
#define R32(o) ((uint32_t)m_read_be(b + (o), 4))
#define W16(o, v) m_write_be(b + (o), 2, (uint16_t)(v))
    if (R16(0x00) == 0 || R16(0x0C) != 0) return false; /* inactive or paused */
    if (!s_active_seen) {
        s_active_seen = true;
        rt_log("DSP: first active channel %u: pitch %#x wave %#x blockbytes %u loop %u", ch, R16(0x04), R32(0x118), R16(0x100), R16(0x102));
    }
    if (R16(0x10A) != 0) { W16(0x02, 1); return false; } /* forced stop: report finished */
    Src s = {0};
    s.wave = R32(0x118);
    /* Sample source type (VPB +0x100), for ARAM sources also bytes per 16 samples:
     * 5 / 9 = AFC (2- / 4-bit), 8 = PCM8, 16 = PCM16. */
    s.block_bytes = R16(0x100);
    switch (s.block_bytes) {
    case 5: case 9: s.afc = true; break;
    case 8: s.pcm_bytes = 1; break;
    case 16: s.pcm_bytes = 2; break;
    default: return false;
    }
    if (!s.wave) return false;
    s.looped = R16(0x102) != 0;
    double pitch = R16(0x04) / 4096.0;

    /* Volumes. A voice either uses the automatic (Dolby) mixer, flag +0x58: volume +0x56 and
     * pan +0x50 (high byte, 0..127), and then the mixer slots at +0x10 hold other data (they are
     * neither read nor written); or explicit main-bus mixers. Like the ucode, an explicit mixer
     * moves from its current volume (+4) to the target (+2) across the sub-frame instead of
     * jumping there: sharp changes (music ducking, panned sounds) stepped every 2.5 ms before. */
    float vl = 0, vr = 0, vl0 = 0, vr0 = 0;
    if (R16(0x58) != 0) {
        float vol = R16(0x56) / 32767.0f;
        float pan = (float)(R16(0x50) >> 8) / 127.0f;
        pan = pan < 0 ? 0 : pan > 1 ? 1 : pan;
        vl = vl0 = vol * sqrtf(1 - pan);
        vr = vr0 = vol * sqrtf(pan);
    } else {
        for (uint32_t i = 0; i < 4; i++) {
            uint32_t mo = 0x10 + 8 * i;
            float target = (float)(int16_t)R16(mo + 2) / 32767.0f;
            float current = (float)(int16_t)R16(mo + 4) / 32767.0f;
            W16(mo + 4, R16(mo + 2)); /* current volume reaches target */
            uint16_t bus = R16(mo);
            if (bus == BUS_MAIN_L) { vl += target; vl0 += current; }
            else if (bus == BUS_MAIN_R) { vr += target; vr0 += current; }
        }
    }
    float dl = (vl - vl0) / SUBFRAME, dr = (vr - vr0) / SUBFRAME;

    /* Playback state lives in the channel block like on the real DSP: JAudio reads and rewrites
     * it between sub-frames (0x70 ARAM address, 0x74 samples left, 0x110 loop pos, 0x114 end). */
    Voice *v = &s_voices[ch];
    uint32_t addr, remaining;
    if (R16(0x08) != 0) {
        uint32_t start = R32(0x68);
        voice_reset(v);
        v->skip = s.afc ? start % 16 : 0;
        addr = addr_of(&s, start);
        remaining = R32(0x114) - start;
        W16(0x08, 0);
        if (g_charvoice_on && !start && charvoice_match(s.wave, &v->rep)) remaining = v->rep.n;
    } else {
        addr = R32(0x70);
        remaining = R32(0x74);
    }
    s.loop_h0 = (int16_t)R16(0x104);
    s.loop_h1 = (int16_t)R16(0x106);
    s.loop_pos = R32(0x110);
    s.loop_end = R32(0x114);
    if (v->rep.pcm) pitch *= v->rep.ratio;
    bool finished = false;
    if (v->cur == INT32_MIN) {
        int32_t x;
        if (fetch(&s, v, &addr, &remaining, &x)) { v->prev = x; v->cur = x; }
        else finished = true;
    }
    if (!finished) {
        for (int i = 0; i < SUBFRAME; i++) {
            double smp = v->prev + (double)(v->cur - v->prev) * v->frac;
            out_l[i] += (float)smp * (vl0 + dl * (float)(i + 1));
            out_r[i] += (float)smp * (vr0 + dr * (float)(i + 1));
            v->frac += pitch;
            while (v->frac >= 1.0) {
                v->frac -= 1.0;
                int32_t x;
                if (fetch(&s, v, &addr, &remaining, &x)) { v->prev = v->cur; v->cur = x; }
                else { finished = true; break; }
            }
            if (finished) break;
        }
    }
    m_write_be(b + 0x70, 4, addr);
    m_write_be(b + 0x74, 4, remaining);
    /* VPB +0x64: decoded AFC samples not yet consumed (JASAramStream's play position uses it). */
    W16(0x64, s.afc && !v->rep.pcm && v->valid > v->idx ? v->valid - v->idx : 0);
    if (finished) W16(0x02, 1);
    return true;
#undef R16
#undef R32
#undef W16
}

/* Render the released sub-frame and queue its completion mail; true if it mailed. The worker
 * calls it with s_dl held, which it drops while mixing (only the renderer touches the voices, and
 * the frame state changes only between a sub-frame's mail and the next release). */
static bool step(void) {
    if (!s_frame || !s_released) return false;
    if (s_next >= s_subframes) { s_frame = false; return false; }
    s_released = false;
    uint32_t k = s_next++, nch = s_channels, left = s_left, right = s_right;
    bool more = s_next < s_subframes;
    float level = s_level;
    unlock();
    float l[SUBFRAME] = {0}, r[SUBFRAME] = {0};
    uint64_t voices = 0;
    for (uint32_t ch = 0; ch < nch; ch++) voices += mix_channel(ch, l, r);
    g_dsp_subframes++;
    g_dsp_voices_last = voices;
    uint32_t off = k * SUBFRAME * 2;
    for (int i = 0; i < SUBFRAME; i++) {
        float cl = l[i] * level, cr = r[i] * level;
        cl = cl < -32768 ? -32768 : cl > 32767 ? 32767 : cl;
        cr = cr < -32768 ? -32768 : cr > 32767 ? 32767 : cr;
        m_write_be(left + off + 2 * i, 2, (uint16_t)(int16_t)cl);
        m_write_be(right + off + 2 * i, 2, (uint16_t)(int16_t)cr);
    }
    lock();
    mail_push(MAIL_FRAME);
    if (!more) s_frame = false;
    return true;
}

/* DSP interrupt: run JAudio's DSP callback, which reads the mail (rendering the released
 * sub-frame first, unless the worker does that). */
void dsp_interrupt(Ctx *c) {
    if (!s_async) step();
    uint32_t m;
    if (g_dsp_callback && dsp_mail_peek(&m)) {
        uint32_t arg = 0;
        ctx_call_guest(c, g_dsp_callback, &arg, 1);
    }
}
