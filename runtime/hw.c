/* Hardware / MMIO boundary: everything outside MEM1 lands here (C port of gc-runtime's hw.rs).
 *
 * Registers at 0xCC000000..0xCC010000 are backed by a byte array (reads return what the game
 * last wrote) with device behaviour layered on top where polling loops or DMA need it:
 *   CP 0xCC000000 status "idle" once the GPU thread drained the FIFO
 *   PE 0xCC001000 interrupt status bits write-1-to-clear; token register
 *   VI 0xCC002000 beam position advances with time; retrace status set on VI interrupt
 *   PI 0xCC003000 reset switch reads "released"; CPU FIFO write pointer advances
 *   DSP 0xCC005000 mailboxes always ready; ARAM DMA performed instantly + interrupt
 *   DI 0xCC006000 read DMA from the disc image + interrupt (fallback; DVD is HLE'd)
 *   SI/EXI transfers complete instantly; no devices attached
 *   AI 0xCC006C00 sample counter advances
 *   GX 0xCC008000 write-gather pipe -> RAM FIFO + gx decoder
 *   LC 0xE0000000 16 KiB locked cache scratch */
#include <stdlib.h>
#include <string.h>
#include "gx.h"
#include "rt.h"

#define MMIO_BASE 0xCC000000u
#define MMIO_SIZE 0x10000u
#define LC_BASE 0xE0000000u
#define LC_SIZE 0x4000u

#define CP_SR 0x0000
#define PE_ISR 0x100A
#define PE_TOKEN 0x100E
#define VI_DISP_POS 0x202C
#define VI_DI0 0x2030
#define PI_INTSR 0x3000
#define PI_FIFO_BASE 0x300C
#define PI_FIFO_END 0x3010
#define PI_FIFO_WPTR 0x3014
#define DSP_MBOX_IN_H 0x5000
#define DSP_MBOX_OUT_H 0x5004
#define DSP_MBOX_OUT_L 0x5006
#define DSP_CR 0x500A
#define AR_MODE 0x5016
#define AR_MMADDR 0x5020
#define AR_ARADDR 0x5024
#define AR_CNT 0x5028
#define AI_DMA_HI 0x5030
#define AI_DMA_LO 0x5032
#define AI_DMA_CTRL 0x5036
#define DI_SR 0x6000
#define DI_CMD0 0x6008
#define DI_CMD1 0x600C
#define DI_MAR 0x6014
#define DI_LEN 0x6018
#define DI_CR 0x601C
#define SI_COMCSR 0x6434
#define AI_CR 0x6C00
#define AI_SCNT 0x6C08
static const uint32_t EXI_CR[3] = {0x680C, 0x6820, 0x6834};

static uint8_t s_mmio[MMIO_SIZE];
static uint8_t s_lc[LC_SIZE];
/* CPU FIFO attached to the GP (false while it is redirected to build a display list). */
bool g_gx_link = true;
/* Completed 32-byte bursts not yet handed to the GX decoder: submitted 16 KiB at a time instead
 * of taking the decoder's queue lock per burst. hw_gx_flush() sends the rest; it runs before
 * every non-pure HLE call (waits, thread switches, GXDrawDone), interrupt delivery and every
 * other hardware access, so the
 * decoder has everything before the game can wait on it or look at its state. Guest threads
 * run one at a time, so only the token holder touches this. */
#define STAGE_FLUSH 16384 /* mmio_tramp.c compares with it too */
RT_HIDDEN uint8_t g_gx_stage[STAGE_FLUSH + 64];
/* Layout shared with mmio_tramp.c: stage_len = completed bytes in g_gx_stage, gather_len =
 * bytes of the burst being assembled behind them, pending = bytes of bursts the assembly fast
 * path completed (while linked) whose PI write-pointer advance is still owed (wptr_sync). */
