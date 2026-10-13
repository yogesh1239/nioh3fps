/* Native test of the probe thunks (src/probe.c generated entry-count thunks and the
   static thunks in src/probe_thunk.S), built from the real probe.c code paths:
   1. register transparency: an asm harness loads every GPR (except rsp) and all 16
      ymm/xmm registers with test patterns, puts a fake return address and two canaries
      on the stack and jumps to the thunk; the trampoline target records everything.
      All GPRs, all ymm registers (full 256 bits when AVX is present), rsp, the return
      address and the canaries must arrive unchanged. Run with the entry rsp both
      call-aligned (8 mod 16) and misaligned (0 mod 16).
   2. unwind at every instruction: the same run single-stepped (trap flag + VEH); at
      each instruction inside the thunk RtlVirtualUnwind must give rip = the return
      address, rsp = entry rsp + 8 and every GPR equal to its value at entry.
   3. counters and histogram: exact entry counts, histogram keys/counts with collisions
      and table overflow, a multi-threaded stress (lock inc, lock cmpxchg claiming), and
      probe_hit receiving the ctx and the return address (C thunks, order and xell).
   4. a generated thunk is exactly lock inc [rip]; jmp [rip] (byte check). */
#include "../src/common.h"
#include <stdio.h>
#include <string.h>

static int g_fail, g_checks;
#define CHECK(c, ...) do { g_checks++; if (!(c)) { g_fail++; printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* stand-ins for DLL globals and functions probe.c references */
uintptr_t g_base;
volatile LONG g_marker;
volatile LONG64 g_frame, g_last_xell_qpc;
int64_t g_qpf;
void log_printf(const char *fmt, ...) { (void)fmt; }
void crash_add_alloc_of(const void *p, const char *name) { (void)p; (void)name; }
static trace_rec g_rec;
static volatile int g_nrec;
trace_rec *trace_begin(uint64_t *idx)
{
    *idx = 0;
    for (size_t k = 0; k < sizeof g_rec / 8; k++) ((volatile uint64_t *)&g_rec)[k] = 0;
    return &g_rec;
}
void trace_end(trace_rec *r, uint64_t idx) { (void)r; (void)idx; g_nrec++; }

/* harness state (offsets used by the asm below) */
typedef struct {
    uint64_t g[16];          /* 0: rax rcx rdx rbx rsp rbp rsi rdi r8..r15 */
    uint8_t  x[16][32];      /* 128: ymm0..15 */
    uint64_t at_rsp[3];      /* 640: [rsp] (return address), canary, canary */
} regs_t;
_Static_assert(__builtin_offsetof(regs_t, x) == 128 && __builtin_offsetof(regs_t, at_rsp) == 640, "layout");
__attribute__((aligned(64))) regs_t g_in, g_out;
__attribute__((aligned(64))) uint8_t g_save[256];
void *g_entry;
uint8_t g_avx, g_tf, g_misalign;
void run_harness(void);
void recorder(void);
uint64_t g_flags_in,g_flags_out;

#define GPR_LOAD(off, r)  "  movq g_in+" #off "(%rip), %" #r "\n"
#define GPR_STORE(off, r) "  movq %" #r ", g_out+" #off "(%rip)\n"
#define YLOAD(i)  "  vmovdqu g_in+128+32*" #i "(%rip), %ymm" #i "\n"
#define YSTORE(i) "  vmovdqu %ymm" #i ", g_out+128+32*" #i "(%rip)\n"
#define XLOAD(i)  "  movdqu g_in+128+32*" #i "(%rip), %xmm" #i "\n"
#define XSTORE(i) "  movdqu %xmm" #i ", g_out+128+32*" #i "(%rip)\n"
#define ALL16(M) M(0) M(1) M(2) M(3) M(4) M(5) M(6) M(7) M(8) M(9) M(10) M(11) M(12) M(13) M(14) M(15)

