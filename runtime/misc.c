/* Small runtime pieces: audio output queue (audio.rs), controller state (pad.rs), the native
 * MTX library (psmtx.rs) and the session-only "unlock all" cheat (cheats.rs). */
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "rt.h"
#include "native.h"

/* ---- audio: AI DMA buffers (interleaved stereo s16 big-endian) -> host stream ----
 * The game delivers audio in bursts while the host pulls at a steady rate. A jitter buffer
 * absorbs that: playback starts once TARGET_MS is queued, an underrun outputs silence and
 * re-primes, and the consumer nudges its rate (+-0.5 %) to keep the queue near the target.
 * 100 ms (60 before build 1612 left ~1 underrun a minute on the Switch when the game thread
 * stalled); an underrun fades the last sample out and playback fades back in, instead of
 * stepping to and from silence (a click). */
#define TARGET_MS 100
#define FADE 96
#define MAX_MS 500
#define QCAP (48000 * MAX_MS / 1000 + 4096)

static int16_t s_q[QCAP][2];
static size_t s_qhead, s_qlen;
static PMutex s_alock;
static bool s_alock_init;
static volatile uint32_t s_rate = 32000;
static bool s_primed;
static int16_t s_last[2];
static int s_fade_in = FADE;
uint64_t g_audio_underruns;

static void alock(void) {
    if (!s_alock_init) { pmutex_init(&s_alock); s_alock_init = true; }
    pmutex_lock(&s_alock);
}
static size_t frames_ms(int ms) { return (size_t)s_rate * (size_t)ms / 1000; }

void audio_push_be(const uint8_t *b, size_t n, uint32_t rate) {
    s_rate = rate;
    alock();
    size_t max = frames_ms(MAX_MS);
    for (size_t i = 0; i + 4 <= n; i += 4) {
        if (s_qlen >= QCAP) { s_qhead = (s_qhead + 1) % QCAP; s_qlen--; }
        size_t t = (s_qhead + s_qlen) % QCAP;
        s_q[t][0] = (int16_t)(b[i] << 8 | b[i + 1]);
        s_q[t][1] = (int16_t)(b[i + 2] << 8 | b[i + 3]);
        s_qlen++;
    }
    while (s_qlen > max) { s_qhead = (s_qhead + 1) % QCAP; s_qlen--; }
    pmutex_unlock(&s_alock);
}

size_t audio_queued(void) { alock(); size_t n = s_qlen; pmutex_unlock(&s_alock); return n; }
uint32_t audio_rate(void) { return s_rate; }

double audio_rate_trim(void) {
    double target = (double)(frames_ms(TARGET_MS) ? frames_ms(TARGET_MS) : 1);
    double err = ((double)audio_queued() - target) / target * 0.005;
    return 1.0 + (err < -0.005 ? -0.005 : err > 0.005 ? 0.005 : err);
}

void audio_pop(int16_t *out, size_t n) {
    alock();
    size_t i = 0;
    if (!s_primed) {
        if (s_qlen < frames_ms(TARGET_MS)) goto silence;
        s_primed = true;
        s_fade_in = 0;
    }
    for (; i < n; i++) {
        if (!s_qlen) {
            g_audio_underruns++;
            s_primed = false;
            goto silence;
        }
        int g = s_fade_in < FADE ? ++s_fade_in : FADE;
        s_last[0] = s_q[s_qhead][0];
        s_last[1] = s_q[s_qhead][1];
        out[2 * i] = (int16_t)(s_last[0] * g / FADE);
        out[2 * i + 1] = (int16_t)(s_last[1] * g / FADE);
        s_qhead = (s_qhead + 1) % QCAP;
        s_qlen--;
    }
    pmutex_unlock(&s_alock);
    return;
silence: /* from the last sample down to 0 over FADE samples, then 0 */
    for (; i < n; i++) {
        for (int k = 0; k < 2; k++) {
            int v = s_last[k];
            v -= v / (FADE / 8 > 0 ? FADE / 8 : 1);
            if (v > -8 && v < 8) v = 0;
            s_last[k] = (int16_t)v;
            out[2 * i + k] = (int16_t)v;
        }
    }
    pmutex_unlock(&s_alock);
}

