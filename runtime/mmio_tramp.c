/* Slow-path trampolines for generated code's memory accesses (gc_core.h mmio_rd / mmio_wr).
 *
 * Every guest load and store has an inline MEM1 fast path and an out-of-line call for anything
 * else (hardware registers, the gather pipe, the locked cache). A normal call clobbers every
 * caller-saved register, so with hundreds of such calls in a function the compiler kept most of
 * the game's registers in stack slots (measured: ~12% of game-thread samples on spill loads and
 * stores). These trampolines preserve every register except the result, so the call sites cost
 * the register allocator nothing. AArch64 also writes the gather pipe (the common case) without
 * leaving assembly, and completes 32-byte bursts there too while the CPU FIFO is linked to
 * the GP (hw.c settles the write pointer later); otherwise hw_gather_commit.
 *
 * Entry: x0/rcx = Ctx *, x1/rdx = address, x2/r8 = size, x3/r9 = value (writes).
 * Exit: x0/rax = value read. Everything else unchanged. */
#include "rt.h"

#if defined(__aarch64__)
#define SAVE_ALL                                                                          \
    "stp x29, x30, [sp, #-16]!\n stp x1, x2, [sp, #-16]!\n stp x3, x4, [sp, #-16]!\n"     \
    "stp x5, x6, [sp, #-16]!\n stp x7, x8, [sp, #-16]!\n stp x9, x10, [sp, #-16]!\n"      \
    "stp x11, x12, [sp, #-16]!\n stp x13, x14, [sp, #-16]!\n stp x15, x18, [sp, #-16]!\n" \
    "stp q0, q1, [sp, #-32]!\n stp q2, q3, [sp, #-32]!\n stp q4, q5, [sp, #-32]!\n"       \
    "stp q6, q7, [sp, #-32]!\n stp q8, q9, [sp, #-32]!\n stp q10, q11, [sp, #-32]!\n"     \
    "stp q12, q13, [sp, #-32]!\n stp q14, q15, [sp, #-32]!\n stp q16, q17, [sp, #-32]!\n" \
    "stp q18, q19, [sp, #-32]!\n stp q20, q21, [sp, #-32]!\n stp q22, q23, [sp, #-32]!\n" \
    "stp q24, q25, [sp, #-32]!\n stp q26, q27, [sp, #-32]!\n stp q28, q29, [sp, #-32]!\n" \
    "stp q30, q31, [sp, #-32]!\n"
#define RESTORE_ALL                                                                       \
    "ldp q30, q31, [sp], #32\n ldp q28, q29, [sp], #32\n ldp q26, q27, [sp], #32\n"       \
    "ldp q24, q25, [sp], #32\n ldp q22, q23, [sp], #32\n ldp q20, q21, [sp], #32\n"       \
    "ldp q18, q19, [sp], #32\n ldp q16, q17, [sp], #32\n ldp q14, q15, [sp], #32\n"       \
    "ldp q12, q13, [sp], #32\n ldp q10, q11, [sp], #32\n ldp q8, q9, [sp], #32\n"         \
    "ldp q6, q7, [sp], #32\n ldp q4, q5, [sp], #32\n ldp q2, q3, [sp], #32\n"             \
    "ldp q0, q1, [sp], #32\n"                                                             \
    "ldp x15, x18, [sp], #16\n ldp x13, x14, [sp], #16\n ldp x11, x12, [sp], #16\n"       \
    "ldp x9, x10, [sp], #16\n ldp x7, x8, [sp], #16\n ldp x5, x6, [sp], #16\n"            \
    "ldp x3, x4, [sp], #16\n ldp x1, x2, [sp], #16\n ldp x29, x30, [sp], #16\n"