__asm__(
    ".globl run_harness\nrun_harness:\n"
    "  movq %rbx, g_save+0(%rip)\n  movq %rbp, g_save+8(%rip)\n  movq %rsi, g_save+16(%rip)\n  movq %rdi, g_save+24(%rip)\n"
    "  movq %r12, g_save+32(%rip)\n  movq %r13, g_save+40(%rip)\n  movq %r14, g_save+48(%rip)\n  movq %r15, g_save+56(%rip)\n"
    "  movdqu %xmm6, g_save+64(%rip)\n  movdqu %xmm7, g_save+80(%rip)\n  movdqu %xmm8, g_save+96(%rip)\n"
    "  movdqu %xmm9, g_save+112(%rip)\n  movdqu %xmm10, g_save+128(%rip)\n  movdqu %xmm11, g_save+144(%rip)\n"
    "  movdqu %xmm12, g_save+160(%rip)\n  movdqu %xmm13, g_save+176(%rip)\n  movdqu %xmm14, g_save+192(%rip)\n"
    "  movdqu %xmm15, g_save+208(%rip)\n"
    "  movq %rsp, g_save+224(%rip)\n"
    "  andq $-16, %rsp\n  subq $256, %rsp\n"
    "  cmpb $0, g_misalign(%rip)\n  je 1f\n  subq $8, %rsp\n1:\n"
    "  pushq g_in+656(%rip)\n  pushq g_in+648(%rip)\n  pushq g_in+640(%rip)\n"
    "  movq %rsp, g_in+32(%rip)\n"
    "  cmpb $0, g_avx(%rip)\n  je 2f\n"
    ALL16(YLOAD)
    "  jmp 3f\n2:\n"
    ALL16(XLOAD)
    "3:\n"
    GPR_LOAD(0, rax) GPR_LOAD(8, rcx) GPR_LOAD(16, rdx) GPR_LOAD(24, rbx) GPR_LOAD(40, rbp) GPR_LOAD(48, rsi) GPR_LOAD(56, rdi)
    GPR_LOAD(64, r8) GPR_LOAD(72, r9) GPR_LOAD(80, r10) GPR_LOAD(88, r11) GPR_LOAD(96, r12) GPR_LOAD(104, r13)
    GPR_LOAD(112, r14) GPR_LOAD(120, r15)
    "  cmpb $0, g_tf(%rip)\n  je 4f\n  pushfq\n  orq $0x100, (%rsp)\n  popfq\n"
    "4:\n  pushfq\n  popq g_flags_in(%rip)\n  jmp *g_entry(%rip)\n"

    ".globl recorder\nrecorder:\n"
    "  pushfq\n  popq g_flags_out(%rip)\n"
    GPR_STORE(0, rax) GPR_STORE(8, rcx) GPR_STORE(16, rdx) GPR_STORE(24, rbx) GPR_STORE(32, rsp) GPR_STORE(40, rbp)
    GPR_STORE(48, rsi) GPR_STORE(56, rdi) GPR_STORE(64, r8) GPR_STORE(72, r9) GPR_STORE(80, r10) GPR_STORE(88, r11)
    GPR_STORE(96, r12) GPR_STORE(104, r13) GPR_STORE(112, r14) GPR_STORE(120, r15)
    "  cmpb $0, g_avx(%rip)\n  je 5f\n"
    ALL16(YSTORE)
    "  vzeroupper\n  jmp 6f\n5:\n"
    ALL16(XSTORE)
    "6:\n"
    "  movq (%rsp), %rax\n  movq %rax, g_out+640(%rip)\n  movq 8(%rsp), %rax\n  movq %rax, g_out+648(%rip)\n"
    "  movq 16(%rsp), %rax\n  movq %rax, g_out+656(%rip)\n"
    "  movq g_save+224(%rip), %rsp\n"
    "  movq g_save+0(%rip), %rbx\n  movq g_save+8(%rip), %rbp\n  movq g_save+16(%rip), %rsi\n  movq g_save+24(%rip), %rdi\n"
    "  movq g_save+32(%rip), %r12\n  movq g_save+40(%rip), %r13\n  movq g_save+48(%rip), %r14\n  movq g_save+56(%rip), %r15\n"
    "  movdqu g_save+64(%rip), %xmm6\n  movdqu g_save+80(%rip), %xmm7\n  movdqu g_save+96(%rip), %xmm8\n"
    "  movdqu g_save+112(%rip), %xmm9\n  movdqu g_save+128(%rip), %xmm10\n  movdqu g_save+144(%rip), %xmm11\n"
    "  movdqu g_save+160(%rip), %xmm12\n  movdqu g_save+176(%rip), %xmm13\n  movdqu g_save+192(%rip), %xmm14\n"
    "  movdqu g_save+208(%rip), %xmm15\n"
    "  ret\n");