RT_HIDDEN GxGatherPipe g_gx_gp;
#define g_gx_stage_len g_gx_gp.stage_len
#define g_gx_gather_len g_gx_gp.gather_len
static void wptr_sync(void);

void hw_gx_flush(void) {
    wptr_sync();
    if (g_gx_stage_len) {
        if (g_gx_link) gx_submit(g_gx_stage, g_gx_stage_len);
        if (g_gx_gather_len) memmove(g_gx_stage, g_gx_stage + g_gx_stage_len, g_gx_gather_len);
        g_gx_stage_len = 0;
    }
}
static uint32_t s_ai_samples, s_unmapped_logged;
uint64_t g_ai_buffers, g_gx_bytes, g_ai_stale, g_ai_lost;

void hw_init(void) {}

static uint64_t get(uint32_t off, unsigned size) { return get_be(s_mmio + off, size); }
static void set(uint32_t off, unsigned size, uint64_t v) { put_be(s_mmio + off, size, v); }
static uint32_t get16(uint32_t off) { return (uint32_t)get(off, 2); }
static void set16(uint32_t off, uint32_t v) { set(off, 2, v); }
static uint32_t get32(uint32_t off) { return (uint32_t)get(off, 4); }

/* Does the access [off, off+size) touch register bytes [reg, reg+reg_size)? */
static inline bool hits(uint32_t off, unsigned size, uint32_t reg, uint32_t reg_size) {
    return off < reg + reg_size && reg < off + size;
}

/* Apply PE register effects of events the GPU thread produced (on the CPU side). */
static void apply_gx_events(void) {
    GxEvent ev[64];
    int n;
    while ((n = gx_take_events(ev, 64)) > 0) {
        for (int i = 0; i < n; i++) {
            switch (ev[i].kind) {
            case GXEV_DRAW_DONE: set16(PE_ISR, get16(PE_ISR) | 0x0008); break;
            case GXEV_TOKEN: set16(PE_TOKEN, ev[i].token); break;
            case GXEV_TOKEN_INT: set16(PE_TOKEN, ev[i].token); set16(PE_ISR, get16(PE_ISR) | 0x0004); break;
            }
        }
        if (n < 64) break;
    }
}

static uint32_t ai_rate(void) { return get32(AI_CR) & 0x40 ? 32000 : 48000; }

uint64_t rt_mmio_read(Ctx *c, uint32_t addr, unsigned size) {
    hw_gx_flush();
    (void)c;
    if (addr >= LC_BASE && addr < LC_BASE + LC_SIZE) {
        uint32_t o = addr - LC_BASE;
        return o + size <= LC_SIZE ? get_be(s_lc + o, size) : 0;
    }
    if ((addr & 0xFFFF0000u) != MMIO_BASE) {
        if (s_unmapped_logged < 32) { s_unmapped_logged++; rt_log("unmapped read %#010x (%u bytes) from host %p (rt_mmio_read at %p)", addr, size, __builtin_return_address(0), (void *)rt_mmio_read); }
        return 0;
    }
    uint32_t off = addr & 0xFFFF;
    if (off + size > MMIO_SIZE) return 0;
    /* Live registers: refresh the backing bytes before the generic read. */
    if (hits(off, size, CP_SR, 2)) set16(CP_SR, gx_gpu_idle() ? 0x000C : 0);
    if (hits(off, size, PE_TOKEN, 2) || hits(off, size, PE_ISR, 2)) apply_gx_events();
    if (hits(off, size, VI_DISP_POS, 4)) {
        uint32_t v, h;
        sched_vi_beam(&v, &h);
        set(VI_DISP_POS, 4, (uint64_t)v << 16 | h);
    }
    if (hits(off, size, PI_INTSR, 4)) set(PI_INTSR, 4, get32(PI_INTSR) | 0x00010000); /* reset switch released */
    if (hits(off, size, DSP_MBOX_IN_H, 2)) set16(DSP_MBOX_IN_H, get16(DSP_MBOX_IN_H) & ~0x8000u); /* DSP took our mail */
    /* DSP -> CPU mail comes from the DSP emulation's queue; reading the low half consumes it. */
    if (hits(off, size, DSP_MBOX_OUT_H, 2)) {
        uint32_t m;
        set16(DSP_MBOX_OUT_H, dsp_mail_peek(&m) ? (m >> 16) | 0x8000 : 0);
    }
    if (hits(off, size, DSP_MBOX_OUT_L, 2)) {
        uint32_t m;
        set16(DSP_MBOX_OUT_L, dsp_mail_pop(&m) ? m & 0xFFFF : 0);
    }
    if (hits(off, size, DSP_CR, 2)) set16(DSP_CR, get16(DSP_CR) & ~0x0201u); /* reset done, ARAM DMA idle */
    if (hits(off, size, AR_MODE, 2)) set16(AR_MODE, get16(AR_MODE) | 1);
    const uint32_t done_regs[5] = {DI_CR, SI_COMCSR, EXI_CR[0], EXI_CR[1], EXI_CR[2]};
    for (int i = 0; i < 5; i++)
        if (hits(off, size, done_regs[i], 4)) set(done_regs[i], 4, get32(done_regs[i]) & ~1u); /* transfer complete */
    if (hits(off, size, AI_SCNT, 4)) {
        s_ai_samples += 32;
        set(AI_SCNT, 4, s_ai_samples);
    }
    return get(off, size);
}

