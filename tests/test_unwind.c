/* Native unwind test for the P1/P2 stub pages (PLAN section 3, "Unwind").
   Builds the stub allocation exactly as the DLL does (src/stubs.c), with the hosts'
   UNWIND_INFO bytes copied from Nioh3.exe.unpacked.exe at test time, registers it with
   RtlAddFunctionTable and calls RtlVirtualUnwind from every stub instruction offset.
   Each walk must reach the host's caller (return-address sentinel, rsp = entry + 8)
   with every nonvolatile register restored from the host's frame.
   Cross-check: the same fake frame is unwound from the patch site inside a copy of the
   real host code registered with the original (untransformed) UNWIND_INFO; both
   CONTEXTs must be identical. Sensitivity check: an untransformed copy at the stub
   must fail at small offsets (otherwise the test would prove nothing). */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/stubs.h"
#include "../src/expected.h"

static int g_fail, g_checks;
#define CHECK(c, ...) do { g_checks++; if (!(c)) { g_fail++; printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); } } while (0)

static uint8_t *g_file; static size_t g_size;
static IMAGE_SECTION_HEADER *g_sec; static int g_nsec;

static uint8_t *rva_ptr(DWORD rva)
{
    for (int i = 0; i < g_nsec; i++) {
        DWORD va = g_sec[i].VirtualAddress, vs = g_sec[i].Misc.VirtualSize;
        if (rva >= va && rva < va + vs && rva - va < g_sec[i].SizeOfRawData)
            return g_file + g_sec[i].PointerToRawData + (rva - va);
    }
    return NULL;
}

static RUNTIME_FUNCTION *find_pdata(DWORD begin)
{
    IMAGE_NT_HEADERS64 *nt = (IMAGE_NT_HEADERS64 *)(g_file + ((IMAGE_DOS_HEADER *)g_file)->e_lfanew);
    IMAGE_DATA_DIRECTORY d = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
    RUNTIME_FUNCTION *rf = (RUNTIME_FUNCTION *)rva_ptr(d.VirtualAddress);
    for (DWORD i = 0; i < d.Size / sizeof *rf; i++)
        if (rf[i].BeginAddress == begin) return &rf[i];
    return NULL;
}

typedef struct {
    const char *name;
    DWORD host_begin, host_end, site_rva;
    int frame_size;           /* entry rsp - body rsp */
    const uint8_t *exp_unwind; size_t exp_len;
} host_def;

typedef struct { DWORD64 rip, rsp, rbx, rbp, rsi, rdi, r12, r13, r14, r15; M128A xmm[10]; } regs_t;

static uint64_t g_stack[1024];

static void make_stack(void)
{
    for (int i = 0; i < 1024; i++) g_stack[i] = 0x5E5E000000000000ull | ((uint64_t)i << 8) | 0x11;
}

static regs_t unwind_from(DWORD64 rip, DWORD64 body_rsp, int *ok)
{
    CONTEXT c; regs_t r; DWORD64 ib = 0; PVOID hd = NULL; DWORD64 ef = 0;
    PRUNTIME_FUNCTION fe;
    memset(&c, 0, sizeof c);
    c.ContextFlags = CONTEXT_FULL;
    c.Rip = rip; c.Rsp = body_rsp;
    c.Rbx = 0xBAD0000000000001ull; c.Rbp = 0xBAD0000000000002ull; c.Rsi = 0xBAD0000000000003ull;
    c.Rdi = 0xBAD0000000000004ull; c.R12 = 0xBAD0000000000005ull; c.R13 = 0xBAD0000000000006ull;
    c.R14 = 0xBAD0000000000007ull; c.R15 = 0xBAD0000000000008ull;
    for (int i = 0; i < 16; i++) { (&c.Xmm0)[i].Low = 0xBADF00D000000000ull + (uint64_t)i; (&c.Xmm0)[i].High = 0xBADF00D100000000ull + (uint64_t)i; }
    fe = RtlLookupFunctionEntry(rip, &ib, NULL);
    *ok = fe != NULL;
    if (fe) RtlVirtualUnwind(UNW_FLAG_NHANDLER, ib, rip, fe, &c, &hd, &ef, NULL);
    r.rip = c.Rip; r.rsp = c.Rsp; r.rbx = c.Rbx; r.rbp = c.Rbp; r.rsi = c.Rsi; r.rdi = c.Rdi;
    r.r12 = c.R12; r.r13 = c.R13; r.r14 = c.R14; r.r15 = c.R15;
    for (int i = 0; i < 10; i++) r.xmm[i] = (&c.Xmm6)[i];
    return r;
}

