/* Probe entries and consumer fixes. Preserve flags, MXCSR, every GPR and
   XMM0..5 (C preserves XMM6..15). Legacy SSE leaves YMM upper halves intact.
   RBP gives exact unwind metadata even when a host mid-site RSP is unaligned. */
.text
.macro CP_THUNK name, id, tramp
.globl \name
.seh_proc \name
\name:
    pushq %rax
    .seh_pushreg %rax
    pushq %rcx
    .seh_pushreg %rcx
    pushq %rdx
    .seh_pushreg %rdx
    pushq %rbx
    .seh_pushreg %rbx
    pushq %rbp
    .seh_pushreg %rbp
    pushq %rsi
    .seh_pushreg %rsi
    pushq %rdi
    .seh_pushreg %rdi
    pushq %r8
    .seh_pushreg %r8
    pushq %r9
    .seh_pushreg %r9
    pushq %r10
    .seh_pushreg %r10
    pushq %r11
    .seh_pushreg %r11
    pushq %r12
    .seh_pushreg %r12
    pushq %r13
    .seh_pushreg %r13
    pushq %r14
    .seh_pushreg %r14
    pushq %r15
    .seh_pushreg %r15
    pushfq
    .seh_stackalloc 8
    movq %rsp,%rbp
    .seh_setframe %rbp,0
    .seh_endprologue
    andq $-16,%rsp
    subq $144,%rsp
    stmxcsr 128(%rsp)
    movdqu %xmm0,32(%rsp)
    movdqu %xmm1,48(%rsp)
    movdqu %xmm2,64(%rsp)
    movdqu %xmm3,80(%rsp)
    movdqu %xmm4,96(%rsp)
    movdqu %xmm5,112(%rsp)
.if \id == -1 || \id == -3
    movq 128(%rbp),%rax /* mid helper call return points into the generated stub */
    movl 20(%rax),%ecx /* context id at stub+26 */
.else
    movl $\id,%ecx
.endif
    leaq 8(%rbp),%rdx
    leaq 32(%rsp),%r8
    leaq 128(%rbp),%r9
.if \id == -2
    movq %r9,%rcx
    call consumer_fps_value
    movl %eax,120(%rbp)
    movl $0,124(%rbp)
.elseif \id == -4
    call consumer_x_value
    movss %xmm0,32(%rsp)
.elseif \id == -3
    call consumer_mid_factor
    movss %xmm0,24(%rsp)
.else
    call consumer_probe_hit
.endif
    movdqu 32(%rsp),%xmm0
    movdqu 48(%rsp),%xmm1
    movdqu 64(%rsp),%xmm2
    movdqu 80(%rsp),%xmm3
    movdqu 96(%rsp),%xmm4
    movdqu 112(%rsp),%xmm5
.if \id == -3
    cmpl $0,24(%rsp)
    je 9f
    movq 128(%rbp),%rax
    movl 20(%rax),%eax
    cmpl $1,%eax
    jne 7f
    mulss 24(%rsp),%xmm13
    jmp 9f
7:
    cmpl $4,%eax
    je 8f
    mulss 24(%rsp),%xmm6
    jmp 9f
8:
    mulss 24(%rsp),%xmm1
9:
.endif
    ldmxcsr 128(%rsp)
    /* Restore flags before the standard LEA/POP epilogue, while the frame
       register still identifies this frame. POPFQ inside an epilogue would
       defeat Windows' recogniser after the caller's RBP had been restored. */
    pushq 0(%rbp)
    popfq
    leaq 8(%rbp),%rsp
    popq %r15
    popq %r14
    popq %r13
    popq %r12
    popq %r11
    popq %r10
    popq %r9
    popq %r8
    popq %rdi
    popq %rsi
    popq %rbp
    popq %rbx
    popq %rdx
    popq %rcx
    popq %rax
.if \id < 0
    ret
.else
    jmp *\tramp(%rip)
.endif
    .seh_endproc
.endm
CP_THUNK cp_fps_entry,0,cp_fps_tramp
CP_THUNK cp_ph_entry,5,cp_ph_tramp
CP_THUNK cp_phstep_entry,8,cp_phstep_tramp
CP_THUNK cp_mid_leaf,-1,unused
CP_THUNK cf_fps_entry,-2,unused
CP_THUNK cf_mid_leaf,-3,unused
CP_THUNK cf_x_entry,-4,unused