static void aram_dma(void) {
    uint32_t mm = get32(AR_MMADDR) & 0x03FFFFE0u, ar = get32(AR_ARADDR) & 0x00FFFFE0u, cnt = get32(AR_CNT);
    if (mm >= RAM_SIZE || ar >= ARAM_SIZE) { rt_log("ARAM DMA out of range: mm %#x ar %#x", mm, ar); return; }
    uint32_t len = cnt & 0x7FFFFFE0u;
    if (len > RAM_SIZE - mm) len = RAM_SIZE - mm;
    if (len > ARAM_SIZE - ar) len = ARAM_SIZE - ar;
    if (cnt & 0x80000000u) { hw_gx_flush(); gx_drain(); memcpy(g_ram + mm, g_aram + ar, len); gx_mem_dirty(mm, len); }
    else memcpy(g_aram + ar, g_ram + mm, len);
    set16(DSP_CR, get16(DSP_CR) | 0x0020);
    sched_raise(IRQ_DSP_ARAM);
}

/* EXI channel 0, device 1: the RTC / SRAM chip. Without it the SDK reads an all-zero SRAM,
 * whose sound-mode bit means mono, and the game mixed every sound to the middle. The SRAM
 * image (64 bytes: OSSram + OSSramEx) lives for the session, so OSSetSoundMode etc. stick. */
static uint8_t s_sram[64];
static uint32_t s_exi_cmd, s_exi_pos;
static bool s_exi_have_cmd;

static void sram_init(void) {
    static bool done;
    if (done) return;
    done = true;
    memset(s_sram, 0, sizeof s_sram);
    s_sram[0x12] = 0;    /* language: English */
    s_sram[0x13] = 0x04; /* flags: stereo */
    uint16_t sum = 0, inv = 0;
    for (int i = 0x0C; i < 0x14; i += 2) {
        uint16_t w = (uint16_t)(s_sram[i] << 8 | s_sram[i + 1]);
        sum += w;
        inv += (uint16_t)~w;
    }
    s_sram[0] = sum >> 8; s_sram[1] = sum & 0xFF;
    s_sram[2] = inv >> 8; s_sram[3] = inv & 0xFF;
}

/* A transfer started on EXI channel 0 (CR TSTART). The first immediate write after a select
 * is the command (SDK OSRtc: 0x20000100 + offset * 64, bit 31 = write); the transfers after it
 * read or write the SRAM from that offset. */
