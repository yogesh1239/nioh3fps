/* Semantic test of the P1/P2 stub bytes built by src/stubs.c: the stubs are entered with
   the register state of the patch site and "return" (jmp rax) to a landing pad instead of
   the game. Checks:
   P1: [obj+0x624] = [obj+0x628] * r1, xmm2 holds the product, rdi untouched;
   P2: xmm7 = 60/eax * r2 only when eax == 120, otherwise exactly 60/eax (original math). */
#include "../src/stubs.h"
#include <stdio.h>
#include <string.h>
#include <math.h>

static int g_fail, g_checks;
#define CHECK(c, ...) do { g_checks++; if (!(c)) { g_fail++; printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* run_p1(stub, obj): rdi = obj, jump to stub; landing_p1 restores and returns xmm2 */
float run_p1(void *stub, void *obj);
/* run_p2(stub, fps): eax = fps (rax zero-extended), xmm0 = 0, jump to stub; landing_p2 returns xmm7 */
float run_p2(void *stub, int fps);
void landing_p1(void);
void landing_p2(void);

__asm__(
    ".globl run_p1\n run_p1:\n"
    "  pushq %rdi\n"
    "  movq %rdx, %rdi\n"
    "  movl $0xDEAD, %eax\n"
    "  jmp *%rcx\n"
    ".globl landing_p1\n landing_p1:\n"
    "  movaps %xmm2, %xmm0\n"
    "  popq %rdi\n"
    "  ret\n"
    ".globl run_p2\n run_p2:\n"
    "  subq $24, %rsp\n"
    "  movdqu %xmm7, (%rsp)\n"
    "  movl %edx, %eax\n"
    "  xorps %xmm0, %xmm0\n"
    "  jmp *%rcx\n"
    ".globl landing_p2\n landing_p2:\n"
    "  movaps %xmm7, %xmm0\n"
    "  movdqu (%rsp), %xmm7\n"
    "  addq $24, %rsp\n"
    "  ret\n");

int main(void)
{
    static const uint8_t uw[8] = { 0x01, 0x00, 0x00, 0x00, 0, 0, 0, 0 };   /* minimal host info: no codes */
    uint8_t *alloc = VirtualAlloc(NULL, 2 * STUB_PAGE, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    stubs_params sp; stubs_layout L; char err[128];
    DWORD old;
    uint8_t obj[0x800];
    float speeds[] = { 1.0f, 0.1f, 0.0f, 1.7f, 0.333f };
    float rs[] = { 1.0f, 0.5f, 1.37f, 2.0f, 0.8333333f };
    int fpss[] = { 120, 60, 30, 121, 0x7FFFFFFF };

    sp.alloc = alloc; sp.p1_return = (uintptr_t)landing_p1; sp.p2_return = (uintptr_t)landing_p2;
    sp.p1_host_unwind = uw; sp.p2_host_unwind = uw;
    if (stubs_build(&sp, &L, err, sizeof err)) { printf("build: %s\n", err); return 1; }
    VirtualProtect(alloc, STUB_PAGE, PAGE_EXECUTE_READ, &old);
    FlushInstructionCache(GetCurrentProcess(), alloc, STUB_PAGE);

    for (unsigned i = 0; i < sizeof rs / sizeof *rs; i++)
        for (unsigned j = 0; j < sizeof speeds / sizeof *speeds; j++) {
            float x;
            memset(obj, 0xAB, sizeof obj);
            memcpy(obj + 0x628, &speeds[j], 4);
            *L.r1 = rs[i];
            x = run_p1(L.p1_stub, obj);
            CHECK(!memcmp(obj + 0x624, &(float){ speeds[j] * rs[i] }, 4), "P1 [+0x624] = %g*%g", speeds[j], rs[i]);
            CHECK(x == speeds[j] * rs[i], "P1 xmm2");
            CHECK(!memcmp(obj + 0x628, &speeds[j], 4) && obj[0x623] == 0xAB && obj[0x62c] == 0xAB, "P1 touches only +0x624");
        }
    *L.r1 = 1.0f;
    for (unsigned i = 0; i < sizeof rs / sizeof *rs; i++)
        for (unsigned j = 0; j < sizeof fpss / sizeof *fpss; j++) {
            float x, orig, want;
            *L.r2 = rs[i];
            x = run_p2(L.p2_stub, fpss[j]);
            orig = 60.0f / (float)fpss[j];          /* the game's own math: movss 60; cvtsi2ss; divss */
            want = fpss[j] == 120 ? orig * rs[i] : orig;
            CHECK(x == want, "P2 fps=%d r2=%g: got %g want %g", fpss[j], rs[i], x, want);
        }
    *L.r2 = 1.0f;
    CHECK(run_p2(L.p2_stub, 120) == 0.5f, "P2 passthrough at 120 equals the original 0.5");
    DWORD tls=TlsAlloc();
    CHECK(tls<64,"inline native TLS slot");
    VirtualProtect(alloc,STUB_PAGE,PAGE_READWRITE,&old);
    stubs_s2l(&sp,&L,0x728,0x724,tls);
    VirtualProtect(alloc,STUB_PAGE,PAGE_EXECUTE_READ,&old);
    for (int guard=0;guard<2;guard++) {
        TlsSetValue(tls,(void *)(uintptr_t)guard);
        for (unsigned i=0;i<sizeof rs/sizeof *rs;i++) for (unsigned j=0;j<sizeof speeds/sizeof *speeds;j++) {
            memset(obj,0xab,sizeof obj); memcpy(obj+0x728,&speeds[j],4); *L.r1=rs[i];
            float want=speeds[j]*(guard ? 1.0f : rs[i]);
            float got=run_p1(L.p1_stub,obj);
            CHECK(got==want && !memcmp(obj+0x724,&want,4),"decoded P1 fields, in_gate=%d",guard);
            CHECK(!memcmp(obj+0x728,&speeds[j],4) && obj[0x624]==0xab,"P1 leaves original offsets/req untouched");
        }
        for (unsigned i=0;i<sizeof rs/sizeof *rs;i++) for (unsigned j=0;j<sizeof fpss/sizeof *fpss;j++) {
            *L.r2=rs[i]; float orig=60.0f/(float)fpss[j];
            float want=(!guard && fpss[j]==120) ? orig*rs[i] : orig;
            CHECK(run_p2(L.p2_stub,fpss[j])==want,"S2L P2 fps=%d in_gate=%d",fpss[j],guard);
        }
    }
    TlsSetValue(tls,NULL); TlsFree(tls);
    printf("%d checks, %d failures\n", g_checks, g_fail);
    printf(g_fail ? "TEST_STUBS: FAIL\n" : "TEST_STUBS: PASS\n");
    return g_fail ? 1 : 0;
}