static uint64_t at(DWORD64 a) { return *(uint64_t *)(uintptr_t)a; }

/* expected registers from the host prolog (disassembly in tools/re, PLAN F3/F9c) */
static int check_expected(const host_def *h, const regs_t *r, DWORD64 entry, char *why, size_t n)
{
    if (r->rip != at(entry)) { snprintf(why, n, "rip %llx != ret sentinel %llx", (unsigned long long)r->rip, (unsigned long long)at(entry)); return 0; }
    if (r->rsp != entry + 8) { snprintf(why, n, "rsp off by %lld", (long long)(r->rsp - (entry + 8))); return 0; }
    if (h->host_begin == EXP_P1HOST_BEGIN) {
        /* push rbp; push rbx; push rsi; push rdi; push r14; lea rbp,[rsp-0x37]; sub rsp,0xf0 */
        if (r->rbp != at(entry - 8) || r->rbx != at(entry - 16) || r->rsi != at(entry - 24) ||
            r->rdi != at(entry - 32) || r->r14 != at(entry - 40)) { snprintf(why, n, "P1 host pushes not restored"); return 0; }
        if (r->r12 != 0xBAD0000000000005ull || r->r13 != 0xBAD0000000000006ull || r->r15 != 0xBAD0000000000008ull) { snprintf(why, n, "untouched regs changed"); return 0; }
    } else {
        /* mov rax,rsp; mov [rax+8],rbx; mov [rax+0x10],rsi; push rdi; sub rsp,0x50;
           movaps [rax-0x18],xmm6; movaps [rax-0x28],xmm7 */
        if (r->rbx != at(entry + 8) || r->rsi != at(entry + 16) || r->rdi != at(entry - 8)) { snprintf(why, n, "P2 host saves not restored"); return 0; }
        if (memcmp(&r->xmm[0], (void *)(uintptr_t)(entry - 0x18), 16) || memcmp(&r->xmm[1], (void *)(uintptr_t)(entry - 0x28), 16)) { snprintf(why, n, "xmm6/xmm7 not restored"); return 0; }
        if (r->rbp != 0xBAD0000000000002ull || r->r14 != 0xBAD0000000000007ull) { snprintf(why, n, "untouched regs changed"); return 0; }
    }
    return 1;
}

