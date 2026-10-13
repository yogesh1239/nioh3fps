/* Heartbeat assembly transparency, per-instruction native unwind, real host stub unwind. */
#include "../src/common.h"
#include "../src/heartbeat.h"
#include <stdio.h>
#include <string.h>
static int g_fail,g_checks;
#define CHECK(c,...) do { g_checks++; if (!(c)) { g_fail++; printf("FAIL: "); printf(__VA_ARGS__); puts(""); } } while(0)
uint64_t g_in_flags,g_out_flags;
uint8_t g_stubmode;
uint64_t g_packet=0x1122334455667788ull;
static uint64_t test_packet=0xabcdef1234567890ull, test_mailbox=0x123456789, test_frame=42;
extern void heartbeat_leaf_end(void);
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
    "  movq %rsp, g_in+32(%rip)\n  movq g_packet(%rip), %rax\n  movq %rax,120(%rsp)\n"
    "  cmpb $0, g_avx(%rip)\n  je 2f\n"
    ALL16(YLOAD)
    "  jmp 3f\n2:\n"
    ALL16(XLOAD)
    "3:\n"
    GPR_LOAD(0, rax) GPR_LOAD(8, rcx) GPR_LOAD(16, rdx) GPR_LOAD(24, rbx) GPR_LOAD(40, rbp) GPR_LOAD(48, rsi) GPR_LOAD(56, rdi)
    GPR_LOAD(64, r8) GPR_LOAD(72, r9) GPR_LOAD(80, r10) GPR_LOAD(88, r11) GPR_LOAD(96, r12) GPR_LOAD(104, r13)
    GPR_LOAD(112, r14) GPR_LOAD(120, r15)
    "  cmpb $0, g_tf(%rip)\n  je 4f\n  pushfq\n  orq $0x100, (%rsp)\n  popfq\n"
    "4:\n  pushfq\n  popq g_in_flags(%rip)\n  jmp *g_entry(%rip)\n"

    ".globl recorder\nrecorder:\n  pushfq\n  popq g_out_flags(%rip)\n"
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
    g_in.g[3]=test_mailbox;
    g_in.g[12]=test_packet;
    g_in.g[13]=test_frame;
    memset(&g_out, 0, sizeof g_out);
    g_entry = entry;
    run_harness();
    for (int i = 0; i < 16; i++)
        if (g_out.g[i] != (i==1 && g_stubmode ? g_packet : g_in.g[i]+(i==4 && !g_stubmode ? 8 : 0))) { ok = 0; printf("  %s: GPR %d changed (%llx -> %llx)\n", what, i, (unsigned long long)g_in.g[i], (unsigned long long)g_out.g[i]); }
    for (int i = 0; i < 16; i++)
        if (memcmp(g_out.x[i], g_in.x[i], (size_t)nx)) { ok = 0; printf("  %s: %smm%d changed\n", what, g_avx ? "y" : "x", i); }
    if (memcmp(g_out.at_rsp, g_in.at_rsp+(g_stubmode ? 0 : 1), g_stubmode ? 24 : 16)) { ok = 0; printf("  %s: return address or canaries changed\n", what); }
    if (((g_out_flags^g_in_flags)&~0x10100ull)!=0) { ok=0; puts("flags changed"); }
    return ok;
}


void fake_qpc(void);
__asm__(".globl fake_qpc\nfake_qpc:\n"
        "movq $1234567,(%rcx)\n"
        "xorq %rax,%rax\nxorq %rcx,%rcx\nxorq %rdx,%rdx\nxorq %r8,%r8\nxorq %r9,%r9\nxorq %r10,%r10\nxorq %r11,%r11\n"
        "pxor %xmm0,%xmm0\npxor %xmm1,%xmm1\npxor %xmm2,%xmm2\npxor %xmm3,%xmm3\npxor %xmm4,%xmm4\npxor %xmm5,%xmm5\nret\n");

