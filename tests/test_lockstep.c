/* Drive the real H1/H2/runtime gate with a fake native worker/render clock.
   No game code executes. Verifies the hook's clock order, pairing and nested hooks. */
#include "../src/lockstep.h"
#include "../src/resolve.h"
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <stdlib.h>
#include <stdarg.h>
config_t g_cfg;
resolved_t g_resolved;
uintptr_t g_base,g_clock_global;
int64_t g_qpf=120000;
volatile LONG g_toggle=1,g_marker;
volatile LONG64 g_frame,g_last_xell_qpc;
stubs_layout g_stubs;
static int checks,fail;
#define CHECK(x) do { checks++; if (!(x)) { fail++; printf("FAIL %d: %s\n",__LINE__,#x); } } while(0)
static int64_t now=100000;
static int relatch_logs, xell_latch_logs[3], take_latch_logs, mismatch_logs;
static int online_disarm_logs, online_clear_logs;
static int32_t online_obj[2];
static uintptr_t online_ptr=(uintptr_t)online_obj;
static uint8_t clk[64],S[64];
static void *worker=(void *)0xabcdef, *mailbox=(void *)0x123456;
static int emit=1,nested,overlap_check,publish_enabled=1;
static DWORD render_tid=999;
static float seen_r1,seen_r2;
static int64_t arrival;
static trace_rec recs[4096]; static uint64_t nrec;
int64_t test_qpc_now(void) { return now; }
void log_printf(const char *fmt,...)
{
    char line[256]; va_list args; va_start(args,fmt);
    vsnprintf(line,sizeof line,fmt,args); va_end(args);
    if (strstr(line,"HB RELATCH")) relatch_logs++;
    if (strstr(line,"HB LATCH source=take mailbox=")) take_latch_logs++;
    if (strstr(line,"HB WARN publish mailbox mismatch")) mismatch_logs++;
    if (strstr(line,"ONLINE: gate disarmed (flag=")) online_disarm_logs++;
    if (!strcmp(line,"ONLINE: cleared")) online_clear_logs++;
    for (int source=1;source<=2;source++) {
        char expected[96];
        snprintf(expected,sizeof expected,"HB LATCH source=xell_%s tid=%lu",
                 source==1 ? "entry" : "exit",(unsigned long)GetCurrentThreadId());
        if (!strcmp(line,expected)) xell_latch_logs[source]++;
    }
}
trace_rec *trace_begin(uint64_t *idx) { *idx=nrec++; trace_rec *r=recs+(*idx%4096); memset(r,0,sizeof *r); return r; }
void trace_end(trace_rec *r,uint64_t idx) { (void)r; (void)idx; }
void hooks_init_state(void);
/* Simulate render registers/TID on this test thread, using the real assembly
   recorder rather than a C model. QPC is a no-call fake; restore TEB immediately. */
void render_leaf(void *box,DWORD tid,uintptr_t packet,uint32_t frame);
__asm__(".globl render_leaf\nrender_leaf:\npushq %rbx\npushq %r12\npushq %r13\nsubq $48,%rsp\n"
        "movq %rcx,%rbx\nmovq %r8,%r12\nmovl %r9d,%r13d\n"
        "movl %gs:0x48,%eax\nmovl %eax,32(%rsp)\nmovl %edx,%gs:0x48\n"
        "call heartbeat_leaf\nmovl 32(%rsp),%eax\nmovl %eax,%gs:0x48\n"
        "addq $48,%rsp\npopq %r13\npopq %r12\npopq %rbx\nret\n");
static BOOL WINAPI render_qpc(LARGE_INTEGER *q) { q->QuadPart=now; return TRUE; }
static void render_pair(void *box,DWORD tid)
{
    int tls=gate_tls_get(); gate_tls_set(0);
    render_leaf(box,tid,0x765432,(uint32_t)g_hb_write);
    gate_tls_set(tls);
}
static void render_take(void) { render_pair(mailbox,render_tid); }
static int guard_pump,guard_hits;
static LONG CALLBACK hb_guard(EXCEPTION_POINTERS *ep)
{
    if (ep->ExceptionRecord->ExceptionCode!=EXCEPTION_GUARD_PAGE || !guard_pump)
        return EXCEPTION_CONTINUE_SEARCH;
    guard_pump=0; guard_hits++; now++; render_take();
    return EXCEPTION_CONTINUE_EXECUTION;
}
static uint8_t scene(void) { return 0; }
static uint64_t clock_original(void *a1,void *a2,void *a3,void *a4)
{ CHECK(a1==clk && a2==worker && a3==(void *)3 && a4==(void *)4); if (!nested) seen_r1=*g_stubs.r1; return 0xaabbccdd11223344ull; }
static uint64_t update_original(void *a1,void *a2,void *a3,void *a4)
{ (void)a1; if (overlap_check) CHECK(gate_tls_get()==1); CHECK(a2==S && a3==(void *)3 && a4==(void *)4); if (!nested) seen_r2=*g_stubs.r2; return 0x88776655; }
static uint64_t publish_original(void *a1,void *a2,void *a3,void *a4)
{ (void)a2; (void)a3; (void)a4; CHECK(a1==mailbox); return 17; }
static uint64_t frame_original(void *a1,void *a2,void *a3,void *a4)
{
    (void)a1; (void)a2; (void)a3; (void)a4;
    h2_detour(NULL,S,(void *)3,(void *)4);
    if (publish_enabled) publish_detour(mailbox,worker,NULL,NULL);
    if (emit) render_take();
    return 23;
}
static void help(void *ctx,uint8_t flag)
{
    CHECK(ctx==worker && flag==0 && gate_tls_get()==1);
    LONG64 frame=g_frame,hb=g_hb_write; uint64_t records=nrec;
    float r1=*g_stubs.r1,r2=*g_stubs.r2;
    nested=1;
    CHECK(h1_detour(clk,worker,(void *)3,(void *)4)==0xaabbccdd11223344ull);
    CHECK(h2_detour(NULL,S,(void *)3,(void *)4)==0x88776655);
    heartbeat_leaf(); /* recursive worker heartbeat is also suppressed */
    nested=0;
    CHECK(g_frame==frame && g_hb_write==hb && nrec==records && *g_stubs.r1==r1 && *g_stubs.r2==r2);
    now+=100;
    if (arrival && now>=arrival) { render_take(); arrival=0; }
}
static void tick(void)
{
    CHECK(h1_detour(clk,worker,(void *)3,(void *)4)==0xaabbccdd11223344ull);
    CHECK(frame_detour(NULL,worker,NULL,NULL)==23);
    CHECK(gate_tls_get()==0);
}