int main(int argc, char **argv)
{
    const char *exe = argc > 1 ? argv[1] : "G:\\SteamLibrary\\steamapps\\common\\Nioh3\\Nioh3.exe.unpacked.exe";
    FILE *f = fopen(exe, "rb");
    IMAGE_NT_HEADERS64 *nt;
    host_def H[2] = {
        { "P1 (host 0x5ad5f8)", EXP_P1HOST_BEGIN, EXP_P1HOST_END, EXP_P1_RVA, 5 * 8 + 0xF0, EXP_P1HOST_UNWIND, sizeof EXP_P1HOST_UNWIND },
        { "P2 (host 0x39e4e4)", EXP_P2HOST_BEGIN, EXP_P2HOST_END, EXP_P2_RVA + EXP_P2_HOOKOFF, 8 + 0x50, EXP_P2HOST_UNWIND, sizeof EXP_P2HOST_UNWIND },
    };
    const uint8_t *host_uw[2];
    uint8_t *alloc, *ref, *neg;
    stubs_params sp; stubs_layout L; char err[256];
    DWORD old;

    if (!f) { printf("cannot open %s\n", exe); return 2; }
    _fseeki64(f, 0, SEEK_END); g_size = (size_t)_ftelli64(f); _fseeki64(f, 0, SEEK_SET);
    g_file = malloc(g_size);
    if (fread(g_file, 1, g_size, f) != g_size) { printf("read failed\n"); return 2; }
    fclose(f);
    nt = (IMAGE_NT_HEADERS64 *)(g_file + ((IMAGE_DOS_HEADER *)g_file)->e_lfanew);
    g_sec = IMAGE_FIRST_SECTION(nt); g_nsec = nt->FileHeader.NumberOfSections;
    printf("exe %s: timestamp %08lX (expected %08X), SizeOfImage %08lX\n", exe,
           (unsigned long)nt->FileHeader.TimeDateStamp, EXP_PE_TIMESTAMP, (unsigned long)nt->OptionalHeader.SizeOfImage);
    CHECK(nt->FileHeader.TimeDateStamp == EXP_PE_TIMESTAMP, "PE timestamp differs from expected.h");

    for (int k = 0; k < 2; k++) {
        RUNTIME_FUNCTION *rf = find_pdata(H[k].host_begin);
        CHECK(rf != NULL, "%s: no .pdata entry", H[k].name);
        if (!rf) return 1;
        CHECK(rf->EndAddress == H[k].host_end, "%s: .pdata end %lx", H[k].name, (unsigned long)rf->EndAddress);
        host_uw[k] = rva_ptr(rf->UnwindData);
        CHECK(stubs_unwind_len(host_uw[k]) == H[k].exp_len && !memcmp(host_uw[k], H[k].exp_unwind, H[k].exp_len),
              "%s: UNWIND_INFO in exe differs from expected.h", H[k].name);
        printf("%s: UNWIND_INFO rva %lx, %u codes, prolog %u B, flags %u\n", H[k].name, (unsigned long)rf->UnwindData,
               host_uw[k][2], host_uw[k][1], host_uw[k][0] >> 3);
    }

    /* 1. the stub allocation, built exactly like the DLL */
    alloc = VirtualAlloc(NULL, 2 * STUB_PAGE, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    sp.alloc = alloc;
    sp.p1_return = 0x140000000ull + 0x5ad6b2;
    sp.p2_return = 0x140000000ull + 0x39e537;
    sp.p1_host_unwind = host_uw[0];
    sp.p2_host_unwind = host_uw[1];
    if (stubs_build(&sp, &L, err, sizeof err)) { printf("stubs_build: %s\n", err); return 1; }
    VirtualProtect(alloc, STUB_PAGE, PAGE_EXECUTE_READ, &old);
    CHECK(RtlAddFunctionTable(L.table, L.count, (DWORD64)(uintptr_t)alloc), "RtlAddFunctionTable(stubs)");
    CHECK(*L.r1 == 1.0f && *L.r2 == 1.0f && *L.c60 == 60.0f, "data page init");
    CHECK(!memcmp(L.p1_stub + 34, "\xFF\xE0", 2) && !memcmp(L.p2_stub + 40, "\xFF\xE0", 2), "jmp rax must be FF E0");
    CHECK((STUB_UW1_OFF & 3) == 0 && (STUB_UW2_OFF & 3) == 0, "unwind copies 4-byte aligned");
    {
        const uint8_t *u = alloc + STUB_UW1_OFF;
        CHECK((u[0] >> 3) == 0 && u[1] == 0 && u[2] == host_uw[0][2], "P1 transformed header");
        u = alloc + STUB_UW2_OFF;
        CHECK((u[0] >> 3) == 0 && u[1] == 0 && u[2] == host_uw[1][2], "P2 transformed header");
    }

    /* 2. reference copies of the real host code with the original UNWIND_INFO */
    ref = VirtualAlloc(NULL, 0x10000, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    {
        RUNTIME_FUNCTION *rt = (RUNTIME_FUNCTION *)(ref + 0xF000);
        DWORD off = 0x1000;
        for (int k = 0; k < 2; k++) {
            DWORD len = H[k].host_end - H[k].host_begin;
            memcpy(ref + off, rva_ptr(H[k].host_begin), len);
            memcpy(ref + 0xE000 + k * 0x100, host_uw[k], stubs_unwind_len(host_uw[k]) + 16);  /* + handler rva/data, unused */
            rt[k].BeginAddress = off; rt[k].EndAddress = off + len; rt[k].UnwindData = 0xE000 + k * 0x100;
            off += 0x1000;
        }
        VirtualProtect(ref, 0x10000, PAGE_EXECUTE_READ, &old);
        CHECK(RtlAddFunctionTable(rt, 2, (DWORD64)(uintptr_t)ref), "RtlAddFunctionTable(ref)");
    }

    /* 3. negative control: stub bytes with the untransformed host UNWIND_INFO */
    neg = VirtualAlloc(NULL, 0x2000, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    {
        RUNTIME_FUNCTION *nt2 = (RUNTIME_FUNCTION *)(neg + 0x800);
        memcpy(neg, alloc, 0x100);
        memcpy(neg + 0x400, host_uw[0], stubs_unwind_len(host_uw[0]) + 16);
        memcpy(neg + 0x500, host_uw[1], stubs_unwind_len(host_uw[1]) + 16);
        nt2[0].BeginAddress = STUB_P1_OFF; nt2[0].EndAddress = STUB_P1_OFF + STUB_P1_LEN; nt2[0].UnwindData = 0x400;
        nt2[1].BeginAddress = STUB_P2_OFF; nt2[1].EndAddress = STUB_P2_OFF + STUB_P2_LEN; nt2[1].UnwindData = 0x500;
        VirtualProtect(neg, 0x2000, PAGE_EXECUTE_READ, &old);
        RtlAddFunctionTable(nt2, 2, (DWORD64)(uintptr_t)neg);
    }

    make_stack();
    for (int k = 0; k < 2; k++) {
        DWORD64 body = (DWORD64)(uintptr_t)&g_stack[300];
        DWORD64 entry = body + (DWORD64)H[k].frame_size;
        const uint8_t *stub = k == 0 ? L.p1_stub : L.p2_stub;
        const int *ins = k == 0 ? L.p1_insn : L.p2_insn;
        int nins = k == 0 ? L.p1_ninsn : L.p2_ninsn, okfe, neg_bad = 0, good = 0;
        DWORD64 refrip = (DWORD64)(uintptr_t)(ref + 0x1000 + k * 0x1000 + (H[k].site_rva - H[k].host_begin));
        regs_t rr = unwind_from(refrip, body, &okfe);
        char why[160];
        CHECK(okfe, "%s: reference lookup", H[k].name);
        CHECK(check_expected(&H[k], &rr, entry, why, sizeof why), "%s: reference unwind wrong: %s", H[k].name, why);
        for (int i = 0; i < nins; i++) {
            DWORD64 rip = (DWORD64)(uintptr_t)(stub + ins[i]);
            regs_t r = unwind_from(rip, body, &okfe), rn;
            int ok = okfe && check_expected(&H[k], &r, entry, why, sizeof why);
            CHECK(okfe, "%s +%d: RtlLookupFunctionEntry found nothing", H[k].name, ins[i]);
            CHECK(ok, "%s +%d: %s", H[k].name, ins[i], why);
            CHECK(!memcmp(&r, &rr, sizeof r), "%s +%d: CONTEXT differs from the unwind at the real patch site", H[k].name, ins[i]);
            good += ok && !memcmp(&r, &rr, sizeof r);
            rn = unwind_from((DWORD64)(uintptr_t)(neg + (stub - alloc) + ins[i]), body, &okfe);
            if (!okfe || !check_expected(&H[k], &rn, entry, why, sizeof why)) neg_bad++;
        }
        printf("%s: %d/%d stub instruction offsets unwind to the host's caller; untransformed control fails at %d offsets\n",
               H[k].name, good, nins, neg_bad);
        CHECK(neg_bad > 0, "%s: negative control did not fail; test is not sensitive", H[k].name);
    }
    /* v8 adds native TLS guards. Walk each actual new instruction as well. */
    RtlDeleteFunctionTable(L.table);
    VirtualProtect(alloc,STUB_PAGE,PAGE_READWRITE,&old);
    stubs_s2l(&sp,&L,0x628,0x624,0);
    VirtualProtect(alloc,STUB_PAGE,PAGE_EXECUTE_READ,&old);
    CHECK(RtlAddFunctionTable(L.table,L.count,(DWORD64)(uintptr_t)alloc),"register guarded stubs");
    for (int k=0;k<2;k++) {
        const uint8_t *stub=k ? L.p2_stub : L.p1_stub;
        const int *ins=k ? L.p2_insn : L.p1_insn;
        int nins=k ? L.p2_ninsn : L.p1_ninsn,okfe; char why[160];
        DWORD64 body=(DWORD64)(uintptr_t)&g_stack[300],entry=body+H[k].frame_size;
        DWORD64 refrip=(DWORD64)(uintptr_t)(ref+0x1000+k*0x1000+H[k].site_rva-H[k].host_begin);
        regs_t rr=unwind_from(refrip,body,&okfe);
        for (int j=0;j<nins;j++) {
            regs_t got=unwind_from((DWORD64)(uintptr_t)(stub+ins[j]),body,&okfe);
            CHECK(okfe && check_expected(&H[k],&got,entry,why,sizeof why) && !memcmp(&got,&rr,sizeof got),
                  "S2L %s +%d: equals host unwind",H[k].name,ins[j]);
        }
        printf("S2L %s: all %d guarded stub instructions checked\n",H[k].name,nins);
    }
    printf("%d checks, %d failures\n", g_checks, g_fail);
    printf(g_fail ? "TEST_UNWIND: FAIL\n" : "TEST_UNWIND: PASS\n");
    return g_fail ? 1 : 0;
}
