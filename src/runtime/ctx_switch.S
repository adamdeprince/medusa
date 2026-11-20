/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Execution-context switch for x86-64 (System V ABI).
 *
 *   void dnuma_ctx_switch(void **save_sp, void *load_sp);
 *
 * Saves the callee-saved state of the current context on its own stack,
 * stores the resulting stack pointer in *save_sp, then resumes the context
 * whose saved stack pointer is load_sp.
 *
 * At a call boundary the ABI makes every vector and x87 data register
 * caller-saved, so the complete state of a context that suspends itself
 * voluntarily is: rbx, rbp, r12-r15, the MXCSR and x87 control words, and
 * its stack. All of it lives on the context's stack, which is distributed
 * memory - so a suspended context is fully described by one pointer and can
 * be resumed by any machine that can fault its stack in.
 */
        .text
        .globl  dnuma_ctx_switch
        .type   dnuma_ctx_switch, @function
dnuma_ctx_switch:
        pushq   %rbp
        pushq   %rbx
        pushq   %r12
        pushq   %r13
        pushq   %r14
        pushq   %r15
        subq    $8, %rsp
        stmxcsr (%rsp)
        fnstcw  4(%rsp)
        movq    %rsp, (%rdi)
        movq    %rsi, %rsp
        ldmxcsr (%rsp)
        fldcw   4(%rsp)
        addq    $8, %rsp
        popq    %r15
        popq    %r14
        popq    %r13
        popq    %r12
        popq    %rbx
        popq    %rbp
        ret
        .size   dnuma_ctx_switch, .-dnuma_ctx_switch

/*
 * First activation of a context: dnuma_ctx_switch "returns" here with the
 * entry point in r12 and its argument in r13.
 */
        .globl  dnuma_fiber_thunk
        .type   dnuma_fiber_thunk, @function
dnuma_fiber_thunk:
        movq    %r12, %rdi
        movq    %r13, %rsi
        andq    $-16, %rsp
        call    dnuma_fiber_main
        ud2
        .size   dnuma_fiber_thunk, .-dnuma_fiber_thunk

        .section .note.GNU-stack, "", @progbits
