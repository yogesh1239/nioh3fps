#include "../src/gate.h"
#include "../src/factor.h"
#include <stdio.h>
#include <string.h>
#include <math.h>
static int checks,fail;
#define CHECK(x) do { checks++; if (!(x)) { fail++; printf("FAIL %d: %s\n",__LINE__,#x); } } while(0)
typedef struct { int64_t now,advance,arrival; uint64_t seq,burst; unsigned helps,begin,end; int64_t waited; int timeout; } fake;
static int64_t clock_fn(void *p) { return ((fake *)p)->now; }
static uint64_t seq_fn(void *p) { return ((fake *)p)->seq; }
static void help_fn(void *p) { fake *f=p; f->helps++; f->now+=f->advance; if (f->arrival && f->now>=f->arrival) { f->seq+=f->burst; f->arrival=0; } }
static void event_fn(void *p,int begin,uint64_t seq,int64_t now,int64_t waited,int timeout)
{ fake *f=p; (void)seq; (void)now; if (begin) f->begin++; else { f->end++; f->waited=waited; f->timeout=timeout; } }
static void warm(gate_state *s,gate_input *i,gate_io *io,fake *f)
{
    gate_result o; gate_init(s); memset(f,0,sizeof *f); f->advance=100;
    *i=(gate_input){1,1,0,1,7,7,100000,120000,1333,0};
    *io=(gate_io){f,clock_fn,seq_fn,help_fn,event_fn};
    for (int j=0;j<=240;j++) { i->now=f->now=100000+j*1000; f->seq++; gate_run(s,i,io,&o); }
    CHECK(o.armed && !o.waited && s->baseline_ok); /* first armed tick bootstraps */
}
int main(void)
{
    gate_state s, saved; gate_input i; gate_io io; gate_result o; fake f;
    warm(&s,&i,&io,&f);
    s.armed=0; i.now=f.now+=1000; /* unchanged seq would wait without bootstrap */
    gate_run(&s,&i,&io,&o); CHECK(o.armed && !o.waited && !f.helps);
    i.now=f.now+=1000; f.arrival=f.now+400; f.burst=1;
    gate_run(&s,&i,&io,&o);
    CHECK(o.armed && o.waited && !o.timeout && o.wait_qpc==400 && f.helps==4);
    CHECK(f.begin==1 && f.end==1 && f.waited==400 && !f.timeout);
    f.seq+=10; i.now=f.now+=1000; unsigned h=f.helps;
    gate_run(&s,&i,&io,&o); CHECK(o.armed && !o.waited && s.last_seq==f.seq);
    i.now=f.now+=1000; f.arrival=f.now+300; f.burst=1;
    gate_run(&s,&i,&io,&o); CHECK(o.waited && f.helps==h+3); /* no banked burst */
    saved=s; i.in_gate=1; i.tid=999; i.previous_h2=0;
    gate_run(&s,&i,&io,&o); CHECK(!memcmp(&saved,&s,sizeof s) && !o.waited);
    i.in_gate=0; i.tid=7; i.previous_h2=1;
    i.now=f.now+=1000; i.median_interval=120000; h=f.helps;
    gate_run(&s,&i,&io,&o); CHECK(o.timeout && o.wait_qpc==6000 && f.helps==h+60); /* 50ms cap */
    i.median_interval=1000;
    for (int j=0;j<2;j++) { i.now=f.now+=1000; gate_run(&s,&i,&io,&o); }
    CHECK(o.timeout && o.backoff && o.reset && s.backoffs==1 && !s.armed && s.blocked_until==f.now+240000);
    i.now=f.now+=1000; gate_run(&s,&i,&io,&o); CHECK(!o.armed && !o.waited);
    /* Three non-clustered timeouts do not trigger backoff. */
    warm(&s,&i,&io,&f);
    for (int j=0;j<3;j++) { i.now=f.now+=120001; gate_run(&s,&i,&io,&o); }
    CHECK(s.timeouts==3 && s.backoffs==0 && s.armed);
    /* Five timeout clusters in a minute permanently disarm this session. */
    warm(&s,&i,&io,&f);
    for (int k=0;k<5;k++) {
        if (k) { s.baseline_ok=1; s.armed=1; i.now=f.now=s.blocked_until; }
        for (int j=0;j<3;j++) { i.now=f.now+=1000; gate_run(&s,&i,&io,&o); }
    }
    CHECK(s.backoffs==5 && s.session_off);
    i.now=f.now+=240000; s.baseline_ok=1; gate_run(&s,&i,&io,&o); CHECK(!o.armed);
    warm(&s,&i,&io,&f);
    i.tid=i.paired_tid=8; i.now=f.now+=1000; gate_run(&s,&i,&io,&o);
    CHECK(o.tid_change && !o.armed && o.reset && s.update_tid==8 && s.blocked_until==f.now+120000);
    warm(&s,&i,&io,&f);
    i.previous_h2=0; i.now=f.now+=1000; gate_run(&s,&i,&io,&o); CHECK(!o.armed && o.reset);
    warm(&s,&i,&io,&f); i.conditions=0; gate_run(&s,&i,&io,&o); CHECK(!o.armed);
    /* Factor clock taken after gate: 8.333ms native work + 2.5ms wait. */
    warm(&s,&i,&io,&f);
    factor_state fs; factor_out fo; factor_cfg cfg={TM_RAW,8,.2,.5,2,0}; factor_init(&fs);
    factor_update(&fs,&cfg,f.now,i.qpf,120,1,&fo);
    i.now=f.now+=1000; f.arrival=f.now+300; f.burst=1;
    gate_run(&s,&i,&io,&o);
    float factor=factor_update(&fs,&cfg,clock_fn(&f),i.qpf,120,1,&fo);
    CHECK(o.wait_qpc==300 && fabs(factor-1.3)<1e-6 && fabs(fo.dt-1300.0/120000)<1e-12);
    factor_update(&fs,&cfg,f.now+=10000,i.qpf,120,1,&fo);
    CHECK(fabs(fs.time_lost-10000.0/120000+2.0/120)<1e-12);
    factor_update(&fs,&cfg,f.now+=100,i.qpf,120,1,&fo);
    CHECK(fabs(fs.time_gained-(.5/120-100.0/120000))<1e-10);
    /* A lower native rate must never arm, even with healthy heartbeats/pairs. */
    gate_init(&s); i=(gate_input){1,1,0,1,7,7,100000,120000,1000,0};
    for (int j=0;j<300;j++) { i.now=f.now=100000+j*1500; f.seq++; gate_run(&s,&i,&io,&o); }
    CHECK(!o.armed && !s.baseline_ok && !s.waits);
    /* Pairing validation, rather than a duplicate H2 count, invalidates the
       complete baseline window for both missing and extra H2 calls. */
    for (int bad=0;bad<=2;bad+=2) {
        gate_init(&s); i=(gate_input){1,1,0,1,7,7,100000,120000,1000,0};
        for (int j=0;j<240;j++) { i.now=f.now=100000+j*1000; f.seq++; gate_run(&s,&i,&io,&o); }
        i.previous_h2=bad; i.now=f.now+=1000; gate_run(&s,&i,&io,&o);
        CHECK(!o.armed && !s.baseline_ok && !s.baseline_start);
        i.previous_h2=1;
        for (int j=0;j<240;j++) { i.now=f.now+=1000; f.seq++; gate_run(&s,&i,&io,&o); CHECK(!o.armed); }
        i.now=f.now+=1000; f.seq++; gate_run(&s,&i,&io,&o); CHECK(o.armed);
    }
    /* Every nonzero session flag disarms; the full hold starts on clearing,
       even after a long online session has supplied healthy native ticks. */
    const int32_t flags[]={1,2,3,-1};
    for (unsigned k=0;k<sizeof flags/sizeof *flags;k++) {
        warm(&s,&i,&io,&f); CHECK(!i.online_flag && s.armed);
        i.online_flag=flags[k]; i.now=f.now+=1000; f.seq++;
        gate_run(&s,&i,&io,&o);
        CHECK(!o.armed && !o.waited && o.reset && o.online_edge==1 && !s.baseline_ok);
        for (int j=0;j<300;j++) {
            i.now=f.now+=1000; f.seq++; gate_run(&s,&i,&io,&o);
            CHECK(!o.armed && !o.waited && !o.online_edge && !s.baseline_ok);
        }
        i.online_flag=0; i.now=f.now+=1000; f.seq++; gate_run(&s,&i,&io,&o);
        CHECK(o.online_edge==-1 && !o.armed && s.blocked_until==i.now+2*i.qpf);
        for (int j=0;j<239;j++) {
            i.now=f.now+=1000; f.seq++; gate_run(&s,&i,&io,&o);
            CHECK(!o.armed && !o.waited && !o.online_edge);
        }
        i.now=f.now+=1000; f.seq++; gate_run(&s,&i,&io,&o); CHECK(o.armed && !o.waited);
        /* A second online edge restarts the clear hold. */
        i.online_flag=2; i.now=f.now+=1000; gate_run(&s,&i,&io,&o); CHECK(!o.armed && o.online_edge==1);
    }
    printf("%d checks, %d failures\nTEST_GATE: %s\n",checks,fail,fail ? "FAIL" : "PASS"); return fail ? 1 : 0;
}