static void exi0_transfer(void) {
    uint32_t csr = get32(0x6800), cr = get32(EXI_CR[0]);
    if (!(csr & 0x100)) { s_exi_have_cmd = false; return; } /* not device 1 */
    sram_init();
    bool dma = cr & 2;
    unsigned rw = (cr >> 2) & 3, len = dma ? get32(0x6808) : ((cr >> 4) & 3) + 1;
    if (!s_exi_have_cmd) {
        if (!dma && rw == 1) {
            s_exi_cmd = get32(0x6810);
            s_exi_have_cmd = true;
            s_exi_pos = ((s_exi_cmd & 0x7FFFFFFFu) - 0x20000100u) >> 6;
        }
        return;
    }
    uint32_t a = s_exi_cmd & 0x7FFFFFFFu;
    bool sram = a >= 0x20000100u && a < 0x20000100u + 64 * 64;
    for (unsigned i = 0; i < len; i++, s_exi_pos++) {
        uint8_t b = sram && s_exi_pos < sizeof s_sram ? s_sram[s_exi_pos] : 0;
        if (rw == 0) { /* read */
            if (dma) {
                uint32_t m = (get32(0x6804) & 0x03FFFFE0u) + i;
                if (m < RAM_SIZE) g_ram[m] = b;
            } else {
                uint32_t d = get32(0x6810);
                d = (d & ~(0xFFu << (24 - 8 * i))) | (uint32_t)b << (24 - 8 * i);
                set(0x6810, 4, d);
            }
        } else if (rw == 1 && sram && s_exi_pos < sizeof s_sram) { /* write */
            uint32_t m = (get32(0x6804) & 0x03FFFFE0u) + i;
            s_sram[s_exi_pos] = dma ? (m < RAM_SIZE ? g_ram[m] : 0) : (uint8_t)(get32(0x6810) >> (24 - 8 * i));
        }
    }
}

static void di_command(void) {
    uint32_t cmd = get32(DI_CMD0);
    if (cmd >> 24 == 0xA8) {
        uint64_t off = (uint64_t)get32(DI_CMD1) << 2;
        uint32_t dst = get32(DI_MAR) & 0x03FFFFE0u;
        if (dst > RAM_SIZE) dst = RAM_SIZE;
        uint32_t len = get32(DI_LEN);
        if (len > RAM_SIZE - dst) len = RAM_SIZE - dst;
        hw_gx_flush();
        gx_drain_dma(len);
        disc_read(off, g_ram + dst, len);
        gx_mem_dirty(dst, len);
        if (len >= (256u << 10)) gx_mem_dirty_all();
        set(DI_LEN, 4, 0);
    }
    set(DI_CR, 4, get32(DI_CR) & ~1u);
    set(DI_SR, 4, get32(DI_SR) | 0x10); /* TCINT */
    sched_raise(IRQ_DI);
}

/* CPU write-gather pipe: data leaves in 32-byte bursts, like the hardware, to the RAM FIFO and
 * (when the CPU FIFO is linked to the GP) to the GPU. GXFlush pads with 32 zero bytes, so every
 * command the game waits on is pushed through. */
/* Completed 32-byte bursts: copied to the FIFO in RAM (write pointer advanced) and kept staged
 * for the decoder. The gather pipe assembles each burst in place behind the staged ones. */
/* Advance the PI write pointer by n bytes (whole bursts), wrapping at the FIFO end. */
static void wptr_advance(uint32_t n) {
    uint32_t base = get32(PI_FIFO_BASE) & 0x03FFFFE0u, end = get32(PI_FIFO_END) & 0x03FFFFE0u;
    uint32_t wptr_reg = get32(PI_FIFO_WPTR), wptr = (wptr_reg & 0x03FFFFE0u) + n;
    uint32_t wrap = wptr_reg & 0x04000000u;
    if (end > base)
        while (wptr >= end) { wptr = base + (wptr - end); wrap = 0x04000000u; }
    set(PI_FIFO_WPTR, 4, wptr | wrap);
    g_gx_bytes += n;
}
/* Before anything reads the write pointer: hw_gx_flush (every MMIO access, HLE call, interrupt)
 * and gather_commit. */
