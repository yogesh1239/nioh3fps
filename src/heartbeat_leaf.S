/* No C calls, no waits, no rdtsc. Entry is a normal call from the generated
   original-rsp stub. Preserve flags, volatile GPRs and XMMs; QPC is the only call. */
.text
.globl heartbeat_leaf
.seh_proc heartbeat_leaf
heartbeat_leaf:
    pushfq
    .seh_stackalloc 8
    pushq %rax
    .seh_pushreg %rax
    pushq %rcx
    .seh_pushreg %rcx
    pushq %rdx
    .seh_pushreg %rdx
    pushq %r8
    .seh_pushreg %r8
    pushq %r9
    .seh_pushreg %r9
    pushq %r10
    .seh_pushreg %r10
    pushq %r11
    .seh_pushreg %r11
    subq $168,%rsp
    .seh_stackalloc 168
    movdqu %xmm0,32(%rsp)
    .seh_savexmm %xmm0,32
    movdqu %xmm1,48(%rsp)
    .seh_savexmm %xmm1,48
    movdqu %xmm2,64(%rsp)
    .seh_savexmm %xmm2,64
    movdqu %xmm3,80(%rsp)
    .seh_savexmm %xmm3,80
    movdqu %xmm4,96(%rsp)
    .seh_savexmm %xmm4,96
    movdqu %xmm5,112(%rsp)
    .seh_savexmm %xmm5,112
    .seh_endprologue
    movl g_gate_tls(%rip),%ecx
    cmpl $64,%ecx
    jae .Ldone
    movq %gs:0x30,%rax
    cmpq $1,0x1480(%rax,%rcx,8)
    je .Ldone
    movl %gs:0x68,%eax
    movl %eax,136(%rsp)
    leaq 128(%rsp),%rcx
    call *g_qpc_fn(%rip)
    movl 136(%rsp),%eax
    movl %eax,%gs:0x68
    movq 128(%rsp),%r9
    movl %gs:0x48,%r8d
    movl $1,%eax
    lock xaddq %rax,g_hb_write(%rip)
    movq %rax,%r10
    andl $4095,%eax
    shlq $6,%rax
    leaq g_hb_ring(%rip),%rdx
    addq %rax,%rdx
    movq $0,(%rdx)
    movq %r9,8(%rdx)
    movq %r12,16(%rdx)
    movq %rbx,24(%rdx)
    movl %r8d,40(%rdx)
    movl %r13d,48(%rdx)
    movl $4,%r11d
    testq %r12,%r12
    je .Lrecord /* null paths are diagnostic only, including latch/history */
    movl $1,%r11d
    cmpq g_expected_mailbox(%rip),%rbx
    jne .Lrecord
    /* Pair selection and recovery belong to H1, never this leaf. */
    cmpl g_render_tid(%rip),%r8d
    je .Lprimary
    movq %r9,g_render_conflict(%rip)
    jmp .Lrecord
.Lprimary:
    lock incq g_take_seq(%rip)
    xorl %r11d,%r11d
    movq %r12,%rax
    xchgq %rax,g_previous_packet(%rip)
    movl %r13d,%ecx
    xchgl %ecx,g_previous_frame(%rip)
    testq %rax,%rax /* no previous heartbeat, including after a relatch */
    je .Lnew
    cmpl %r13d,%ecx
    je .Lrepeat
    cmpq %r12,%rax /* pointer equality is the secondary repeat signal */
    jne .Lnew
.Lrepeat:
    movl $2,%r11d
.Lnew:
    cmpl $0,g_heartbeat_source(%rip)
    jne .Lrecord
    movq %r9,g_render_qpc(%rip)
    lock incq g_render_seq(%rip)
.Lrecord:
    movq g_take_seq(%rip),%rax
    movq %rax,32(%rdx)
    movl %r11d,44(%rdx)
    incq %r10
    movq %r10,(%rdx) /* release, x86 ordered stores */
.Ldone:
    movdqu 32(%rsp),%xmm0
    movdqu 48(%rsp),%xmm1
    movdqu 64(%rsp),%xmm2
    movdqu 80(%rsp),%xmm3
    movdqu 96(%rsp),%xmm4
    movdqu 112(%rsp),%xmm5
    addq $168,%rsp

    .seh_endproc
/* popfq is not a Windows canonical epilogue. Give each restoration instruction
   the unwind state that actually remains, so a sample there cannot double-pop
   the saved frame. All codes at offset zero describe the already-built frame. */
.macro HB_POP name, reg, remaining
.seh_proc hb_restore_\name
hb_restore_\name:
    .seh_stackalloc 8
    .if \remaining >= 1
    .seh_pushreg %rax
    .endif
    .if \remaining >= 2
    .seh_pushreg %rcx
    .endif
    .if \remaining >= 3
    .seh_pushreg %rdx
    .endif
    .if \remaining >= 4
    .seh_pushreg %r8
    .endif
    .if \remaining >= 5
    .seh_pushreg %r9
    .endif
    .if \remaining >= 6
    .seh_pushreg %r10
    .endif
    .if \remaining >= 7
    .seh_pushreg %r11
    .endif
    .seh_endprologue
    popq %\reg
.seh_endproc
.endm
    HB_POP r11, r11, 7
    HB_POP r10, r10, 6
    HB_POP r9, r9, 5
    HB_POP r8, r8, 4
    HB_POP rdx, rdx, 3
    HB_POP rcx, rcx, 2
    HB_POP rax, rax, 1
.seh_proc hb_restore_flags
hb_restore_flags:
    .seh_stackalloc 8
    .seh_endprologue
    popfq
.seh_endproc
.seh_proc hb_restore_ret
hb_restore_ret:
    .seh_endprologue
    ret
.seh_endproc
.globl heartbeat_leaf_end
heartbeat_leaf_end:
