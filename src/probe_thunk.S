/* Static probe thunks (the per-target entry-count thunks are generated at runtime by
   probe.c; see there). Every thunk is entered by MinHook's jump at a function entry with
   the caller's registers and stack untouched, and leaves by `jmp [rip+tramp]` to the
   MinHook trampoline. All of them are register-transparent: every general-purpose and
   xmm register reaches the trampoline unchanged, rsp is unchanged and nothing at or above
   the entry rsp is written. Only rflags changes (dead across a call boundary). Pushes go
   below the entry rsp and are popped before the jump; each push is in the unwind info and
   the pops + jmp form a standard epilogue, so a stack walk at any instruction is exact.

   Data lines (probe_sline in common.h, one 64-byte line each, in the DLL's .data):
     g_sline[n].count @0, .tramp @8, .ctx @16; lines 0-1 hist thunks, 2-5 C thunks.

   probe_hist_thunk0/1 (GetFps / GetTimeStep return-address histograms):
     lock inc count; push rax rcx rdx r8 r9; key = [rsp+40] (the return address);
     open addressing over g_probe_hist[n] (HIST_SLOTS entries of {key, count}, Fibonacci hash,
     linear probe <= 32): an empty slot is claimed with lock cmpxchg, the count of the
     matching slot is incremented with lock inc; after 32 probes g_probe_hist_ovf[n] is
     incremented instead. Pure memory, no calls, no alignment assumption.

   probe_cthunk0..3 (camera ordering records, xellSleep timestamp: they call C):
     lock inc count; push rbp rax rcx rdx r8 r9 r10 r11; rbp = rsp (frame register);
     rsp aligned down to 16 (entry alignment is not assumed); xmm0-5 saved
     with movups (no alignment dependency even for the saves);
     probe_hit(ctx, return address); restore; lea rsp,[rbp]; pops; jmp [rip+tramp].
     probe_hit runs no AVX code, so ymm upper halves are not touched either. */

#define HIST_MASK 0x1FF0           /* (HIST_SLOTS - 1) * 16 */
#define HIST_BYTES (512 * 16)      /* HIST_SLOTS * sizeof(entry) */
#define HIST_PROBES 32

    .text

.macro HIST_THUNK n
    .globl  probe_hist_thunk\n
    .def    probe_hist_thunk\n; .scl 2; .type 32; .endef
    .seh_proc probe_hist_thunk\n
probe_hist_thunk\n:
    lock incq g_sline+64*\n(%rip)
    pushq   %rax
    .seh_pushreg %rax
    pushq   %rcx
    .seh_pushreg %rcx
    pushq   %rdx
    .seh_pushreg %rdx
    pushq   %r8
    .seh_pushreg %r8
    pushq   %r9
    .seh_pushreg %r9
    .seh_endprologue
    movq    40(%rsp), %rdx                  /* key = return address */
    movabsq $0x9E3779B97F4A7C15, %rcx      /* Fibonacci hashing: top 9 bits of key * 2^64/phi */
    imulq   %rdx, %rcx
    shrq    $51, %rcx
    andl    $HIST_MASK, %ecx                /* byte offset of the home slot */
    leaq    g_probe_hist+HIST_BYTES*\n(%rip), %r8
    movl    $HIST_PROBES, %r9d
1:  movq    (%r8,%rcx), %rax
    cmpq    %rdx, %rax
    je      3f
    testq   %rax, %rax
    jnz     2f
    lock cmpxchgq %rdx, (%r8,%rcx)          /* claim the empty slot (rax = 0) */
    je      3f
    cmpq    %rdx, %rax                      /* lost the race: claimed for the same key? */
    je      3f
2:  addl    $16, %ecx
    andl    $HIST_MASK, %ecx
    decl    %r9d
    jnz     1b
    leaq    g_probe_hist_ovf+8*\n-8(%rip), %r8    /* table full: count into g_probe_hist_ovf[n] */
    xorl    %ecx, %ecx
3:  lock incq 8(%r8,%rcx)
    popq    %r9
    popq    %r8
    popq    %rdx
    popq    %rcx
    popq    %rax
    jmp     *g_sline+64*\n+8(%rip)
    .seh_endproc
.endm

.macro C_THUNK n
    .globl  probe_cthunk\n
    .def    probe_cthunk\n; .scl 2; .type 32; .endef
    .seh_proc probe_cthunk\n
probe_cthunk\n:
    lock incq g_sline+64*(2+\n)(%rip)
    pushq   %rbp
    .seh_pushreg %rbp
    pushq   %rax
    .seh_pushreg %rax
    pushq   %rcx
    .seh_pushreg %rcx
    pushq   %rdx
    .seh_pushreg %rdx
    pushq   %r8
    .seh_pushreg %r8
    pushq   %r9
    .seh_pushreg %r9
    pushq   %r10
    .seh_pushreg %r10
    pushq   %r11
    .seh_pushreg %r11
    movq    %rsp, %rbp
    .seh_setframe %rbp, 0
    .seh_endprologue
    andq    $-16, %rsp
    subq    $0x80, %rsp
    movups  %xmm0, 0x20(%rsp)
    movups  %xmm1, 0x30(%rsp)
    movups  %xmm2, 0x40(%rsp)
    movups  %xmm3, 0x50(%rsp)
    movups  %xmm4, 0x60(%rsp)
    movups  %xmm5, 0x70(%rsp)
    movq    g_sline+64*(2+\n)+16(%rip), %rcx  /* ctx */
    movq    64(%rbp), %rdx                     /* return address */
    call    probe_hit
    movups  0x20(%rsp), %xmm0
    movups  0x30(%rsp), %xmm1
    movups  0x40(%rsp), %xmm2
    movups  0x50(%rsp), %xmm3
    movups  0x60(%rsp), %xmm4
    movups  0x70(%rsp), %xmm5
    leaq    0(%rbp), %rsp
    popq    %r11
    popq    %r10
    popq    %r9
    popq    %r8
    popq    %rdx
    popq    %rcx
    popq    %rax
    popq    %rbp
    jmp     *g_sline+64*(2+\n)+8(%rip)
    .seh_endproc
.endm

    HIST_THUNK 0
    HIST_THUNK 1
    C_THUNK 0
    C_THUNK 1
    C_THUNK 2
    C_THUNK 3