static void wptr_sync(void) {
    if (g_gx_gp.pending) {
        uint32_t n = g_gx_gp.pending;
        g_gx_gp.pending = 0;
        wptr_advance(n);
    }
}
static void gather_commit(void) {
    wptr_sync();
    while (g_gx_gather_len >= 32) {
        const uint8_t *burst = g_gx_stage + g_gx_stage_len;
        uint32_t wptr = get32(PI_FIFO_WPTR) & 0x03FFFFE0u;
        if (!g_gx_link && wptr && wptr + 32 <= RAM_SIZE) memcpy(g_ram + wptr, burst, 32);
        wptr_advance(32);
        g_gx_stage_len += 32;
        g_gx_gather_len -= 32;
        if (g_gx_stage_len >= STAGE_FLUSH) hw_gx_flush();
    }
}
/* Capture (native.c's verifier): gather-pipe writes go to a buffer instead of the FIFO. */
RT_HIDDEN uint8_t *g_gx_cap;
static size_t s_cap_len, s_cap_max;
void hw_gather_capture(uint8_t *buf, size_t max) { g_gx_cap = buf; s_cap_len = 0; s_cap_max = max; }
size_t hw_gather_captured(void) { return s_cap_len; }

static inline void gather_write(unsigned size, uint64_t v) {
    if (UNLIKELY(g_gx_cap)) {
        if (s_cap_len + size <= s_cap_max) put_be(g_gx_cap + s_cap_len, size, v);
        s_cap_len += size;
        return;
    }
    put_be(g_gx_stage + g_gx_stage_len + g_gx_gather_len, size, v);
    if ((g_gx_gather_len += size) >= 32) gather_commit();
}
void hw_gather_write(unsigned size, uint64_t v) { gather_write(size, v); }
void hw_gather_commit(void) { gather_commit(); } /* mmio_tramp.c, once a burst is complete */
/* Bytes already in FIFO (big-endian) order, e.g. straight from guest RAM. */
void hw_gather_copy(const uint8_t *src, size_t n) {
    if (UNLIKELY(g_gx_cap)) {
        if (s_cap_len + n <= s_cap_max) memcpy(g_gx_cap + s_cap_len, src, n);
        s_cap_len += n;
        return;
    }
    /* linked to the GP and the stage has room: one copy; completed bursts become staged bytes
     * whose write-pointer advance is owed (as the assembly fast path does) */
    if (g_gx_link && g_gx_stage_len + g_gx_gather_len + n <= STAGE_FLUSH) {
        memcpy(g_gx_stage + g_gx_stage_len + g_gx_gather_len, src, n);
        unsigned t = g_gx_gather_len + (unsigned)n, done = t & ~31u;
        g_gx_stage_len += done;
        g_gx_gather_len = t - done;
        g_gx_gp.pending += done;
        return;
    }
    while (n) {
        size_t k = 32 - g_gx_gather_len < n ? 32 - g_gx_gather_len : n;
        memcpy(g_gx_stage + g_gx_stage_len + g_gx_gather_len, src, k);
        src += k;
        n -= k;
        if ((g_gx_gather_len += (unsigned)k) >= 32) gather_commit();
    }
}

