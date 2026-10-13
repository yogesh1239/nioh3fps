#include "stubs.h"
#include <string.h>
#include <stdio.h>

size_t stubs_unwind_len(const uint8_t *ui)
{
    return 4 + 2 * (size_t)((ui[2] + 1) & ~1);
}

static int unwind_slots(int op, int info)
{
    switch (op) {
    case 0: return 1;                    /* UWOP_PUSH_NONVOL */
    case 1: return info == 0 ? 2 : 3;    /* UWOP_ALLOC_LARGE */
    case 2: return 1;                    /* UWOP_ALLOC_SMALL */
    case 3: return 1;                    /* UWOP_SET_FPREG */
    case 4: return 2;                    /* UWOP_SAVE_NONVOL */
    case 5: return 3;                    /* UWOP_SAVE_NONVOL_FAR */
    case 8: return 2;                    /* UWOP_SAVE_XMM128 */
    case 9: return 3;                    /* UWOP_SAVE_XMM128_FAR */
    case 10: return 1;                   /* UWOP_PUSH_MACHFRAME */
    default: return -1;
    }
}

int stubs_transform_unwind(const uint8_t *src, uint8_t *dst, size_t *len, char *err, size_t errlen)
{
    int ver = src[0] & 7, flags = src[0] >> 3, cnt = src[2], fr = src[3] & 15, i;
    size_t n = stubs_unwind_len(src);
    if (ver != 1 || (flags & 4) || fr != 0) {
        snprintf(err, errlen, "unsupported host UNWIND_INFO (version %d, flags %d, frame register %d)", ver, flags, fr);
        return -1;
    }
    memcpy(dst, src, n);
    dst[0] = (uint8_t)(ver | (0 << 3));   /* Flags = 0: drop EHANDLER/UHANDLER */
    dst[1] = 0;                           /* SizeOfProlog = 0 */
    for (i = 0; i < cnt;) {
        int op = dst[4 + 2 * i + 1] & 15, info = dst[4 + 2 * i + 1] >> 4;
        int ns = unwind_slots(op, info);
        if (ns < 0 || i + ns > cnt) {
            snprintf(err, errlen, "unsupported unwind opcode %d at slot %d", op, i);
            return -1;
        }
        dst[4 + 2 * i] = 0;               /* CodeOffset of the code's first slot only */
        i += ns;
    }
    *len = n;
    return 0;
}

static void put32(uint8_t *p, int32_t v) { memcpy(p, &v, 4); }
static void put64(uint8_t *p, uint64_t v) { memcpy(p, &v, 8); }

/* rip-relative disp from the end of an instruction at code+end to target */
static int32_t rel(const uint8_t *insn_end, const volatile void *target)
{
    return (int32_t)((intptr_t)target - (intptr_t)insn_end);
}

