.text
.globl dsr_fiber_context_switch
.type dsr_fiber_context_switch, @function
dsr_fiber_context_switch:
.cfi_startproc
.cfi_def_cfa %rsp, 8
.cfi_offset %rip, -8
    endbr64
    movq %rsp, 0(%rdi)
    movq %rbx, 8(%rdi)
    movq %rbp, 16(%rdi)
    movq %r12, 24(%rdi)
    movq %r13, 32(%rdi)
    movq %r14, 40(%rdi)
    movq %r15, 48(%rdi)
    leaq .Lresume(%rip), %rax
    movq %rax, 56(%rdi)
    stmxcsr 64(%rdi)
    fnstcw 68(%rdi)

    movq 0(%rsi), %rsp
    movq 8(%rsi), %rbx
    movq 16(%rsi), %rbp
    movq 24(%rsi), %r12
    movq 32(%rsi), %r13
    movq 40(%rsi), %r14
    movq 48(%rsi), %r15
    ldmxcsr 64(%rsi)
    fldcw 68(%rsi)
    movq 56(%rsi), %rax
    jmp *%rax

.Lresume:
    endbr64
    ret
.cfi_endproc
.size dsr_fiber_context_switch, .-dsr_fiber_context_switch

.section .note.GNU-stack,"",@progbits