/* ---- pad ---- */
static PadStatus s_pads[4];
static PMutex s_plock;
static bool s_plock_init;
static void plock(void) {
    if (!s_plock_init) { pmutex_init(&s_plock); s_plock_init = true; }
    pmutex_lock(&s_plock);
}
void pad_set(int port, const PadStatus *s) { plock(); s_pads[port & 3] = *s; pmutex_unlock(&s_plock); }
void pad_get_all(PadStatus out[4]) { plock(); memcpy(out, s_pads, sizeof s_pads); pmutex_unlock(&s_plock); }
static volatile unsigned s_motors;
void pad_motor_set(int port, bool on) {
    unsigned bit = 1u << (port & 3);
    s_motors = on ? s_motors | bit : s_motors & ~bit;
}
unsigned pad_motor_mask(void) { return s_motors; }
volatile bool g_pad_hold;

/* ---- MTX library: the HLE hook path (indirect calls, the runtime) into the native versions
 * in native.c; generated code calls those directly. ---- */
bool psmtx_call(Ctx *c, int which) {
    uint64_t a0 = ctx_gpr(c, 3), a1 = ctx_gpr(c, 4), a2 = ctx_gpr(c, 5);
    switch (which) {
    case MTX_COPY: n_PSMTXCopy(c, c->ram, a0, a1, a2, 0, 0, 0, 0, 0); break;
    case MTX_CONCAT: n_PSMTXConcat(c, c->ram, a0, a1, a2, 0, 0, 0, 0, 0); break;
    case MTX_MULTVEC: n_PSMTXMultVec(c, c->ram, a0, a1, a2, 0, 0, 0, 0, 0); break;
    case VEC_CROSS: n_PSVECCrossProduct(c, c->ram, a0, a1, a2, 0, 0, 0, 0, 0); break;
    case VEC_MAG: {
        uint64_t bits = n_PSVECMag(c, c->ram, a0, a1, a2, 0, 0, 0, 0, 0);
        WR(c, 0x4000 + 8, 8, bits);
        WR(c, 0x5000 + 8, 8, bits);
        break;
    }
    case MTX_INVERSE: ctx_set_gpr(c, 3, (uint32_t)n_PSMTXInverse(c, c->ram, a0, a1, a2, 0, 0, 0, 0, 0)); break;
    default: return false;
    }
    return true;
}

/* ---- "Unlock all": session-only, never reaches the memory card. The save record (SystemRecord,
 * 0x803BC1C0) keeps the unlocks as two bit sets: +4 secret karts, +6 secret game flags. While the
 * cheat is on every bit is set in RAM and the player's own earned bits are kept aside; saving
 * (SystemFile::fetch) puts the earned bits back for the copy, loading (SystemFile::store) takes
 * the loaded bits as the earned ones. ---- */
#define SECRET_KARTS 0x803BC1C4u
#define SECRET_KARTS_ALL 0x1FFF
#define SECRET_GAME 0x803BC1C6u
#define SECRET_GAME_ALL 0x01FF
static bool s_unlock;
static uint16_t s_earned[2];

static void apply_unlock(void) {
    const uint32_t addr[2] = {SECRET_KARTS, SECRET_GAME};
    const uint16_t all[2] = {SECRET_KARTS_ALL, SECRET_GAME_ALL};
    for (int k = 0; k < 2; k++) {
        uint16_t v = (uint16_t)m_read_be(addr[k], 2);
        if ((v & all[k]) != all[k]) {
            s_earned[k] = v;
            m_write_be(addr[k], 2, v | all[k]);
        }
    }
}
void cheats_unlock_all(void) { s_unlock = true; apply_unlock(); }
void cheats_set(bool on) {
    if (on) { cheats_unlock_all(); return; }
    if (!s_unlock) return;
    s_unlock = false;
    m_write_be(SECRET_KARTS, 2, s_earned[0]);
    m_write_be(SECRET_GAME, 2, s_earned[1]);
}
bool cheats_active(void) { return s_unlock; }
void cheats_tick(void) { if (s_unlock) apply_unlock(); }
void cheats_before_save(void) {
    if (!s_unlock) return;
    m_write_be(SECRET_KARTS, 2, s_earned[0]);
    m_write_be(SECRET_GAME, 2, s_earned[1]);
}
void cheats_after_save(void) { if (s_unlock) apply_unlock(); }
void cheats_after_load(void) {
    if (!s_unlock) return;
    s_earned[0] = (uint16_t)m_read_be(SECRET_KARTS, 2);
    s_earned[1] = (uint16_t)m_read_be(SECRET_GAME, 2);
    apply_unlock();
}