static uint8_t *file_image(const char *path,size_t *size)
{
    FILE *f=fopen(path,"rb"); if (!f) return NULL;
    fseek(f,0,SEEK_END); size_t n=(size_t)ftell(f); rewind(f);
    uint8_t *file=malloc(n); if (fread(file,1,n,f)!=n) return NULL; fclose(f);
    IMAGE_NT_HEADERS64 *nt=(void *)(file+((IMAGE_DOS_HEADER *)file)->e_lfanew);
    *size=nt->OptionalHeader.SizeOfImage;
    uint8_t *img=calloc(1,*size); memcpy(img,file,nt->OptionalHeader.SizeOfHeaders);
    IMAGE_SECTION_HEADER *s=IMAGE_FIRST_SECTION(nt);
    for (int j=0;j<nt->FileHeader.NumberOfSections;j++) memcpy(img+s[j].VirtualAddress,file+s[j].PointerToRawData,s[j].SizeOfRawData);
    free(file); return img;
}
static CONTEXT unwind_at(uintptr_t ip,uintptr_t rsp)
{
    CONTEXT c={0}; DWORD64 ib=0,est=0; void *hd=NULL;
    c.ContextFlags=CONTEXT_FULL; c.Rip=ip; c.Rsp=rsp;
    c.Rbx=1; c.Rbp=2; c.Rdi=3; c.Rsi=4; c.R12=5; c.R13=6; c.R14=7; c.R15=8;
    PRUNTIME_FUNCTION fe=RtlLookupFunctionEntry(ip,&ib,NULL);
    CHECK(fe!=NULL,"function entry at sampled instruction");
    if (fe) RtlVirtualUnwind(UNW_FLAG_NHANDLER,ib,ip,fe,&c,&hd,&est,NULL);
    return c;
}
static hb_rec record_at(void *entry,uint64_t packet,uint64_t frame)
{
    test_packet=packet; test_frame=frame;
    CHECK(transparent("gameplay record",entry,(uintptr_t)recorder),"record preserves all registers/flags/YMM and original stack");
    hb_rec r=g_hb_ring[(g_hb_write-1)&(HB_COUNT-1)];
    CHECK(r.packet==packet && r.frame_no==(uint32_t)frame && r.mailbox==test_mailbox &&
          r.qpc==1234567 && r.tid==GetCurrentThreadId(),"r12/r13d/rbx and QPC recorded");
    return r;
}
static void record_tests(void *entry)
{
    g_qpc_fn=(void *)fake_qpc; g_heartbeat_source=0;
    g_render_tid=(LONG)GetCurrentThreadId(); g_expected_mailbox=test_mailbox;
    g_previous_packet=0; g_previous_frame=0;
    LONG64 seq=g_take_seq, gate=g_render_seq;
    hb_rec r=record_at(entry,0x100,0);
    CHECK(!r.flags && r.seq==(uint64_t)++seq,"first frame zero is fresh");
    r=record_at(entry,0x200,0);
    CHECK(r.flags==2 && r.seq==(uint64_t)++seq,"same frame/different pointer repeats (pointer-only negative control)");
    CHECK(r.packet!=0x100,"pointer-only classifier would miss this repeat");
    r=record_at(entry,0x300,1);
    CHECK(!r.flags && r.seq==(uint64_t)++seq,"both changed: repeat negative control");
    r=record_at(entry,0x300,2);
    CHECK(r.flags==2 && r.seq==(uint64_t)++seq,"pointer comparison retained as secondary");
    LONG64 qpc=g_render_qpc, conflict=g_render_conflict;
    r=record_at(entry,0,999);
    CHECK(r.flags==4 && r.seq==(uint64_t)seq && g_take_seq==seq && g_render_seq==gate+4 &&
          g_render_qpc==qpc && g_render_conflict==conflict && g_previous_packet==0x300 &&
          g_previous_frame==2,"null cannot advance sequence, liveness or repeat history");
    r=record_at(entry,0x400,999);
    CHECK(!r.flags && r.seq==(uint64_t)++seq,"null frame does not make next packet repeat");
    test_mailbox++;
    r=record_at(entry,0x500,1000);
    CHECK(r.flags==1 && r.seq==(uint64_t)seq && g_previous_frame==999,"unrelated rbx mailbox cannot advance heartbeat/history");
    /* Apply H1's relatch reset to this generated stub; integration tests below
       separately exercise actual H1 qualification and replacement with the leaf. */
    g_expected_mailbox=test_mailbox; g_previous_packet=0; g_previous_frame=0;
    r=record_at(entry,0x400,999);
    CHECK(!r.flags && r.seq==(uint64_t)++seq,"relatched pair starts fresh even with identical frame/pointer");
    g_heartbeat_source=2;
    r=record_at(entry,0x600,1001);
    CHECK(!r.flags && r.seq==(uint64_t)++seq && g_render_seq==gate+6,"xell_exit keeps physical take records without publishing gate sequence");
    gate_tls_set(1); LONG64 writes=g_hb_write;
    CHECK(transparent("in-gate gameplay stub",entry,(uintptr_t)recorder),"suppressed stub remains transparent");
    CHECK(g_hb_write==writes && g_take_seq==seq,"in-gate stub negative control records nothing");
    gate_tls_set(0);
}
static void stub_tests(const char *path)
{
    size_t size; uint8_t *img=file_image(path,&size); CHECK(img!=NULL,"read-only unpacked image"); if (!img) return;
    IMAGE_NT_HEADERS64 *nt=(void *)(img+((IMAGE_DOS_HEADER *)img)->e_lfanew);
    IMAGE_DATA_DIRECTORY pd=nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
    RUNTIME_FUNCTION *r=(void *)(img+pd.VirtualAddress),*host=NULL;
    for (size_t j=0;j<pd.Size/sizeof *r;j++) if (r[j].BeginAddress<=0x3d20eb && r[j].EndAddress>0x3d20eb) host=r+j;
    CHECK(host!=NULL,"real packet-take host .pdata"); if (!host) return;
    uint8_t *a=VirtualAlloc(NULL,0x10000,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);
    RUNTIME_FUNCTION *tbl; char err[128]; DWORD old;
    CHECK(heartbeat_build(a,(uintptr_t)(img+0x1a7495f),img+host->UnwindData,&tbl,err,sizeof err)!=0,"old take instruction negative control rejected");
    CHECK(!heartbeat_build(a,(uintptr_t)(img+0x3d20eb),img+host->UnwindData,&tbl,err,sizeof err),"real original-rsp stub: %s",err);
    /* Reference host code + original metadata, registered against a private copy. */
    size_t len=host->EndAddress-host->BeginAddress;
    CHECK(len<0x8000,"host fits reference allocation");
    memcpy(a+0x1000,img+host->BeginAddress,len);
    memcpy(a+0xf000,img+host->UnwindData,stubs_unwind_len(img+host->UnwindData)+16);
    memcpy(a+0x500,a,23);
    memcpy(a+0xf040,img+host->UnwindData,stubs_unwind_len(img+host->UnwindData)+16);
    tbl[1]=(RUNTIME_FUNCTION){0x500,0x517,0xf040};
    tbl[2]=(RUNTIME_FUNCTION){0x1000,0x1000+(DWORD)len,0xf000};
    VirtualProtect(a,0x10000,PAGE_EXECUTE_READ,&old);
    CHECK(RtlAddFunctionTable(tbl,3,(DWORD64)(uintptr_t)a),"stub and reference unwind registration");
    __attribute__((aligned(16))) uint64_t stack[512];
    for (int j=0;j<512;j++) stack[j]=0xdead000000000000ull+j;
    uintptr_t body=(uintptr_t)(stack+128);
    CONTEXT ref=unwind_at((uintptr_t)(a+0x1000+0x3d20eb-host->BeginAddress),body);
    CHECK(ref.Rip==*(uint64_t *)(body+0x68) && ref.Rsp==body+0x70,"reference reaches host caller");
    int offsets[]={0,5,11,17};
    unsigned negative_bad=0;
    for (int j=0;j<4;j++) {
        CONTEXT got=unwind_at((uintptr_t)(a+offsets[j]),body);
        CHECK(!memcmp(&got,&ref,sizeof got),"HB stub instruction +%d equals real host unwind",offsets[j]);
        CONTEXT bad=unwind_at((uintptr_t)(a+0x500+offsets[j]),body);
        negative_bad+=memcmp(&bad,&ref,sizeof bad)!=0;
    }
    CHECK(negative_bad>0,"untransformed host UI negative control must fail");
    /* Execute the generated stack load against the harness's original body rsp.
       Give its continuation a landing pad in the same allocation (rel32 range). */
    RtlDeleteFunctionTable(tbl); VirtualProtect(a,0x10000,PAGE_READWRITE,&old);
    memcpy(a+0x800,"\x48\x8B\x4C\x24\x78\xFF\x25\0\0\0\0",11);
    uintptr_t target=(uintptr_t)recorder; memcpy(a+0x80b,&target,8);
    CHECK(!heartbeat_build(a,(uintptr_t)(a+0x800),img+host->UnwindData,&tbl,err,sizeof err),"execution stub");
    VirtualProtect(a,0x10000,PAGE_EXECUTE_READ,&old); FlushInstructionCache(GetCurrentProcess(),a,0x10000);
    g_stubmode=1; g_misalign=1;
    CHECK(transparent("original-rsp packet stub",a,(uintptr_t)recorder),"packet load, GPR/flags/YMM/stack preservation through real stub");
    CHECK(g_hb_ring[g_hb_write-1].packet==test_packet,"heartbeat captures r12 independently of displaced rcx load");
    record_tests(a);
    g_stubmode=0; g_misalign=0;
    free(img);
}
int main(int argc,char **argv)
{
    if (argc<2) return 2;
    g_gate_tls=TlsAlloc(); CHECK(g_gate_tls<64,"inline native TLS available");
    g_avx=IsProcessorFeaturePresent(PF_AVX_INSTRUCTIONS_AVAILABLE);
    g_qpc_fn=(void *)fake_qpc;
    g_expected_mailbox=0x123456789;
    g_render_tid=(LONG)GetCurrentThreadId(); /* H1 owns initial pair capture */
    void *vh=AddVectoredExceptionHandler(1,veh);
    for (int path=0;path<9;path++) {
        gate_tls_set(path==3 ? 1 : 0);
        DWORD saved_tls=g_gate_tls;
        if (path==7) g_gate_tls=64;
        g_expected_mailbox=path==2 ? 0x556677 : 0x123456789;
        g_heartbeat_source=path==4 ? 1 : 0;
        if (path==5) g_render_tid=(LONG)GetCurrentThreadId()+1;
        if (path==6) { g_render_tid=(LONG)GetCurrentThreadId(); g_qpc_fn=(void *)QueryPerformanceCounter; }
        if (path==8) test_packet=0;
        g_lo=(uintptr_t)heartbeat_leaf; g_hi=(uintptr_t)heartbeat_leaf_end;
        g_steps=g_unwind_bad=0; g_tf=1;
        CHECK(transparent("heartbeat helper",(void *)heartbeat_leaf,(uintptr_t)recorder),"GPR/flags/YMM/stack path %d",path);
        g_tf=0;
        CHECK(g_steps>0 && !g_unwind_bad,"every helper instruction path %d: %d steps, %d bad: %s",path,g_steps,g_unwind_bad,g_bad_msg);
        printf("helper path %d: %d instructions, %d unwind failures\n",path,g_steps,g_unwind_bad);
        g_gate_tls=saved_tls;
    }
    RemoveVectoredExceptionHandler(vh); gate_tls_set(0);
    CHECK(g_render_seq==3,"selected heartbeat count %lld",(long long)g_render_seq);
    CHECK(g_take_seq==4 && g_hb_ring[3].seq==3,"physical HB sequence advances in xell modes too");
    CHECK(g_hb_write==7 && g_hb_ring[1].flags==2 && g_hb_ring[2].flags==1 && g_hb_ring[4].flags==1 && g_hb_ring[6].flags==4,"repeat/null recorded, in_gate changes no state");
    CHECK(g_render_qpc>0 && g_hb_ring[0].qpc==1234567 && g_hb_ring[0].packet==0xabcdef1234567890ull,"rdtsc-free fake QPC and packet");
    test_packet=0xabcdef1234567890ull; stub_tests(argv[1]);
    printf("%d checks, %d failures\nTEST_HEARTBEAT: %s\n",g_checks,g_fail,g_fail ? "FAIL" : "PASS");
    return g_fail ? 1 : 0;
}