/* ---- single-step unwind checker ---- */
static uintptr_t g_lo, g_hi;          /* thunk code range */
static int g_steps, g_unwind_bad;
static char g_bad_msg[160];

static LONG CALLBACK veh(EXCEPTION_POINTERS *ep)
{
    CONTEXT *c = ep->ContextRecord;
    if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP) return EXCEPTION_CONTINUE_SEARCH;
    if (c->Rip == (DWORD64)(uintptr_t)recorder) { c->EFlags &= ~0x100u; return EXCEPTION_CONTINUE_EXECUTION; }
    if (c->Rip >= g_lo && c->Rip < g_hi) {
        DWORD64 ib = 0, est = 0;
        void *hd = NULL;
        CONTEXT u = *c;
        PRUNTIME_FUNCTION fe = RtlLookupFunctionEntry(c->Rip, &ib, NULL);
        g_steps++;
        if (!fe) {
            if (!g_unwind_bad++) snprintf(g_bad_msg, sizeof g_bad_msg, "no function entry at +%llu", (unsigned long long)(c->Rip - g_lo));
        } else {
            DWORD64 got[16];
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, ib, c->Rip, fe, &u, &hd, &est, NULL);
            got[0] = u.Rax; got[1] = u.Rcx; got[2] = u.Rdx; got[3] = u.Rbx; got[4] = u.Rsp; got[5] = u.Rbp; got[6] = u.Rsi; got[7] = u.Rdi;
            got[8] = u.R8; got[9] = u.R9; got[10] = u.R10; got[11] = u.R11; got[12] = u.R12; got[13] = u.R13; got[14] = u.R14; got[15] = u.R15;
            if (u.Rip != g_in.at_rsp[0] || u.Rsp != g_in.g[4] + 8) {
                if (!g_unwind_bad++) snprintf(g_bad_msg, sizeof g_bad_msg, "at +%llu: unwound rip %llx rsp %llx", (unsigned long long)(c->Rip - g_lo),
                                              (unsigned long long)u.Rip, (unsigned long long)u.Rsp);
            } else {
                for (int i = 0; i < 16; i++)
                    if (i != 4 && got[i] != g_in.g[i]) {
                        if (!g_unwind_bad++) snprintf(g_bad_msg, sizeof g_bad_msg, "at +%llu: GPR %d not restored", (unsigned long long)(c->Rip - g_lo), i);
                        break;
                    }
            }
        }
    }
    c->EFlags |= 0x100u;
    return EXCEPTION_CONTINUE_EXECUTION;
}

static uint64_t g_seed = 0x123456789ABCDEFull;
static uint64_t rnd(void) { g_seed ^= g_seed << 13; g_seed ^= g_seed >> 7; g_seed ^= g_seed << 17; return g_seed; }

static void fill_in(uint64_t key)
{
    for (int i = 0; i < 16; i++) g_in.g[i] = rnd();
    for (int i = 0; i < 16; i++) for (int k = 0; k < 32; k += 8) { uint64_t v = rnd(); memcpy(&g_in.x[i][k], &v, 8); }
    g_in.at_rsp[0] = key;
    g_in.at_rsp[1] = 0x1111111111111111ull;
    g_in.at_rsp[2] = 0x2222222222222222ull;
}