__asm__(
    ".text\n"
    ".global rt_mmio_read_tramp\n .hidden rt_mmio_read_tramp\n .type rt_mmio_read_tramp, %function\n"
    ".p2align 4\n"
    "rt_mmio_read_tramp:\n"
    SAVE_ALL
    "bl rt_mmio_read\n"
    RESTORE_ALL
    "ret\n"
    ".size rt_mmio_read_tramp, .-rt_mmio_read_tramp\n"

    ".global rt_mmio_write_tramp\n .hidden rt_mmio_write_tramp\n .type rt_mmio_write_tramp, %function\n"
    ".p2align 4\n"
    "rt_mmio_write_tramp:\n"
    /* gather pipe (0xCC008000..7), not capturing: append big-endian at stage + staged + gathered
     * (hw.c g_gx_gp: stage_len at +0, gather_len at +8, pending at +12) */
    "and w16, w1, #0xfffffff8\n"
    "mov w17, #0x8000\n movk w17, #0xcc00, lsl #16\n"
    "cmp w16, w17\n b.ne 9f\n"
    "adrp x16, g_gx_cap\n ldr x16, [x16, :lo12:g_gx_cap]\n cbnz x16, 9f\n"
    "stp x4, x5, [sp, #-16]!\n"
    "adrp x17, g_gx_gp\n add x17, x17, :lo12:g_gx_gp\n"
    "ldr x4, [x17]\n ldr w5, [x17, #8]\n"
    "adrp x16, g_gx_stage\n add x16, x16, :lo12:g_gx_stage\n"
    "add x16, x16, x4\n add x16, x16, w5, uxtw\n"
    "cmp x2, #4\n b.ne 1f\n rev w4, w3\n str w4, [x16]\n b 5f\n"
    "1: cmp x2, #1\n b.ne 2f\n strb w3, [x16]\n b 5f\n"
    "2: cmp x2, #2\n b.ne 3f\n rev16 w4, w3\n strh w4, [x16]\n b 5f\n"
    "3: cmp x2, #8\n b.ne 8f\n rev x4, x3\n str x4, [x16]\n"
    "5: add w5, w5, w2\n cmp w5, #32\n b.hs 6f\n"
    "str w5, [x17, #8]\n"
    "ldp x4, x5, [sp], #16\n"
    "ret\n"
    /* burst complete: while linked to the GP and the stage has room, just move it to the
     * staged bytes and owe the write-pointer advance (hw.c wptr_sync); else hw_gather_commit */
    "6: adrp x16, g_gx_link\n ldrb w16, [x16, :lo12:g_gx_link]\n cbz w16, 7f\n"
    "ldr x4, [x17]\n add x4, x4, #32\n cmp x4, #16384\n b.hs 7f\n"
    "sub w5, w5, #32\n str x4, [x17]\n str w5, [x17, #8]\n"
    "ldr w16, [x17, #12]\n add w16, w16, #32\n str w16, [x17, #12]\n"
    "ldp x4, x5, [sp], #16\n"
    "ret\n"
    "7: str w5, [x17, #8]\n"
    "ldp x4, x5, [sp], #16\n"
    SAVE_ALL
    "bl hw_gather_commit\n"
    RESTORE_ALL
    "ret\n"
    "8: ldp x4, x5, [sp], #16\n"
    "9:\n"
    SAVE_ALL
    "bl rt_mmio_write\n"
    RESTORE_ALL
    "ret\n"
    ".size rt_mmio_write_tramp, .-rt_mmio_write_tramp\n");

#elif defined(__x86_64__) && defined(_WIN32)
/* Microsoft x64: rax, rcx, rdx, r8-r11 and xmm0-5 are volatile. */
#define TRAMP(name, target)                                                       \
    ".globl " name "\n"                                                           \
    ".p2align 4\n" name ":\n"                                                     \
    "push %rbp\n mov %rsp, %rbp\n"                                                \
    "push %rcx\n push %rdx\n push %r8\n push %r9\n push %r10\n push %r11\n"       \
    "and $-16, %rsp\n sub $128, %rsp\n"                                           \
    "movdqu %xmm0, 32(%rsp)\n movdqu %xmm1, 48(%rsp)\n movdqu %xmm2, 64(%rsp)\n"  \
    "movdqu %xmm3, 80(%rsp)\n movdqu %xmm4, 96(%rsp)\n movdqu %xmm5, 112(%rsp)\n" \
    "call " target "\n"                                                           \
    "movdqu 32(%rsp), %xmm0\n movdqu 48(%rsp), %xmm1\n movdqu 64(%rsp), %xmm2\n"  \
    "movdqu 80(%rsp), %xmm3\n movdqu 96(%rsp), %xmm4\n movdqu 112(%rsp), %xmm5\n" \
    "lea -48(%rbp), %rsp\n"                                                       \
    "pop %r11\n pop %r10\n pop %r9\n pop %r8\n pop %rdx\n pop %rcx\n"             \
    "pop %rbp\n ret\n"
__asm__(".text\n" TRAMP("rt_mmio_read_tramp", "rt_mmio_read") TRAMP("rt_mmio_write_tramp", "rt_mmio_write"));
#endif