int stubs_build(const stubs_params *p, stubs_layout *o, char *err, size_t errlen)
{
    uint8_t *base = p->alloc, *data = base + STUB_PAGE, *s;
    uint8_t uw[64];
    size_t n1, n2;
    RUNTIME_FUNCTION *t;

    memset(o, 0, sizeof *o);
    memset(base, 0xCC, STUB_PAGE);
    memset(data, 0, STUB_PAGE);
    o->r1 = (volatile float *)(data + DATA_R1);
    o->r2 = (volatile float *)(data + DATA_R2);
    o->c60 = (volatile float *)(data + DATA_C60);
    o->ovl_h1 = (volatile long *)(data + DATA_OVL_H1);
    o->ovl_h2 = (volatile long *)(data + DATA_OVL_H2);
    *o->r1 = 1.0f;
    *o->r2 = 1.0f;
    *o->c60 = 60.0f;

    /* P1: movss xmm2,[rdi+0x628]; mulss xmm2,[r1]; movss [rdi+0x624],xmm2;
           mov rax, imm64(base+0x5ad6b2); jmp rax (FF E0) */
    s = base + STUB_P1_OFF; o->p1_stub = s;
    o->p1_insn[o->p1_ninsn++] = 0;
    memcpy(s, "\xF3\x0F\x10\x97\x28\x06\x00\x00", 8);
    o->p1_insn[o->p1_ninsn++] = 8;
    memcpy(s + 8, "\xF3\x0F\x59\x15", 4); put32(s + 12, rel(s + 16, o->r1));
    o->p1_insn[o->p1_ninsn++] = 16;
    memcpy(s + 16, "\xF3\x0F\x11\x97\x24\x06\x00\x00", 8);
    o->p1_insn[o->p1_ninsn++] = 24;
    s[24] = 0x48; s[25] = 0xB8; put64(s + 26, p->p1_return);
    o->p1_insn[o->p1_ninsn++] = 34;
    s[34] = 0xFF; s[35] = 0xE0;

    /* P2: movss xmm7,[c60]; cvtsi2ss xmm0,rax; divss xmm7,xmm0; cmp eax,120; jne skip;
           mulss xmm7,[r2]; skip: mov rax, imm64(base+0x39e537); jmp rax */
    s = base + STUB_P2_OFF; o->p2_stub = s;
    o->p2_insn[o->p2_ninsn++] = 0;
    memcpy(s, "\xF3\x0F\x10\x3D", 4); put32(s + 4, rel(s + 8, o->c60));
    o->p2_insn[o->p2_ninsn++] = 8;
    memcpy(s + 8, "\xF3\x48\x0F\x2A\xC0", 5);
    o->p2_insn[o->p2_ninsn++] = 13;
    memcpy(s + 13, "\xF3\x0F\x5E\xF8", 4);
    o->p2_insn[o->p2_ninsn++] = 17;
    memcpy(s + 17, "\x83\xF8\x78", 3);
    o->p2_insn[o->p2_ninsn++] = 20;
    s[20] = 0x75; s[21] = 0x08;                       /* jne +8 -> 30 */
    o->p2_insn[o->p2_ninsn++] = 22;
    memcpy(s + 22, "\xF3\x0F\x59\x3D", 4); put32(s + 26, rel(s + 30, o->r2));
    o->p2_insn[o->p2_ninsn++] = 30;
    s[30] = 0x48; s[31] = 0xB8; put64(s + 32, p->p2_return);
    o->p2_insn[o->p2_ninsn++] = 40;
    s[40] = 0xFF; s[41] = 0xE0;

    /* unwind: host UNWIND_INFO copies at 4-byte-aligned offsets */
    if (stubs_unwind_len(p->p1_host_unwind) > sizeof uw || stubs_unwind_len(p->p2_host_unwind) > sizeof uw) {
        snprintf(err, errlen, "host UNWIND_INFO too long");
        return -1;
    }
    if (stubs_transform_unwind(p->p1_host_unwind, uw, &n1, err, errlen)) return -1;
    memcpy(base + STUB_UW1_OFF, uw, n1);
    if (stubs_transform_unwind(p->p2_host_unwind, uw, &n2, err, errlen)) return -1;
    memcpy(base + STUB_UW2_OFF, uw, n2);

    /* sorted RUNTIME_FUNCTION table, RVAs relative to the allocation base */
    t = (RUNTIME_FUNCTION *)(base + STUB_TBL_OFF);
    t[0].BeginAddress = STUB_P1_OFF; t[0].EndAddress = STUB_P1_OFF + STUB_P1_LEN; t[0].UnwindData = STUB_UW1_OFF;
    t[1].BeginAddress = STUB_P2_OFF; t[1].EndAddress = STUB_P2_OFF + STUB_P2_LEN; t[1].UnwindData = STUB_UW2_OFF;
    o->table = t;
    o->count = 2;
    return 0;
}

/* Rebuild S2L bodies with decoded fields and native TLS re-entrancy guards.
   The original builder is retained for the S1 regression/unwind tests. */
void stubs_s2l(const stubs_params *p, stubs_layout *o, uint32_t req, uint32_t eff, DWORD tls)
{
    for (int k=0;k<2;k++) {
        uint8_t *s=p->alloc+(k ? 0x400 : 0x300); int n=0, ni=0, skip120=-1, skipgate;
        int *ins=k ? o->p2_insn : o->p1_insn;
#define EMIT(bytes,len) do { ins[ni++]=n; memcpy(s+n,bytes,len); n+=len; } while(0)
        if (!k) { EMIT("\xF3\x0F\x10\x97",4); put32(s+n,(int32_t)req); n+=4; }
        else {
            EMIT("\xF3\x0F\x10\x3D",4); put32(s+n,rel(s+n+4,o->c60)); n+=4;
            EMIT("\xF3\x48\x0F\x2A\xC0",5); EMIT("\xF3\x0F\x5E\xF8",4);
            EMIT("\x83\xF8\x78",3); EMIT("\x75\0",2); skip120=n-1;
        }
        EMIT("\x65\x48\x8B\x04\x25\x30\0\0\0",9); /* mov rax,gs:[TEB] */
        EMIT("\x48\x83\xB8",3); put32(s+n,(int32_t)(0x1480+8*tls)); n+=4; s[n++]=1;
        EMIT("\x74\0",2); skipgate=n-1;
        EMIT(k ? "\xF3\x0F\x59\x3D" : "\xF3\x0F\x59\x15",4);
        put32(s+n,rel(s+n+4,k ? o->r2 : o->r1)); n+=4;
        s[skipgate]=(uint8_t)(n-skipgate-1);
        if (k) s[skip120]=(uint8_t)(n-skip120-1);
        else { EMIT("\xF3\x0F\x11\x97",4); put32(s+n,(int32_t)eff); n+=4; }
        EMIT("\x48\xB8",2); put64(s+n,k ? p->p2_return : p->p1_return); n+=8;
        EMIT("\xFF\xE0",2);
        if (k) { o->p2_stub=s; o->p2_ninsn=ni; } else { o->p1_stub=s; o->p1_ninsn=ni; }
        o->table[k].BeginAddress=(DWORD)(s-p->alloc); o->table[k].EndAddress=o->table[k].BeginAddress+n;
#undef EMIT
    }
}