/* one run through `entry`; returns 1 if everything arrived unchanged */
static int transparent(const char *what, void *entry, uint64_t key)
{
    int ok = 1, nx = g_avx ? 32 : 16;
    fill_in(key);
    memset(&g_out, 0, sizeof g_out);
    g_entry = entry;
    run_harness();
    for (int i = 0; i < 16; i++)
        if (g_out.g[i] != g_in.g[i]) { ok = 0; printf("  %s: GPR %d changed (%llx -> %llx)\n", what, i, (unsigned long long)g_in.g[i], (unsigned long long)g_out.g[i]); }
    for (int i = 0; i < 16; i++)
        if (memcmp(g_out.x[i], g_in.x[i], (size_t)nx)) { ok = 0; printf("  %s: %smm%d changed\n", what, g_avx ? "y" : "x", i); }
    if (memcmp(g_out.at_rsp, g_in.at_rsp, sizeof g_in.at_rsp)) { ok = 0; printf("  %s: return address or canaries changed\n", what); }
    return ok;
}

static void range_of(void *entry, uintptr_t *lo, uintptr_t *hi)
{
    DWORD64 ib = 0;
    PRUNTIME_FUNCTION fe = RtlLookupFunctionEntry((DWORD64)(uintptr_t)entry, &ib, NULL);
    *lo = fe ? (uintptr_t)(ib + fe->BeginAddress) : 0;
    *hi = fe ? (uintptr_t)(ib + fe->EndAddress) : 0;
}

static void test_one(const char *what, probe_ctx *c, uint64_t key)
{
    uintptr_t lo, hi;
    range_of(c->entry, &lo, &hi);
    CHECK(lo == (uintptr_t)c->entry, "%s: function table entry starts at the thunk", what);
    for (int mis = 0; mis < 2; mis++) {
        g_misalign = (uint8_t)mis;
        g_tf = 0;
        CHECK(transparent(what, c->entry, key), "%s: register/stack transparent (%s entry rsp)", what, mis ? "misaligned" : "call-aligned");
        g_lo = lo; g_hi = hi; g_steps = 0; g_unwind_bad = 0; g_bad_msg[0] = 0;
        g_tf = 1;
        transparent(what, c->entry, key);
        g_tf = 0;
        CHECK(g_steps > 0 && !g_unwind_bad, "%s: RtlVirtualUnwind exact at all %d single-stepped instructions (%s): %s", what, g_steps,
              mis ? "misaligned" : "call-aligned", g_bad_msg);
        if (!mis) printf("  %s: %d instructions single-stepped, unwind exact at each\n", what, g_steps);
    }
}

/* ---- C-callable use for counters and stress ---- */
typedef int (*ifn)(int);
__attribute__((noinline)) static int target_fn(int a) { return a * 3 + 1; }

#define CALL8(f, x) (f(x) + f(x + 1) + f(x + 2) + f(x + 3) + f(x + 4) + f(x + 5) + f(x + 6) + f(x + 7))
static ifn volatile g_stress_fn;
static DWORD WINAPI stress(LPVOID p)
{
    volatile int s = 0;
    ifn f = g_stress_fn;
    (void)p;
    for (int i = 0; i < 20000; i++) s += CALL8(f, i);
    return (DWORD)s;
}

static void run_threads(ifn f, int n)
{
    HANDLE h[8];
    g_stress_fn = f;
    for (int i = 0; i < n; i++) h[i] = CreateThread(NULL, 0, stress, NULL, 0, NULL);
    WaitForMultipleObjects((DWORD)n, h, TRUE, INFINITE);
    for (int i = 0; i < n; i++) CloseHandle(h[i]);
}

static uint64_t hist_find(int slot, uint64_t key)
{
    for (int i = 0; i < HIST_SLOTS; i++) if (g_probe_hist[slot][i].key == key) return g_probe_hist[slot][i].count;
    return 0;
}