uint64_t call_xell(void);
void xell_test_return(void);
__asm__(".globl call_xell\ncall_xell:\nsubq $40,%rsp\nmovl $1,%ecx\nmovl $2,%edx\nmovl $3,%r8d\nmovl $4,%r9d\ncall xell_detour\n"
        ".globl xell_test_return\nxell_test_return:\naddq $40,%rsp\nret\n");
static uint64_t xell_original(void *a1,void *a2,void *a3,void *a4)
{ CHECK(a1==(void *)1 && a2==(void *)2 && a3==(void *)3 && a4==(void *)4); now+=7; SetLastError(321); return 0x1122334455667788ull; }
/* A second real xell caller, synchronised with the fake-clock owner. */
static HANDLE xell_go,xell_done;
static volatile int xell_stop;
static DWORD WINAPI xell_worker(void *ctx)
{
    (void)ctx;
    for (;;) {
        WaitForSingleObject(xell_go,INFINITE);
        if (xell_stop) return 0;
        call_xell(); SetEvent(xell_done);
    }
}
static void xell_call(int other)
{
    if (other) { SetEvent(xell_go); CHECK(WaitForSingleObject(xell_done,5000)==WAIT_OBJECT_0); }
    else CHECK(call_xell()==0x1122334455667788ull);
}
static void xell_tick(int other,int64_t step)
{
    now+=step-7; xell_call(other); tick();
}
static void take_reset(void)
{
    emit=publish_enabled=0; g_toggle=0; g_heartbeat_source=0; now+=3*g_qpf;
    g_render_tid=0; g_expected_mailbox=g_published_mailbox=0;
    g_render_qpc=g_render_conflict=g_render_seq=g_hb_relatches=0;
    g_previous_packet=0; g_previous_frame=0;
    hooks_init_state();
}
static void take_latch_tests(void)
{
    void *a=(void *)0x111111, *b=(void *)0x222222, *burst=(void *)0x333333;
    take_reset(); int logs=take_latch_logs;
    /* Live finding: 95/s from one pair must qualify with no publisher at all. */
    for (int j=0;j<95;j++) { now+=1264; render_pair(a,2001); tick(); }
    CHECK(!g_render_tid);
    now+=1264; render_pair(a,2001); tick();
    CHECK(g_expected_mailbox==(uintptr_t)a && g_render_tid==2001 && !g_published_mailbox);
    CHECK(take_latch_logs==logs+1 && !g_hb_relatches && !g_l3_tick);
    LONG64 seq=g_render_seq;
    now+=1000; render_pair(a,2001);
    CHECK(g_render_seq==seq+1 && !(g_hb_ring[(g_hb_write-1)%HB_COUNT].flags&1));
    render_pair(a,2002); render_pair(b,2001);
    CHECK(g_render_seq==seq+1 && g_hb_ring[(g_hb_write-1)%HB_COUNT].flags==1);
    /* A conflicting advisory publisher cannot change the selected pair or arming. */
    g_published_mailbox=(uintptr_t)b; g_toggle=1; int warns=mismatch_logs;
    for (int j=0;j<500;j++) { now+=1000; render_pair(a,2001); tick(); }
    CHECK(g_l3_tick && g_expected_mailbox==(uintptr_t)a && g_render_tid==2001);
    CHECK(mismatch_logs==warns+1);
    g_toggle=0;
    for (int j=0;j<701;j++) { now+=1000; render_pair(a,2001); tick(); }
    CHECK(mismatch_logs==warns+2);
    /* Relatch works with no publish, preserving the live-pair veto and hold. */
    g_published_mailbox=0;
    for (int j=0;j<121;j++) { now+=1000; render_pair(b,2002); render_pair(a,2001); tick(); }
    CHECK(g_render_tid==2001 && !g_hb_relatches);
    int64_t last=now;
    for (int j=0;j<59;j++) { now+=1000; render_pair(b,2002); tick(); }
    now=last+g_qpf/2-1; tick(); CHECK(!g_hb_relatches);
    now++; tick(); CHECK(g_render_tid==2002 && g_expected_mailbox==(uintptr_t)b && g_hb_relatches==1);
    CHECK(!g_l3_tick && !g_previous_packet && !g_render_qpc);
    g_toggle=1;
    for (int j=0;j<119;j++) { now+=1000; render_pair(b,2002); tick(); CHECK(!g_l3_tick); }
    for (int j=0;j<380;j++) { now+=1000; render_pair(b,2002); tick(); }
    CHECK(g_l3_tick);
    /* Drain competing full runs together: first-slot 24/s loses to stable 120/s.
       A much faster short burst cannot qualify, even with a higher raw count. */
    take_reset();
    for (int j=0;j<=120;j++) {
        now+=1000;
        if (j%5==0) render_pair(b,2002);
        render_pair(a,2001);
        if (j>=100) for (int k=0;k<10;k++) { now++; render_pair(burst,2003); }
    }
    tick(); CHECK(g_render_tid==2001 && g_expected_mailbox==(uintptr_t)a);
    take_reset();
    for (int j=0;j<130;j++) {
        now+=1000; render_pair(a,2001);
        if (j>=100) for (int k=0;k<10;k++) { now++; render_pair(burst,2003); }
        tick();
    }
    CHECK(g_render_tid==2001 && g_expected_mailbox==(uintptr_t)a);
    /* A null packet, null mailbox, or zero TID never supplies a candidate. */
    take_reset();
    for (int j=0;j<150;j++) {
        now+=1000;
        render_leaf(a,2001,0,(uint32_t)j);
        render_pair(NULL,2002); render_pair(a,0); tick();
    }
    CHECK(!g_render_tid && !g_expected_mailbox && !g_render_seq);
}
static void take_rate_tests(void)
{
    void *a=(void *)0x111111, *b=(void *)0x222222;
    /* A wrong pair first qualifies, then keeps beating at 5/s forever.
       The genuine 95/s pair must recover without waiting for silence. */
    take_reset();
    for (int j=0;j<96;j++) { now+=1264; render_pair(a,2001); tick(); }
    CHECK(g_expected_mailbox==(uintptr_t)a && g_render_tid==2001);
    for (int j=0;j<200;j++) {
        now+=1264;
        if (j%19==0) render_pair(a,2001);
        render_pair(b,2002); tick();
        if (j<95) CHECK(g_render_tid==2001 && !g_hb_relatches);
    }
    CHECK(g_expected_mailbox==(uintptr_t)b && g_render_tid==2002 && g_hb_relatches==1);
    CHECK(!g_l3_tick);
    /* Two healthy 95/s pairs may coexist; a qualified competitor cannot flap. */
    take_reset();
    for (int j=0;j<96;j++) { now+=1264; render_pair(a,2001); tick(); }
    for (int j=0;j<300;j++) {
        now+=1264; render_pair(b,2002); render_pair(a,2001); tick();
        CHECK(g_expected_mailbox==(uintptr_t)a && g_render_tid==2001 && !g_hb_relatches);
    }
    /* Real 8-16 ms jitter remains healthy even alongside another full run. */
    take_reset();
    for (int j=0;j<100;j++) { now+=960+(j%9)*120; render_pair(a,2001); tick(); }
    CHECK(g_render_tid==2001);
    for (int j=0;j<300;j++) {
        now+=960+(j%9)*120; render_pair(b,2002); render_pair(a,2001); tick();
        CHECK(g_expected_mailbox==(uintptr_t)a && g_render_tid==2001 && !g_hb_relatches);
    }
    /* Raw repeats never qualify, nor can they extend an earlier short burst. */
    take_reset();
    for (int j=0;j<150;j++) { now+=1000; render_leaf(a,2001,0x765432,7); tick(); }
    CHECK(!g_render_tid && !g_expected_mailbox);
    for (int j=0;j<80;j++) { now+=1000; render_pair(a,2001); tick(); }
    uint32_t frame=(uint32_t)(g_hb_write-1);
    for (int j=0;j<150;j++) { now+=1000; render_leaf(a,2001,0x765432,frame); tick(); }
    CHECK(!g_render_tid && !g_expected_mailbox);
    /* Only five progressing records per second cannot use repeats as padding. */
    take_reset();
    for (int j=0;j<200;j++) {
        now+=1264; render_leaf(a,2001,0x765432,(uint32_t)(j/19)); tick();
    }
    CHECK(!g_render_tid && !g_expected_mailbox);
    /* Alternating pooled packets under a fixed frame are still repeats. */
    take_reset();
    for (int j=0;j<96;j++) { now+=1264; render_leaf(a,2001,0x765432+(j%2),7); tick(); }
    CHECK(!g_render_tid && !g_expected_mailbox);
    /* Packet changes supply progress only when both frame numbers are zero. */
    take_reset();
    for (int j=0;j<96;j++) { now+=1264; render_leaf(a,2001,0x765432+(j%2),0); tick(); }
    CHECK(g_expected_mailbox==(uintptr_t)a && g_render_tid==2001);
    /* A reused packet with a changing frame qualifies independently. */
    take_reset();
    for (int j=0;j<96;j++) { now+=1264; render_pair(a,2001); tick(); }
    CHECK(g_expected_mailbox==(uintptr_t)a && g_render_tid==2001);
    /* Repeats from a latched pair cannot preserve its old healthy rate. */
    frame=(uint32_t)(g_hb_write-1);
    for (int j=0;j<200;j++) {
        now+=1264; render_leaf(a,2001,0x765432+(j%2),frame); render_pair(b,2002); tick();
    }
    CHECK(g_expected_mailbox==(uintptr_t)b && g_render_tid==2002 && g_hb_relatches==1);
}
static void take_hitch_tests(void)
{
    void *a=(void *)0x111111, *b=(void *)0x222222;
    for (int hitches=1;hitches<=2;hitches++) {
        take_reset();
        for (int j=0;j<96;j++) { now+=1264; render_pair(a,2001); tick(); }
        CHECK(g_expected_mailbox==(uintptr_t)a && g_render_tid==2001);
        /* The second steady 95/s pair has already run for two seconds. */
        for (int j=0;j<191;j++) {
            now+=1264; render_pair(b,2002); render_pair(a,2001); tick();
        }
        CHECK(g_render_tid==2001 && !g_hb_relatches);
        int logs=relatch_logs, observed_hitches=0;
        int64_t start=now, last_a=now;
        int64_t next_a=start+18000, next_b=start+1264, next_tick=start+1000;
        /* Merge exact event times: 150 ms gaps, with hitch starts 300 ms
           apart, while B stays steady and H1 continues at 120/s. */
        while (next_tick<=start+g_qpf) {
            now=next_a<next_b ? next_a : next_b;
            if (next_tick<now) now=next_tick;
            if (now==next_a) {
                if (now==start+18000 || (hitches==2 && now==start+54000)) {
                    CHECK(now-last_a==150*g_qpf/1000); observed_hitches++;
                }
                render_pair(a,2001); last_a=now; next_a+=1264;
                if (hitches==2 && now<start+36000 && next_a>start+36000)
                    next_a=start+36000;
                else if (hitches==2 && now==start+36000)
                    next_a=start+54000;
            }
            if (now==next_b) { render_pair(b,2002); next_b+=1264; }
            if (now==next_tick) {
                tick(); next_tick+=1000;
                CHECK(g_expected_mailbox==(uintptr_t)a && g_render_tid==2001 && !g_hb_relatches);
            }
        }
        CHECK(observed_hitches==hitches && relatch_logs==logs);
    }
}
static HANDLE publish_go[4];
static DWORD WINAPI publish_worker(void *ctx)
{
    int tls=(int)(uintptr_t)ctx;
    WaitForSingleObject(publish_go[tls],INFINITE);
    gate_tls_set(tls);
    publish_detour(mailbox,NULL,NULL,NULL);
    return 0;
}
static void publish_diagnostic_tests(void)
{
    publish_diagnostics before, after;
    lockstep_publish_snapshot(&before);
    uintptr_t selected=g_expected_mailbox;
    for (int tls=0;tls<=2;tls++) {
        gate_tls_set(tls); CHECK(publish_detour(mailbox,NULL,NULL,NULL)==17);
    }
    gate_tls_set(0); lockstep_publish_snapshot(&after);
    CHECK(after.calls==before.calls+3 && after.owned==before.owned+1);
    CHECK(after.tid[0]==GetCurrentThreadId() && after.tls[0]==2 && after.mailbox==(uintptr_t)mailbox);
    CHECK(g_expected_mailbox==selected && g_published_mailbox==(uintptr_t)mailbox);
    before=after;
    void *owned_mailbox=mailbox;
    HANDLE threads[4]; DWORD tids[4];
    for (int j=0;j<4;j++) {
        publish_go[j]=CreateEventW(NULL,FALSE,FALSE,NULL); CHECK(publish_go[j]!=NULL);
        threads[j]=CreateThread(NULL,0,publish_worker,(void *)(uintptr_t)j,0,&tids[j]);
        CHECK(threads[j]!=NULL);
        if (!threads[j]) exit(2);
    }
    /* Keep harness assertion counters single-threaded; all four native TIDs
       exist together, but their calls are released and joined individually. */
    for (int j=0;j<4;j++) {
        mailbox=(void *)(uintptr_t)(0x888880+j);
        SetEvent(publish_go[j]);
        CHECK(WaitForSingleObject(threads[j],5000)==WAIT_OBJECT_0);
    }
    lockstep_publish_snapshot(&after);
    CHECK(after.calls==before.calls+4 && after.owned==before.owned);
    CHECK(after.mailbox==(uintptr_t)mailbox && g_published_mailbox==(uintptr_t)owned_mailbox);
    CHECK(g_expected_mailbox==selected);
    for (int j=1;j<4;j++) {
        int found=0;
        for (int k=0;k<4;k++) if (after.tid[j]==tids[k] && after.tls[j]==k) found=1;
        CHECK(found);
        for (int k=0;k<j;k++) CHECK(after.tid[j]!=after.tid[k]);
    }
    for (int j=0;j<4;j++) { CloseHandle(threads[j]); CloseHandle(publish_go[j]); }
    mailbox=owned_mailbox;
}
static void xell_latch_tests(void)
{
    DWORD other_tid;
    xell_go=CreateEventW(NULL,FALSE,FALSE,NULL);
    xell_done=CreateEventW(NULL,FALSE,FALSE,NULL);
    HANDLE thread=CreateThread(NULL,0,xell_worker,NULL,0,&other_tid);
    CHECK(xell_go && xell_done && thread);
    if (!xell_go || !xell_done || !thread) exit(2);
    emit=publish_enabled=0;
    for (int source=0;source<=2;source++) {
        g_heartbeat_source=source; g_toggle=0; now+=3*g_qpf;
        g_render_tid=0; g_expected_mailbox=g_published_mailbox=0;
        g_render_qpc=g_render_conflict=g_render_seq=g_hb_relatches=0;
        hooks_init_state();
        int logs=relatch_logs;
        /* Matching traffic must never latch the take-mode negative control. */
        if (!source) {
            for (int j=0;j<130;j++) xell_tick(0,1000);
            CHECK(!g_render_tid && !g_render_seq && !g_expected_mailbox);
            CHECK(!g_l3_tick && relatch_logs==logs);
            continue;
        }
        /* Return address and in-gate suppression apply before qualification. */
        for (int j=0;j<130;j++) {
            now+=993; xell_detour((void *)1,(void *)2,(void *)3,(void *)4); tick();
        }
        gate_tls_set(1);
        for (int j=0;j<130;j++) { now+=993; call_xell(); }
        gate_tls_set(0); tick(); CHECK(!g_render_tid);
        /* A take cannot supply the latch/history in a xell mode. */
        g_published_mailbox=(uintptr_t)mailbox;
        for (int j=0;j<130;j++) { now+=1000; render_take(); tick(); }
        CHECK(!g_render_tid && !g_render_seq); g_published_mailbox=0;
        /* Sub-20 Hz, two independently slow TIDs, and interrupted runs fail. */
        for (int j=0;j<25;j++) xell_tick(0,6500);
        CHECK(!g_render_tid);
        now+=g_qpf/10;
        for (int j=0;j<40;j++) xell_tick(j%2,4000);
        CHECK(!g_render_tid);
        now+=g_qpf/10;
        for (int j=0;j<60;j++) xell_tick(0,1000);
        now+=g_qpf/10;
        for (int j=0;j<60;j++) xell_tick(0,1000);
        CHECK(!g_render_tid);
        /* A full run drained too late is stale, and overflow cannot pool
           pre-overflow history with a short retained burst. */
        now+=g_qpf/10;
        for (int j=0;j<121;j++) { now+=993; xell_call(0); }
        now+=g_qpf/10; tick(); CHECK(!g_render_tid);
        now+=g_qpf/10;
        for (int j=0;j<HB_COUNT+1;j++) xell_call(0);
        tick(); CHECK(!g_render_tid);
        /* Exactly 20/s must complete a full second (21 observations). */
        now+=g_qpf/10;
        for (int j=0;j<20;j++) xell_tick(0,6000);
        CHECK(!g_render_tid);
        xell_tick(0,6000);
        CHECK((DWORD)g_render_tid==GetCurrentThreadId() && !g_expected_mailbox);
        CHECK(xell_latch_logs[source]==1 && !g_hb_relatches && !g_l3_tick);
        LONG64 seq=g_render_seq; int64_t start=now;
        xell_call(0);
        CHECK(g_render_seq==seq+1 && g_render_qpc==start+(source==2 ? 7 : 0));
        publish_enabled=1; tick(); publish_enabled=0;
        CHECK(g_published_mailbox==(uintptr_t)mailbox && !g_expected_mailbox);
        g_toggle=1;
        /* The earlier 20 Hz H1 window must expire before a complete native
           120 Hz baseline can pass; xell qualification never bypasses it. */
        for (int j=0;j<500;j++) xell_tick(0,1000);
        CHECK(g_l3_tick && !g_expected_mailbox);
        /* A qualified competitor cannot steal a live TID. */
        g_toggle=0;
        for (int j=0;j<130;j++) { xell_tick(1,1000); xell_call(0); }
        tick(); CHECK((DWORD)g_render_tid==GetCurrentThreadId() && !g_hb_relatches);
        /* Same take-relatch boundary: 500 ms of old-TID silence. */
        int64_t silence_start=g_render_qpc;
        for (int j=0;j<59;j++) xell_tick(1,1000);
        CHECK((DWORD)g_render_tid==GetCurrentThreadId() && !g_hb_relatches);
        now=silence_start+g_qpf/2-1; tick(); CHECK(!g_hb_relatches);
        now++; tick();
        CHECK((DWORD)g_render_tid==other_tid && g_hb_relatches==1 && !g_l3_tick);
        CHECK(relatch_logs==logs+1 && !g_expected_mailbox);
        g_toggle=1;
        for (int j=0;j<119;j++) { xell_tick(1,1000); CHECK(!g_l3_tick); }
        for (int j=0;j<130;j++) xell_tick(1,1000);
        CHECK(g_l3_tick); /* median/sequence and native baseline rebuilt */
        now+=g_qpf/10; tick(); CHECK(!g_l3_tick); /* freshness remains required */
        g_toggle=0; now+=g_qpf/10;
        /* A healthy old-TID run at the end of a batch vetoes replacement. */
        for (int j=0;j<121;j++) {
            now+=993; xell_call(0);
            if (j>=100) xell_call(1);
        }
        tick(); CHECK(g_hb_relatches==1);
        /* An earlier fast run cannot hide subsequent sub-20 Hz decay. */
        for (int j=0;j<30;j++) { now+=6493; xell_call(0); xell_call(1); tick(); }
        for (int j=0;j<12;j++) xell_tick(0,6500);
        CHECK(g_hb_relatches==1 && (DWORD)g_render_tid==other_tid);
    }
    xell_stop=1; SetEvent(xell_go);
    CHECK(WaitForSingleObject(thread,5000)==WAIT_OBJECT_0);
    CloseHandle(thread); CloseHandle(xell_go); CloseHandle(xell_done);
}
static void WINAPI sleep_original(DWORD ms) { CHECK(ms==1); now+=60; SetLastError(456); }
/* The Sleep stub resumes at the original six-byte call's end, with the
   original body RSP. Test both ZF branches after a native sleep substitute. */
static volatile int sleep_zf, sleep_calls;
static volatile DWORD sleep_ms;
static volatile uintptr_t sleep_entry;
void run_sleep_stub(void);
void WINAPI sleep_stub_native(DWORD ms);
__asm__(".globl run_sleep_stub\nrun_sleep_stub:\nsubq $40,%rsp\nmovl $1,%ecx\ncmpl $0,sleep_zf(%rip)\njmp *sleep_entry(%rip)\n"
        ".globl sleep_stub_native\nsleep_stub_native:\nmovl %ecx,sleep_ms(%rip)\nincl sleep_calls(%rip)\n"
        "cmpl $0,sleep_zf(%rip)\nret\n");
static CONTEXT sleep_unwind(uintptr_t ip,uintptr_t rsp)
{
    CONTEXT c={0}; DWORD64 ib=0,est=0; void *hd=NULL;
    c.ContextFlags=CONTEXT_FULL; c.Rip=ip; c.Rsp=rsp;
    c.Rbx=1; c.Rbp=2; c.Rdi=3; c.Rsi=4; c.R12=5; c.R13=6; c.R14=7; c.R15=8;
    for (int j=0;j<16;j++) { (&c.Xmm0)[j].Low=100+j; (&c.Xmm0)[j].High=200+j; }
    PRUNTIME_FUNCTION fe=RtlLookupFunctionEntry(ip,&ib,NULL); CHECK(fe!=NULL);
    if (fe) RtlVirtualUnwind(UNW_FLAG_NHANDLER,ib,ip,fe,&c,&hd,&est,NULL);
    return c;
}
static void sleep_stub_tests(const char *path)
{
    FILE *f=fopen(path,"rb"); CHECK(f!=NULL); if (!f) return;
    fseek(f,0,SEEK_END); size_t len=(size_t)ftell(f); rewind(f);
    uint8_t *file=malloc(len); CHECK(file!=NULL); if (!file) { fclose(f); return; }
    CHECK(fread(file,1,len,f)==len); fclose(f);
    IMAGE_NT_HEADERS64 *nt=(void *)(file+((IMAGE_DOS_HEADER *)file)->e_lfanew);
    uint8_t *img=calloc(1,nt->OptionalHeader.SizeOfImage); CHECK(img!=NULL);
    if (!img) { free(file); return; }
    memcpy(img,file,nt->OptionalHeader.SizeOfHeaders);
    IMAGE_SECTION_HEADER *sections=IMAGE_FIRST_SECTION(nt);
    for (int j=0;j<nt->FileHeader.NumberOfSections;j++)
        memcpy(img+sections[j].VirtualAddress,file+sections[j].PointerToRawData,sections[j].SizeOfRawData);
    IMAGE_DATA_DIRECTORY pd=nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
    RUNTIME_FUNCTION *hosts=(void *)(img+pd.VirtualAddress), *host=NULL;
    /* Historical RVA is a read-only fixture hint, as in test_heartbeat. */
    const DWORD site=0x1a74954;
    CHECK(img[site]==0xff && img[site+1]==0x15);
    for (size_t j=0;j<pd.Size/sizeof *hosts;j++)
        if (hosts[j].BeginAddress<=site && hosts[j].EndAddress>site) host=&hosts[j];
    CHECK(host!=NULL); if (!host) { free(img); free(file); return; }
    uint8_t *a=VirtualAlloc(NULL,0x10000,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);
    CHECK(a!=NULL); if (!a) { free(img); free(file); return; }
    RUNTIME_FUNCTION *tbl; char err[128]; DWORD old;
    CHECK(!sleep1_build(a,(uintptr_t)(a+0x800),img+host->UnwindData,&tbl,err,sizeof err));
    uint8_t expected[26]={0xff,0x15,12,0,0,0,0x0f,0x84,0,0,0,0,0x0f,0x85};
    int32_t disp=0x800+6-12; memcpy(expected+8,&disp,4);
    disp=0x800+6-18; memcpy(expected+14,&disp,4);
    uintptr_t target=(uintptr_t)sleep1_detour; memcpy(expected+18,&target,8);
    CHECK(!memcmp(a,expected,sizeof expected));
    CHECK(tbl[0].BeginAddress==0 && tbl[0].EndAddress==18 && tbl[0].UnwindData==0x100);
    size_t host_len=host->EndAddress-host->BeginAddress; CHECK(host_len<0x8000);
    memcpy(a+0x1000,img+host->BeginAddress,host_len);
    size_t ui_len=stubs_unwind_len(img+host->UnwindData);
    memcpy(a+0xf000,img+host->UnwindData,ui_len);
    memcpy(a+0x500,a,18); memcpy(a+0xf100,img+host->UnwindData,ui_len);
    tbl[1]=(RUNTIME_FUNCTION){0x500,0x512,0xf100};
    tbl[2]=(RUNTIME_FUNCTION){0x1000,0x1000+(DWORD)host_len,0xf000};
    CHECK(VirtualProtect(a,0x10000,PAGE_EXECUTE_READ,&old));
    CHECK(RtlAddFunctionTable(tbl,3,(DWORD64)(uintptr_t)a));
    __attribute__((aligned(16))) uint64_t stack[512];
    for (int j=0;j<512;j++) stack[j]=0xdead000000000000ull+j;
    uintptr_t body=(uintptr_t)(stack+128);
    CONTEXT ref=sleep_unwind((uintptr_t)(a+0x1000+site-host->BeginAddress),body);
    CHECK(ref.Rip==*(uint64_t *)(body+0x1a8) && ref.Rsp==body+0x1b0);
    unsigned negative_bad=0;
    /* All three instruction offsets, including +6, the call return PC. */
    const unsigned offsets[]={0,6,12};
    for (unsigned j=0;j<3;j++) {
        CONTEXT got=sleep_unwind((uintptr_t)(a+offsets[j]),body);
        CHECK(!memcmp(&got,&ref,sizeof got));
        CONTEXT bad=sleep_unwind((uintptr_t)(a+0x500+offsets[j]),body);
        negative_bad+=memcmp(&bad,&ref,sizeof bad)!=0;
    }
    CHECK(negative_bad>0);
    CHECK(RtlDeleteFunctionTable(tbl));
    CHECK(VirtualProtect(a,0x10000,PAGE_READWRITE,&old));
    /* Enter by jump with the native body-RSP alignment. The call end at +6
       increments a sentinel, releases the harness shadow space, then returns. */
    memcpy(a+0x806,"\xFF\x05\0\0\0\0\x48\x83\xC4\x28\xC3",11);
    disp=0x9000-0x80c; memcpy(a+0x808,&disp,4);
    CHECK(VirtualProtect(a,0x1000,PAGE_EXECUTE_READ,&old));
    CHECK(VirtualProtect(a+0x9000,0x1000,PAGE_READWRITE,&old));
    CHECK(FlushInstructionCache(GetCurrentProcess(),a,0x1000));
    sleep_entry=(uintptr_t)a; g_native_sleep=sleep_stub_native;
    *(int *)(a+0x9000)=0;
    LONG64 polls=g_sleep_polls; int before=sleep_calls;
    run_sleep_stub();
    CHECK(sleep_calls==before+1 && sleep_ms==1 && g_sleep_polls==polls+1);
    CHECK(*(int *)(a+0x9000)==1);
    /* Also resume directly at the stub call's return PC with each ZF value. */
    sleep_entry=(uintptr_t)(a+6);
    for (sleep_zf=0;sleep_zf<2;sleep_zf++) {
        run_sleep_stub();
        CHECK(sleep_calls==before+1 && g_sleep_polls==polls+1);
        CHECK(*(int *)(a+0x9000)==2+sleep_zf);
    }
    CHECK(VirtualFree(a,0,MEM_RELEASE)); free(img); free(file);
}
int main(int argc,char **argv)
{
    if (argc<2) return 2;
    static const uint8_t ui[8]={1,0,0,0,0,0,0,0}; char why[128]; DWORD old;
    uint8_t *alloc=VirtualAlloc(NULL,2*STUB_PAGE,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);
    stubs_params sp={alloc,0,0,ui,ui};
    CHECK(!stubs_build(&sp,&g_stubs,why,sizeof why));
    VirtualProtect(alloc,STUB_PAGE,PAGE_EXECUTE_READ,&old);
    g_gate_tls=TlsAlloc(); CHECK(g_gate_tls<64);
    g_qpc_fn=(void *)render_qpc;
    g_cfg.lockstep=1; g_cfg.fc=(factor_cfg){TM_RAW,8,.2,.5,2,0};
    g_resolved.fps=8; g_resolved.actual_fps=12; g_resolved.req=16; g_resolved.eff=20;
    g_resolved.pause_a=24; g_resolved.pause_b=25;
    g_resolved.online=1; g_resolved.online_global=(uintptr_t)&online_ptr; g_resolved.online_offset=4;
    g_resolved.s_fps=0; g_resolved.s_req=4; g_resolved.s_eff=8;
    void *scene_obj=(void *)1; g_resolved.scene_global=(uintptr_t)&scene_obj;
    g_resolved.hook[A_SCENE]=(uintptr_t)scene; g_resolved.help_jobs=(uintptr_t)help;
    *(int32_t *)(clk+8)=*(int32_t *)(clk+12)=*(int32_t *)S=120;
    *(float *)(clk+16)=*(float *)(clk+20)=*(float *)(S+4)=1;
    void *cp=clk; g_clock_global=(uintptr_t)&cp;
    h1_tramp=(void *)clock_original; h2_tramp=(void *)update_original;
    publish_tramp=(void *)publish_original; frame_tramp=(void *)frame_original;
    g_l2_ready=1; hooks_init_state();
    g_published_mailbox=(uintptr_t)mailbox;
    render_leaf(mailbox,render_tid,0,7);
    emit=0; tick(); emit=1;
    CHECK(!g_render_tid && !g_render_seq && !g_take_seq); /* null cannot latch */
    CHECK(g_hb_ring[0].flags==4 && g_hb_ring[0].frame_no==7);
    for (int j=0;j<370;j++) { tick(); now+=1000; }
    CHECK(g_l3_tick && g_expected_mailbox==(uintptr_t)mailbox && fabs(seen_r1-1)<1e-6);
    /* Exercise the real H1 guarded read, edge logs and native P1/P2 paths. */
    online_ptr=0; now+=1000; tick(); CHECK(g_l3_tick && !online_disarm_logs);
    online_ptr=(uintptr_t)online_obj;
    for (int flag=1;flag<=2;flag++) {
        online_obj[1]=flag; now+=1000; tick();
        CHECK(!g_l3_tick && seen_r1==1 && seen_r2==1 && online_disarm_logs==flag);
        for (int j=0;j<300;j++) { now+=1000; tick(); CHECK(!g_l3_tick && seen_r1==1 && seen_r2==1); }
        CHECK(online_disarm_logs==flag);
        online_obj[1]=0; now+=1000; tick(); CHECK(!g_l3_tick && online_clear_logs==flag);
        for (int j=0;j<239;j++) { now+=1000; tick(); CHECK(!g_l3_tick); }
        now+=1000; tick(); CHECK(g_l3_tick && online_clear_logs==flag);
    }
    /* Even a stale readiness bit cannot bypass missing online validation. */
    g_resolved.online=0; now+=1000; tick(); CHECK(!g_l3_tick && seen_r1==1 && seen_r2==1);
    g_resolved.online=1;
    for (int j=0;j<241;j++) { now+=1000; tick(); }
    CHECK(g_l3_tick);
    now+=1000;
    /* A renderer heartbeat arrives while the pending raw record is sampled.
       Reading health against an earlier now would incorrectly disarm this tick. */
    uintptr_t page=(uintptr_t)&g_hb_ring[(g_hb_write-1)%HB_COUNT]&~(uintptr_t)0xfff;
    void *vh=AddVectoredExceptionHandler(1,hb_guard); guard_pump=1;
    CHECK(VirtualProtect((void *)page,4096,PAGE_READWRITE|PAGE_GUARD,&old));
    tick();
    CHECK(guard_hits==1 && g_l3_tick && fabs(seen_r1-1.001)<1e-6);
    CHECK(VirtualProtect((void *)page,4096,old,&old)); RemoveVectoredExceptionHandler(vh);
    now+=1000;
    emit=0; tick(); now+=1000;
    arrival=now+400; tick(); /* no banked heartbeat: native help must advance it */
    CHECK(fabs(seen_r1-1.4)<1e-6 && fabs(seen_r2-1.4)<1e-6);
    int got_wait=0;
    for (unsigned j=0;j<nrec;j++) if (recs[j].type==RT_GATE_END && fabs(recs[j].dt-400.0/120000)<1e-12) got_wait++;
    CHECK(got_wait==1);
    float r2_before=*g_stubs.r2;
    *g_stubs.ovl_h2=1; overlap_check=1;
    CHECK(h2_detour(NULL,S,(void *)3,(void *)4)==0x88776655);
    overlap_check=0; *g_stubs.ovl_h2=0;
    CHECK(g_ovl_h2_total==1 && *g_stubs.r2==r2_before && gate_tls_get()==0);
    LONG64 h1_before=g_frame;
    CHECK(frame_detour(NULL,worker,NULL,NULL)==23); /* vtable path without H1 */
    CHECK(g_frame==h1_before && seen_r2==1 && recs[nrec-2].type==RT_BYPASS);
    now+=1000; tick(); CHECK(seen_r1==1 && seen_r2==1 && !g_l3_tick);
    g_toggle=0; now+=1000; tick(); CHECK(seen_r1==1 && seen_r2==1 && !g_l3_tick);
    g_toggle=1; now+=1000; emit=1; tick(); CHECK(seen_r1==1 && seen_r2==1); /* bootstrap/reset */
    clk[24]=1; now+=1000; tick(); CHECK(seen_r1==1 && seen_r2==1 && !g_l3_tick);
    clk[24]=0; *(int32_t *)(clk+8)=60; now+=1000; tick(); CHECK(seen_r1==1 && seen_r2==1 && !g_l3_tick);
    /* A recreated mailbox/render worker must recover without restarting the mod. */
    *(int32_t *)(clk+8)=120;
    void *old_mailbox=mailbox; mailbox=(void *)0x234567; render_tid=1001;
    for (int j=0;j<120;j++) { now+=1000; tick(); }
    CHECK(g_expected_mailbox==(uintptr_t)old_mailbox && g_render_tid==999);
    for (int j=0;j<3;j++) { now+=1000; tick(); }
    CHECK(g_expected_mailbox==(uintptr_t)mailbox && g_render_tid==1001 && !g_l3_tick);
    CHECK(g_hb_relatches==1 && relatch_logs==1);
    for (int j=0;j<119;j++) { now+=1000; tick(); CHECK(!g_l3_tick); }
    for (int j=0;j<130;j++) { now+=1000; tick(); }
    CHECK(g_l3_tick);
    /* Another render TID may qualify while the current pair stays live. */
    for (int j=0;j<180;j++) {
        now+=1000; render_pair(mailbox,1002); tick();
    }
    CHECK(g_render_tid==1001 && g_hb_relatches==1);
    /* Its one-second run is already proven, but the old pair must be silent
       for the full 500 ms. Recovery with just a TID change is supported. */
    emit=0; render_tid=1002; g_toggle=0; /* isolate wall-clock boundary from gate waits */
    for (int j=0;j<59;j++) { now+=1000; render_take(); tick(); }
    CHECK(g_render_tid==1001 && g_hb_relatches==1);
    now+=1000; render_take(); tick();
    CHECK(g_render_tid==1002 && g_hb_relatches==2 && relatch_logs==2 && !g_l3_tick);
    g_toggle=1;
    /* Sub-20 Hz traffic cannot qualify, even after a second. */
    old_mailbox=mailbox; mailbox=(void *)0x345678; render_tid=1003;
    now+=1000; tick(); /* advisory publish of the replacement */
    for (int j=0;j<25;j++) { now+=6500; render_take(); tick(); }
    CHECK(g_expected_mailbox==(uintptr_t)old_mailbox && g_hb_relatches==2);
    /* A gap resets the candidate. High-rate fragments don't combine across it. */
    now+=12000;
    for (int j=0;j<60;j++) { now+=1000; render_take(); tick(); }
    now+=12000;
    for (int j=0;j<60;j++) { now+=1000; render_take(); tick(); }
    CHECK(g_expected_mailbox==(uintptr_t)old_mailbox && g_hb_relatches==2);
    /* A full healthy run recovers even though the clock baseline here is slow. */
    for (int j=0;j<65;j++) { now+=1000; render_take(); tick(); }
    CHECK(g_expected_mailbox==(uintptr_t)mailbox && g_render_tid==1003 && g_hb_relatches==3);
    CHECK(relatch_logs==3 && !g_l3_tick);
    /* A short unowned burst cannot steal the latch. Publishing is advisory. */
    for (int j=0;j<60;j++) { now+=1000; render_pair((void *)0x456789,1004); tick(); }
    CHECK(g_expected_mailbox==(uintptr_t)mailbox && g_render_tid==1003 && g_hb_relatches==3);
    /* Even a full qualified run cannot steal from a healthy selected pair. */
    for (int j=0;j<125;j++) {
        now+=1000; render_pair((void *)0x456789,1004); render_take(); tick();
        CHECK(g_expected_mailbox==(uintptr_t)mailbox && g_render_tid==1003 && g_hb_relatches==3);
    }
    /* Each pair must independently meet the rate floor, not their sum. */
    mailbox=(void *)0x56789a; now+=1000; tick();
    for (int j=0;j<40;j++) { now+=4000; render_pair(mailbox,1004+(j%2)); tick(); }
    CHECK(g_expected_mailbox!=(uintptr_t)mailbox && g_hb_relatches==3);
    /* Stale raw records must not replace a pair just because they were drained
       late; likewise, a healthy old run at the batch end vetoes replacement. */
    now+=12000;
    for (int j=0;j<121;j++) { now+=1000; render_pair(mailbox,1006); }
    now+=12000; tick();
    CHECK(g_hb_relatches==3);
    now+=12000;
    for (int j=0;j<121;j++) {
        now+=1000; render_pair(mailbox,1006);
        if (j>=100) render_pair((void *)0x345678,1003);
    }
    tick();
    CHECK(g_hb_relatches==3);
    /* An early fast run must not mask a candidate's subsequent slow rate. */
    for (int j=0;j<30;j++) {
        now+=6500; render_pair(mailbox,1006); render_pair((void *)0x345678,1003); tick();
    }
    for (int j=0;j<12;j++) { now+=6500; render_pair(mailbox,1006); tick(); }
    CHECK(g_hb_relatches==3);
    /* Exactly 20 Hz qualifies after a full second, even with unchanged TID. */
    mailbox=(void *)0x6789ab; render_tid=1003; now+=12000; tick();
    for (int j=0;j<20;j++) { now+=6000; render_take(); tick(); }
    CHECK(g_hb_relatches==3);
    now+=6000; render_take(); tick();
    CHECK(g_expected_mailbox==(uintptr_t)mailbox && g_render_tid==1003 && g_hb_relatches==4);
    /* Real C xell detour: return, entry/exit clocks, filter and all sources. */
    xell_tramp=(void *)xell_original; g_xell_return=(uintptr_t)xell_test_return;
    g_render_tid=(LONG)GetCurrentThreadId();
    for (int source=0;source<3;source++) {
        g_heartbeat_source=source;
        uint64_t count=nrec; LONG64 seq=g_render_seq; int64_t start=now;
        SetLastError(123);
        CHECK(call_xell()==0x1122334455667788ull && GetLastError()==321);
        CHECK(nrec==count+2 && recs[count%4096].type==RT_XELL && recs[(count+1)%4096].type==RT_XELL_EXIT);
        CHECK(recs[count%4096].qpc==start && recs[(count+1)%4096].qpc==start+7);
        CHECK(g_render_seq==seq+(source!=0));
        if (source) CHECK(g_render_qpc==start+(source==2 ? 7 : 0));
    }
    uint64_t count=nrec; LONG64 seq=g_render_seq;
    CHECK(xell_detour((void *)1,(void *)2,(void *)3,(void *)4)==0x1122334455667788ull);
    CHECK(nrec==count && g_render_seq==seq); /* unrelated caller */
    gate_tls_set(1); CHECK(call_xell()==0x1122334455667788ull); gate_tls_set(0);
    CHECK(nrec==count && g_render_seq==seq); /* recursive caller */
    g_native_sleep=sleep_original; sleep1_detour(1);
    CHECK(g_sleep_polls==1 && g_sleep_qpc==60 && GetLastError()==456);
    sleep_stub_tests(argv[1]);
    take_latch_tests();
    take_rate_tests();
    take_hitch_tests();
    publish_diagnostic_tests();
    xell_latch_tests();
    printf("%d checks, %d failures\nTEST_LOCKSTEP: %s\n",checks,fail,fail ? "FAIL" : "PASS"); return fail ? 1 : 0;
}