void rt_mmio_write(Ctx *c, uint32_t addr, unsigned size, uint64_t v) {
    (void)c;
    if (addr == GX_FIFO || (addr & 0xFFFFFFF8u) == GX_FIFO) { gather_write(size, v); return; }
    hw_gx_flush();
    if (addr >= LC_BASE && addr < LC_BASE + LC_SIZE) {
        uint32_t o = addr - LC_BASE;
        if (o + size <= LC_SIZE) put_be(s_lc + o, size, v);
        return;
    }
    if ((addr & 0xFFFF0000u) != MMIO_BASE) {
        if (s_unmapped_logged < 32) { s_unmapped_logged++; rt_log("unmapped write %#010x = %#llx (%u bytes)", addr, (unsigned long long)v, size); }
        return;
    }
    uint32_t off = addr & 0xFFFF;
    if (off + size > MMIO_SIZE) return;
    /* Write-1-to-clear status bits. */
    bool pe_hit = hits(off, size, PE_ISR, 2), dsp_hit = hits(off, size, DSP_CR, 2);
    uint32_t pe_prev = get16(PE_ISR) & 0x000C, dsp_prev = get16(DSP_CR) & 0x00A8;
    set(off, size, v);
    if (pe_hit) {
        uint32_t w = get16(PE_ISR);
        set16(PE_ISR, (w & ~0x000Cu) | (pe_prev & ~(w & 0x000Cu)));
    }
    if (dsp_hit) {
        uint32_t w = get16(DSP_CR);
        set16(DSP_CR, (w & ~0x00A8u) | (dsp_prev & ~(w & 0x00A8u)));
    }
    /* ARAM DMA: triggered by writing the count register (low half completes it). */
    if (hits(off, size, AR_CNT + 2, 2)) aram_dma();
    /* AI DMA: (re)start pacing when the enable bit is written. */
    if (hits(off, size, AI_DMA_CTRL, 2)) {
        uint32_t ctrl = get16(AI_DMA_CTRL);
        uint64_t bytes = (uint64_t)(ctrl & 0x7FFF) * 32;
        sched_set_ai_period(ctrl & 0x8000 && bytes ? bytes / 4 * TB_HZ / ai_rate() : 0);
    }
    if (hits(off, size, DI_CR, 4) && (get32(DI_CR) & 1)) di_command();
    /* EXI0: a deselect (CSR write without device bits) ends the command. */
    if (hits(off, size, 0x6800, 4) && !(get32(0x6800) & 0x380)) s_exi_have_cmd = false;
    if (hits(off, size, EXI_CR[0], 4) && (get32(EXI_CR[0]) & 1)) exi0_transfer();
}

/* Device side effects that accompany an interrupt, applied just before its handler runs. */
void hw_before_interrupt(Ctx *c, int irq) {
    (void)c;
    switch (irq) {
    case IRQ_PE_FINISH:
    case IRQ_PE_TOKEN: apply_gx_events(); break;
    case IRQ_VI: set16(VI_DI0, get16(VI_DI0) | 0x8000); break;
    case IRQ_DSP_AI: {
        /* The AI fetched the buffer the registers point at: hand it to the host and signal the
         * game to prepare the next one. */
        uint32_t a = ((get16(AI_DMA_HI) & 0x03FF) << 16) | (get16(AI_DMA_LO) & 0xFFE0);
        uint32_t len = (get16(AI_DMA_CTRL) & 0x7FFF) * 32;
        if (len && a + len <= RAM_SIZE) {
            audio_push_be(g_ram + a, len, ai_rate());
            g_ai_buffers++;
            /* the game had not refilled this buffer since the AI last played it */
            static struct { uint32_t a; uint64_t h; } seen[4];
            uint64_t h = 1469598103934665603ull;
            bool nz = false;
            for (uint32_t i = 0; i < len; i++) { h = (h ^ g_ram[a + i]) * 1099511628211ull; nz |= g_ram[a + i] != 0; }
            unsigned k = 0;
            while (k < 4 && seen[k].a != a) k++;
            if (k == 4) { k = (unsigned)(g_ai_buffers & 3); seen[k].a = a; seen[k].h = ~h; }
            if (nz && seen[k].h == h) g_ai_stale++;
            seen[k].h = h;
        }
        set16(DSP_CR, get16(DSP_CR) | 0x0008);
        break;
    }
    default: break;
    }
}