int main(void)
{
    probe_ctx *cnt, *cnt2, *hist0, *hist1, *ord, *xell;
    uint8_t *t;
    void *vh;

    g_avx = IsProcessorFeaturePresent(PF_AVX_INSTRUCTIONS_AVAILABLE) ? 1 : 0;
    printf("AVX %s: checking %s registers\n", g_avx ? "present" : "absent", g_avx ? "full ymm" : "xmm");
    if (probe_init()) { printf("probe_init failed\n"); return 1; }
    cnt   = probe_add("count", "entry_count", "test", 0x1000, PK_COUNT, 0);
    hist0 = probe_add("hist0", "retaddr-hist", "test", 0x2000, PK_HIST, 0);
    hist1 = probe_add("hist1", "retaddr-hist", "test", 0x3000, PK_HIST, 1);
    ord   = probe_add("order", "order", "test", 0x4000, PK_ORDER, 0);
    xell  = probe_add("xell", "xell", "test", 0x5000, PK_XELL, 0);
    cnt2  = probe_add("count2", "entry_count", "test", 0x6000, PK_COUNT, 0);
    if (!(cnt && cnt2 && hist0 && hist1 && ord && xell)) { printf("probe_add failed\n"); return 1; }
    CHECK(!probe_add("hist0b", "retaddr-hist", "test", 0x7000, PK_HIST, 0), "second probe on hist slot 0 refused");
    ord->rec_type = RT_CAM;
    xell->match_ret = 0x7FF612345678ull;
    cnt->tramp = hist0->tramp = hist1->tramp = ord->tramp = xell->tramp = (void *)recorder;
    probe_set_tramp(cnt); probe_set_tramp(hist0); probe_set_tramp(hist1); probe_set_tramp(ord); probe_set_tramp(xell);
    cnt2->tramp = (void *)target_fn; probe_set_tramp(cnt2);
    CHECK(probe_finalize_stubs() == 0, "finalize (VirtualProtect + RtlAddFunctionTable)");

    t = cnt->entry;
    CHECK(t[0] == 0xF0 && t[1] == 0x48 && t[2] == 0xFF && t[3] == 0x05 && t[8] == 0xFF && t[9] == 0x25,
          "generated thunk is lock inc qword [rip+x]; jmp qword [rip+y]");
    CHECK((uint8_t *)cnt->count == t + 8 + *(int32_t *)(t + 4) && (uint8_t *)cnt->tramp_slot == t + 14 + *(int32_t *)(t + 10),
          "thunk displacements address its counter and trampoline slot");
    CHECK(((uintptr_t)cnt->count & 63) == 0 && ((uintptr_t)cnt2->count & 63) == 0 && ((uintptr_t)hist0->count & 63) == 0 &&
          ((uintptr_t)ord->count & 63) == 0, "each counter starts its own 64-byte line");

    vh = AddVectoredExceptionHandler(1, veh);
    test_one("entry-count thunk", cnt, 0x7FF600001234ull);
    test_one("hist thunk 0", hist0, 0x7FF600005678ull);
    test_one("hist thunk 1", hist1, 0x7FF60000ABCDull);
    g_nrec = 0;
    test_one("C thunk (order)", ord, 0x7FF60000EEEEull);
    CHECK(g_nrec == 4 && g_rec.type == RT_CAM && g_rec.aux == 0x7FF60000EEEEull, "order: probe_hit got ctx and return address (%d records)", g_nrec);
    g_nrec = 0;
    test_one("C thunk (xell, other caller)", xell, 0x7FF600009999ull);
    CHECK(g_nrec == 0, "xell: other return address not timestamped");
    test_one("C thunk (xell, matching caller)", xell, 0x7FF612345678ull);
    CHECK(g_nrec == 4 && g_rec.type == RT_XELL && g_last_xell_qpc != 0, "xell: matching return address timestamped");
    RemoveVectoredExceptionHandler(vh);

    CHECK(*cnt->count == 4 && *hist0->count == 4 && *hist1->count == 4 && *ord->count == 4 && *xell->count == 8,
          "entry counts exact (%llu %llu %llu %llu %llu)", (unsigned long long)*cnt->count, (unsigned long long)*hist0->count,
          (unsigned long long)*hist1->count, (unsigned long long)*ord->count, (unsigned long long)*xell->count);
    CHECK(hist_find(0, 0x7FF600005678ull) == 4 && hist_find(1, 0x7FF60000ABCDull) == 4 && hist_find(0, 0x7FF60000ABCDull) == 0,
          "histogram keyed by return address, per slot");

    /* histogram fill: 700 distinct keys (> 512 slots), key i called (i % 3) + 1 times */
    {
        uint64_t calls = 0, in_table = 0, wrong = 0, dupl = 0, base_ovf = g_probe_hist_ovf[1];
        g_tf = 0; g_misalign = 0;
        for (int i = 0; i < 700; i++)
            for (int k = 0; k <= i % 3; k++) { fill_in(0x7FF700000000ull + (uint64_t)i * 0x40); g_entry = hist1->entry; run_harness(); calls++; }
        for (int i = 0; i < HIST_SLOTS; i++) {
            uint64_t key = g_probe_hist[1][i].key;
            if (!key || key == 0x7FF60000ABCDull) continue;
            in_table += g_probe_hist[1][i].count;
            if (g_probe_hist[1][i].count != (uint64_t)(((key - 0x7FF700000000ull) / 0x40) % 3 + 1)) wrong++;
            for (int j = i + 1; j < HIST_SLOTS; j++) if (g_probe_hist[1][j].key == key) dupl++;
        }
        CHECK(in_table + (g_probe_hist_ovf[1] - base_ovf) == calls && !wrong && !dupl,
              "histogram fill: %llu calls = %llu in table + %llu overflow, %llu wrong counts, %llu duplicate keys",
              (unsigned long long)calls, (unsigned long long)in_table, (unsigned long long)(g_probe_hist_ovf[1] - base_ovf),
              (unsigned long long)wrong, (unsigned long long)dupl);
        CHECK(g_probe_hist_ovf[1] - base_ovf > 0, "overflow path exercised");
        printf("  histogram fill: %llu calls, %llu in table, %llu overflow\n", (unsigned long long)calls, (unsigned long long)in_table,
               (unsigned long long)(g_probe_hist_ovf[1] - base_ovf));
    }

    /* threads: 8 threads x 20000 x 8 call sites through the count thunk */
    run_threads((ifn)cnt2->entry, 8);
    CHECK(*cnt2->count == 8ull * 20000 * 8, "8-thread entry count exact: %llu", (unsigned long long)*cnt2->count);
    /* threads through hist thunk 0 (tramp -> target_fn): 8 call sites = 8 keys, no duplicates */
    hist0->tramp = (void *)target_fn; probe_set_tramp(hist0);
    {
        uint64_t before = *hist0->count, sum = 0, total;
        int keys = 0;
        run_threads((ifn)hist0->entry, 8);
        for (int i = 0; i < HIST_SLOTS; i++) {
            uint64_t key = g_probe_hist[0][i].key;
            if (!key || key == 0x7FF600005678ull) continue;
            keys++;
            sum += g_probe_hist[0][i].count;
        }
        total = *hist0->count - before;
        CHECK(total == 8ull * 20000 * 8 && sum == total && keys == 8,
              "8-thread histogram: %llu calls, %llu counted under %d keys (want 8)", (unsigned long long)total, (unsigned long long)sum, keys);
    }

    printf("%d checks, %d failures\n", g_checks, g_fail);
    printf(g_fail ? "TEST_THUNK: FAIL\n" : "TEST_THUNK: PASS\n");
    return g_fail ? 1 : 0;
}
