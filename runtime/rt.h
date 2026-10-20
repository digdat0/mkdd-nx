/* The console runtime behind the generated code (C port of MKDD-Recomp's gc-runtime crate).
 *
 * Guest code only ever runs on the host thread that holds the scheduler token (sched.c), which
 * is what makes the unsynchronised RAM/MMIO access sound in practice. */
#pragma once
#include <stdio.h>
#include "gc_core.h"
#include "plat.h"

#define ARAM_SIZE 0x01000000u
#define OS_CURRENT_THREAD 0x800000E4u

/* ---- machine.c ---- */
extern uint8_t *g_ram, *g_aram;
/* Guest MEM1 access for runtime code (0 / ignored outside MEM1). Inline: HLE handlers such as
 * the MTX library call these per float. */
static inline uint64_t m_read_be(uint32_t addr, unsigned size) {
    int64_t o = ram_off(addr);
    return o >= 0 && (uint64_t)o + size <= RAM_SIZE ? get_be(g_ram + o, size) : 0;
}
static inline void m_write_be(uint32_t addr, unsigned size, uint64_t v) {
    int64_t o = ram_off(addr);
    if (o >= 0 && (uint64_t)o + size <= RAM_SIZE) put_be(g_ram + o, size, v);
}
void m_load(uint32_t addr, const void *bytes, size_t n);
const char *m_name(uint32_t addr); /* "?" if unknown */
uint32_t m_addr_of(const char *name);
void ctx_init(Ctx *c);
void ctx_spawn_like(Ctx *c, const Ctx *from, uint32_t thread);
/* Boot the console from the disc image; returns false (with a message) if it can't. */
bool machine_boot(const char *disc_path, Ctx *boot, char *err, size_t errlen);
void machine_run(Ctx *boot); /* runs the game from the DOL entry point on this thread */

/* ---- disc.c ---- */
bool disc_open(const char *path, char *err, size_t errlen);
bool disc_present(void);
void disc_read(uint64_t off, void *out, size_t n);
/* RVZ images (runtime/rvz.c), used by disc.c */
bool rvz_open(FILE *f, uint64_t *disc_size, char *err, size_t errlen);
void rvz_read(uint64_t off, uint8_t *out, size_t n);
uint32_t disc_read_u32(uint64_t off);
bool disc_find(const char *path, uint64_t *off, uint32_t *size); /* file in the disc's FST */
/* Play a mod extracted to mod_dir (sys/main.dol, sys/fst.bin, files/) over the open image: from
 * then on the disc is the mod's, its missing files taken from the image. */
bool disc_overlay(const char *mod_dir, char *err, size_t errlen);
bool disc_overlay_active(void);
const char *disc_overlay_name(void); /* the mod folder's name, NULL without one */
/* Replace n bytes at disc offset off in every later read (front-end layout fixes; at most 1024 bytes). */
void disc_patch(uint64_t off, const void *bytes, size_t n);

/* ---- sched.c ---- */
#define SCHED_NONE 0u
#define SCHED_IDLE 1u
#define SCHED_BOOT 2u
extern volatile uint32_t g_sched_host, g_gx_gpu_host; /* profiler: host thread handles */
uint32_t sched_running_now(void);
void sched_note_host(void);
#define IRQ_DSP_AI 5
#define IRQ_DSP_ARAM 6
#define IRQ_DSP_DSP 7
#define IRQ_AI 8
#define IRQ_PE_TOKEN 18
#define IRQ_PE_FINISH 19
#define IRQ_SI 20
#define IRQ_DI 21
#define IRQ_VI 24
#define TB_HZ 40500000ull
uint64_t sched_timebase(void);
void sched_raise(int irq);
void sched_set_ai_period(uint64_t ticks);
void sched_set_idle_stack(uint32_t sp);
void sched_vi_beam(uint32_t *v, uint32_t *h);
extern volatile uint32_t g_vi_count;
void sched_init(void);
void sched_start_idle(void);
void sched_create(Ctx *c, uint32_t t);
int sched_resume(Ctx *c, uint32_t t);
int sched_suspend(Ctx *c, uint32_t t);
void sched_sleep(Ctx *c, uint32_t queue);
void sched_wakeup(Ctx *c, uint32_t queue);
void sched_yield_now(Ctx *c);
void sched_reschedule(Ctx *c);
void sched_exit(Ctx *c);
uint32_t sched_set_priority(uint32_t t, int prio);
void sched_cancel(Ctx *c, uint32_t t);
void sched_set_alarm(uint32_t ptr, uint32_t handler, uint64_t fire, uint64_t period);
void sched_cancel_alarm(uint32_t ptr);
void sched_describe(char *out, size_t n);

/* ---- hle.c ---- */
void hle_init(void);
extern uint64_t g_dvd_reads;

/* ---- hw.c ---- */
#define GX_FIFO 0xCC008000u
extern bool g_gx_link;
extern uint64_t g_ai_buffers, g_gx_bytes;
extern uint64_t g_ai_stale, g_ai_lost, g_ai_lat_max_us; /* AI buffers sent unchanged since their last use; AI periods merged */
void hw_before_interrupt(Ctx *c, int irq);
void hw_init(void);
void hw_gx_flush(void); /* hand buffered GX FIFO bursts to the decoder */
void hw_gather_write(unsigned size, uint64_t v);      /* a CPU write to the gather pipe */
void hw_gather_copy(const uint8_t *src, size_t n);    /* several, bytes in FIFO order */
void hw_gather_capture(uint8_t *buf, size_t max);     /* NULL: back to the FIFO */
size_t hw_gather_captured(void);

/* ---- dsp.c ---- */
extern uint32_t g_dsp_callback;
extern uint64_t g_dsp_subframes, g_dsp_voices_last;
bool dsp_command(uint32_t cmds, uint32_t count);
void dsp_interrupt(Ctx *c);
bool dsp_mail_pop(uint32_t *mail);
bool dsp_mail_peek(uint32_t *mail);

/* ---- audio.c: AI DMA output (interleaved stereo s16) ---- */
void audio_push_be(const uint8_t *bytes, size_t n, uint32_t rate);
/* Pop up to n frames (stereo) into out; silence while priming or after an underrun. */
void audio_pop(int16_t *out, size_t n);
uint32_t audio_rate(void);
size_t audio_queued(void);
double audio_rate_trim(void);
extern uint64_t g_audio_underruns;

/* ---- pad.c: controller state from the host input layer ---- */
#define PAD_LEFT 0x0001
#define PAD_RIGHT 0x0002
#define PAD_DOWN 0x0004
#define PAD_UP 0x0008
#define PAD_TRIGGER_Z 0x0010
#define PAD_TRIGGER_R 0x0020
#define PAD_TRIGGER_L 0x0040
#define PAD_A 0x0100
#define PAD_B 0x0200
#define PAD_X 0x0400
#define PAD_Y 0x0800
#define PAD_START 0x1000
typedef struct {
    uint16_t button;
    int8_t stick_x, stick_y, substick_x, substick_y;
    uint8_t trigger_l, trigger_r, analog_a, analog_b;
    bool connected;
} PadStatus;
void pad_set(int port, const PadStatus *s);
void pad_get_all(PadStatus out[4]);
/* PADControlMotor: the rumble motor of each port (bit per port, set = running). */
void pad_motor_set(int port, bool on);
unsigned pad_motor_mask(void);
/* Set by the front end while the game is paused (menu open, out of focus): PADRead waits, so the
 * game's main loop stops at a point where it holds no locks of its own. */
extern volatile bool g_pad_hold;

/* ---- psmtx.c / cheats.c / memcard.c / thp.c ---- */
bool psmtx_call(Ctx *c, int which);
enum { MTX_COPY, MTX_CONCAT, MTX_MULTVEC, VEC_CROSS, VEC_MAG, MTX_INVERSE };
/* MKDD Plus 3.3: engine classes listed 50cc, 150cc, 200cc, Mirror (the mod has 200cc second). Run
 * once the mod's main.dol is in memory. */
void plus_class_order(void);
void cheats_unlock_all(void);
void cheats_set(bool on); /* on: unlock_all; off: the earned bits back */
bool cheats_active(void);
void cheats_tick(void);
void cheats_before_save(void);
void cheats_after_save(void);
void cheats_after_load(void);
void memcard_set_path(const char *path);
/* CARD API: returns true and sets *result if handled. */
bool memcard_call(Ctx *c, const char *name, int32_t *result);
bool thp_decode(uint32_t file, const uint32_t dst[3]);
extern volatile uint64_t g_thp_frames; /* movie frames decoded (the widescreen HUD keeps movies 4:3) */

/* ---- diag.c ---- */
void diag_intrinsic(const char *name);
void diag_set_log_file(const char *path);
void diag_start_watchdog(const char *trace_path);
void diag_flush(void);
/* What is logged (− menu LOGGING): LOG_GAME = mkdd.log / stderr.log lines, the 5 s trace and the
 * Switch profiler; LOG_AUDIO = audio lines (DSP, AI timing, audio thread). 0 writes nothing. */
enum { LOG_GAME = 1, LOG_AUDIO = 2 };
extern volatile uint32_t g_log_mask;
/* Start-up: game lines are logged whatever LOGGING says, each written to the SD card at once, so
 * a start that crashes or hangs shows its last step. The front end clears it at the first game frame. */
extern volatile bool g_log_boot;
