#include "lockstep.h"
#include "resolve.h"
#include <string.h>
int g_l2_ready;
volatile LONG g_l3_tick, g_factor_epoch;
volatile LONG64 g_sleep_polls, g_sleep_qpc;
void *publish_tramp, *frame_tramp, *xell_tramp;
uintptr_t g_xell_return;
void (WINAPI *g_native_sleep)(DWORD);
typedef uint64_t (*fn4)(void *,void *,void *,void *);
static gate_state G;
static volatile LONG h2_count, h2_tid;
static volatile LONG last_h1_tid;
static volatile LONG64 publish_calls, publish_owned, publish_pairs[4];
static volatile uintptr_t publish_mailbox;
static int64_t last_publish_warning;
static DWORD last_update_tid(void) { return (DWORD)__atomic_load_n(&last_h1_tid,__ATOMIC_ACQUIRE); }
static uint64_t hb_read, xell_read;
/* Selected matched xell events have their own stream: the physical take ring
   and its trace/leaf ABI remain unchanged. H1 alone owns latch/history state. */
static hb_rec xell_ring[HB_COUNT] __attribute__((aligned(64)));
static volatile LONG64 xell_write;
static int64_t first_beat, last_beat, intervals[16];
static int64_t last_latched_beat; /* liveness survives median/overflow resets */
static unsigned interval_n, interval_pos;
typedef struct {
    uintptr_t mailbox;
    DWORD tid;
    int64_t first, last;
    uint64_t count;
    uint64_t packet;
    uint32_t frame;
    int64_t recent[21]; /* last 20 intervals, so old bursts cannot mask decay */
} hb_candidate;
static hb_candidate candidates[8], latched_health;
static LONG64 seen_ovl_h1,seen_ovl_h2;
static void median_reset(void)
{
    interval_n=interval_pos=0; first_beat=last_beat=0;
    memset(intervals,0,sizeof intervals);
}
void lockstep_init(void)
{
    gate_init(&G); median_reset(); memset(candidates,0,sizeof candidates);
    memset(&latched_health,0,sizeof latched_health);
    last_latched_beat=0;
    last_publish_warning=0;
    hb_read=(uint64_t)__atomic_load_n(&g_hb_write,__ATOMIC_ACQUIRE);
    xell_read=(uint64_t)__atomic_load_n(&xell_write,__ATOMIC_ACQUIRE);
    h2_count=h2_tid=last_h1_tid=0;
    seen_ovl_h1=InterlockedCompareExchange64(&g_ovl_h1_total,0,0);
    seen_ovl_h2=InterlockedCompareExchange64(&g_ovl_h2_total,0,0);
}
/* Called only by the non-overlapping H1 reader. The leaf records all pairs;
   H1 owns latch decisions, candidate timing, history and gate resets. */
static void candidate_observe(hb_candidate *c,const hb_rec *r,int xell,int reset_on_gap)
{
    /* Repeat flags in the leaf are global to the selected pair. Independently
       track each take pair's frame progress, using packets only when both
       frames are unavailable; xell records have no packet/frame. */
    if (!xell && c->count && ((c->frame || r->frame_no) ?
        c->frame==r->frame_no : c->packet==r->packet)) return;
    /* Candidates require a continuous run. A latched pair keeps its recent
       progress across hitches; only silence or recent-rate failure replaces it. */
    if (!c->count || r->qpc<=c->last || (reset_on_gap && r->qpc-c->last>=g_qpf/10)) {
        c->first=r->qpc; c->count=0;
    }
    c->packet=r->packet; c->frame=r->frame_no;
    c->last=r->qpc; c->recent[c->count++%21]=r->qpc;
    if (c->count>=21 && c->last-c->recent[(c->count-21)%21]>g_qpf)
        c->first=c->last; /* require a fresh one-second run after rate decay */
}
static int candidate_rate_ok(const hb_candidate *c,int64_t latest)
{
    /* The latch uses its latest raw beat, so repeats age its progress window.
       Candidates use their latest progressing beat and must also be fresh. */
    return c->count>=21 && latest>=c->last &&
        latest-c->recent[(c->count-21)%21]<=g_qpf;
}
static void latch_observe(const hb_rec *r)
{
    int xell=g_heartbeat_source!=0;
    uintptr_t latched=__atomic_load_n(&g_expected_mailbox,__ATOMIC_ACQUIRE);
    DWORD tid=(DWORD)__atomic_load_n(&g_render_tid,__ATOMIC_ACQUIRE);
    if (!r->tid || (!xell && (!r->mailbox || !r->packet || (r->flags&4)))) return;
    if ((xell || r->mailbox==latched) && r->tid==tid) {
        candidate_observe(&latched_health,r,xell,0); return;
    }
    hb_candidate *c=NULL, *oldest=&candidates[0];
    for (unsigned j=0;j<8;j++) {
        hb_candidate *p=&candidates[j];
        if (p->mailbox==r->mailbox && p->tid==r->tid) { c=p; break; }
        if (p->last<oldest->last) oldest=p;
    }
    if (!c) { c=oldest; memset(c,0,sizeof *c); c->mailbox=(uintptr_t)r->mailbox; c->tid=r->tid; }
    /* Track the recent rate, allowing jitter; a 100 ms gap or lost raw
       records breaks continuous health, just as it does for gate arming. */
    candidate_observe(c,r,xell,1);
}
static void latch_try(void)
{
    int64_t now=qpc_now();
    int xell=g_heartbeat_source!=0;
    DWORD tid=(DWORD)__atomic_load_n(&g_render_tid,__ATOMIC_ACQUIRE);
    if (tid && (!last_latched_beat || now<last_latched_beat)) return;
    if (tid && now-last_latched_beat<g_qpf/2 &&
        candidate_rate_ok(&latched_health,last_latched_beat)) return;
    hb_candidate *c=NULL;
    for (unsigned j=0;j<8;j++) {
        hb_candidate *p=&candidates[j]; int64_t span=p->last-p->first;
        if (!p->tid || (!xell && !p->mailbox) || span<g_qpf || now<p->last ||
            now-p->last>=g_qpf/10 || !candidate_rate_ok(p,p->last)) continue;
        int64_t recent_span=p->last-p->recent[(p->count-21)%21];
        /* All pairs prove their own full second. Among qualified take pairs,
           prefer the highest recent rate, never an unqualified short burst.
           Keep xell's existing first-qualified TID selection unchanged. */
        if (!c || recent_span<c->last-c->recent[(c->count-21)%21]) c=p;
        if (xell) break;
    }
    if (!c) return;
    uintptr_t latched=__atomic_load_n(&g_expected_mailbox,__ATOMIC_ACQUIRE), next=c->mailbox;
    DWORD next_tid=c->tid;
    last_latched_beat=c->last;
    latched_health=*c;
    if (!tid) {
        if (!xell) __atomic_store_n(&g_expected_mailbox,next,__ATOMIC_RELEASE);
        __atomic_store_n(&g_render_tid,(LONG)next_tid,__ATOMIC_RELEASE);
        memset(candidates,0,sizeof candidates);
        if (xell)
            log_printf("HB LATCH source=%s tid=%lu",g_heartbeat_source==1 ? "xell_entry" : "xell_exit",
                       (unsigned long)next_tid);
        else
            log_printf("HB LATCH source=take mailbox=%p tid=%lu",(void *)next,(unsigned long)next_tid);
        return;
    }
    /* No CAS capture remains in the leaf. A transient mixed observation may
       record an extra beat; history/seq are reset and arming is held below. */
    __atomic_store_n(&g_render_tid,0,__ATOMIC_RELEASE);
    __atomic_store_n(&g_expected_mailbox,next,__ATOMIC_RELEASE);
    __atomic_store_n(&g_previous_packet,0,__ATOMIC_RELEASE);
    __atomic_store_n(&g_previous_frame,0,__ATOMIC_RELEASE);
    __atomic_store_n(&g_render_qpc,0,__ATOMIC_RELEASE);
    __atomic_store_n(&g_render_conflict,0,__ATOMIC_RELEASE);
    __atomic_store_n(&g_render_tid,(LONG)next_tid,__ATOMIC_RELEASE);
    InterlockedIncrement64(&g_hb_relatches);
    median_reset(); memset(candidates,0,sizeof candidates);
    G.armed=G.baseline_ok=0; G.baseline_start=0;
    G.last_seq=(uint64_t)__atomic_load_n(&g_render_seq,__ATOMIC_ACQUIRE);
    if (G.blocked_until<now+g_qpf) G.blocked_until=now+g_qpf;
    InterlockedIncrement(&g_factor_epoch);
    if (xell)
        log_printf("HB RELATCH source=%s tid=%lu \u2192 tid=%lu",
                   g_heartbeat_source==1 ? "xell_entry" : "xell_exit",
                   (unsigned long)tid,(unsigned long)next_tid);
    else
        log_printf("HB RELATCH old\u2192new: mailbox=%p TID=%lu \u2192 mailbox=%p TID=%lu",
                   (void *)latched,(unsigned long)tid,(void *)next,(unsigned long)next_tid);
}
static int hb_read_one(hb_rec *ring,uint64_t index,hb_rec *o)
{
    hb_rec *r=&ring[index&(HB_COUNT-1)];
    uint64_t stamp=__atomic_load_n(&r->stamp,__ATOMIC_ACQUIRE);
    if (stamp!=index+1) return 0;
    memcpy(o,r,sizeof *o);
    __atomic_thread_fence(__ATOMIC_ACQUIRE);
    return __atomic_load_n(&r->stamp,__ATOMIC_ACQUIRE)==stamp;
}
static int64_t heartbeat_median(void)
{
    int xell=g_heartbeat_source!=0;
    hb_rec *ring=xell ? xell_ring : g_hb_ring;
    uint64_t *read=xell ? &xell_read : &hb_read;
    uint64_t end=(uint64_t)__atomic_load_n(xell ? &xell_write : &g_hb_write,__ATOMIC_ACQUIRE);
    hb_rec r; int64_t sorted[16];
    if (end-*read>HB_COUNT) {
        *read=end-HB_COUNT; median_reset(); memset(candidates,0,sizeof candidates);
        memset(&latched_health,0,sizeof latched_health);
    }
    while (*read<end) {
        if (!hb_read_one(ring,*read,&r)) break;
        (*read)++;
        if (!xell && (r.flags&4)) continue;
        latch_observe(&r);
        if ((!xell && r.mailbox!=__atomic_load_n(&g_expected_mailbox,__ATOMIC_ACQUIRE)) ||
            r.tid!=(DWORD)__atomic_load_n(&g_render_tid,__ATOMIC_ACQUIRE)) continue;
        if (r.qpc>last_latched_beat) last_latched_beat=r.qpc;
        if (!first_beat) first_beat=r.qpc;
        if (last_beat && r.qpc>last_beat) {
            intervals[interval_pos++%16]=r.qpc-last_beat;
            if (interval_n<16) interval_n++;
        }
        last_beat=r.qpc;
    }
    /* Evaluate only after draining the batch, so the old pair's entire recent
       run can veto replacement. An incomplete batch cannot prove failure,
       since its uncommitted slot could belong to the old pair. */
    if (*read==end) latch_try();
    if (!xell) {
        uintptr_t published=__atomic_load_n(&g_published_mailbox,__ATOMIC_ACQUIRE);
        uintptr_t latched=__atomic_load_n(&g_expected_mailbox,__ATOMIC_ACQUIRE);
        int64_t now=qpc_now();
        if (published && latched && published!=latched && g_render_tid &&
            (!last_publish_warning || (now>=last_publish_warning && now-last_publish_warning>=10*g_qpf))) {
            last_publish_warning=now;
            log_printf("HB WARN publish mailbox mismatch published=%p latched=%p",(void *)published,(void *)latched);
        }
    }
    memcpy(sorted,intervals,interval_n*sizeof *sorted);
    for (unsigned j=1;j<interval_n;j++) {
        int64_t v=sorted[j]; unsigned k=j;
        while (k && sorted[k-1]>v) { sorted[k]=sorted[k-1]; k--; } sorted[k]=v;
    }
    if (!interval_n) return 0;
    return interval_n&1 ? sorted[interval_n/2] : (sorted[interval_n/2-1]+sorted[interval_n/2])/2;
}
static int64_t gate_clock(void *ctx) { (void)ctx; return qpc_now(); }
static uint64_t gate_seq(void *ctx) { (void)ctx; return (uint64_t)__atomic_load_n(&g_render_seq,__ATOMIC_ACQUIRE); }
static void gate_help(void *ctx) { ((void (*)(void *,uint8_t))g_resolved.help_jobs)(ctx,0); }
static void gate_event(void *ctx,int begin,uint64_t seq,int64_t now,int64_t waited,int timeout)
{
    uint64_t idx; trace_rec *r=trace_begin(&idx); (void)ctx;
    r->type=begin ? RT_GATE_BEGIN : RT_GATE_END; r->qpc=now;
    r->frame=(uint64_t)g_frame+1; r->tid=GetCurrentThreadId();
    r->aux=seq; r->dt=(double)waited/g_qpf; r->flags=(uint32_t)timeout;
    r->aux2=(uint32_t)G.backoffs; r->enabled=(uint32_t)G.session_off;
    trace_end(r,idx);
}
int lockstep_h1(uint8_t *clk,void *worker,DWORD tid)
{
    int64_t median=heartbeat_median();
    LONG pairs=InterlockedExchange(&h2_count,0), ptid=InterlockedExchange(&h2_tid,0);
    LONG64 ov1=InterlockedCompareExchange64(&g_ovl_h1_total,0,0), ov2=InterlockedCompareExchange64(&g_ovl_h2_total,0,0);
    if (ov1!=seen_ovl_h1 || ov2!=seen_ovl_h2) { pairs=-1; ptid=0; }
    seen_ovl_h1=ov1; seen_ovl_h2=ov2;
    gate_input in={0}; gate_result out;
    gate_io io={worker,gate_clock,gate_seq,gate_help,gate_event};
    in.enabled=g_l2_ready && g_resolved.online && g_cfg.lockstep && g_toggle && !g_cfg.fc.safe_mode;
    /* As with the clock/scene reads, guard the resolved global and null
       subsystem pointer, then take one plain volatile dword sample per H1. */
    if (g_resolved.online && g_resolved.online_global) {
        uintptr_t g=*(uintptr_t volatile *)g_resolved.online_global;
        if (g) in.online_flag=*(volatile int32_t *)(g+g_resolved.online_offset);
    }
    int basic=in.enabled && worker && *(uint8_t *volatile *)g_clock_global==clk && *(int32_t *)(clk+g_resolved.fps)==120 &&
        !clk[g_resolved.pause_a] && !clk[g_resolved.pause_b] &&
        *(void **)g_resolved.scene_global && !((uint8_t (*)(void))g_resolved.hook[A_SCENE])();
    int64_t hb=__atomic_load_n(&g_render_qpc,__ATOMIC_ACQUIRE);
    int64_t conflict=__atomic_load_n(&g_render_conflict,__ATOMIC_ACQUIRE);
    /* Read now AFTER shared timestamps: a concurrent heartbeat must not be
       mistaken for a future/unhealthy timestamp because median/scene work took time. */
    int64_t now=qpc_now();
    in.conditions=basic && hb>0 && now>=hb && now-hb<g_qpf/10 && median>0 && g_render_tid &&
        first_beat && last_beat-first_beat>=2*g_qpf && (!conflict || now-conflict>=2*g_qpf);
    in.tid=tid; in.paired_tid=(uint32_t)ptid;
    in.previous_h2=last_update_tid()==(DWORD)ptid ? (int)pairs : 0;
    in.now=now; in.qpf=g_qpf; in.median_interval=median;
    gate_tls_set(1);
    gate_run(&G,&in,&io,&out);
    if (out.online_edge>0) log_printf("ONLINE: gate disarmed (flag=%d)",in.online_flag);
    else if (out.online_edge<0) log_printf("ONLINE: cleared");
    /* An overlap on another worker while helping invalidates this tick too.
       Recursive help calls on this worker were already excluded by native TLS. */
    if (ov1!=InterlockedCompareExchange64(&g_ovl_h1_total,0,0) ||
        ov2!=InterlockedCompareExchange64(&g_ovl_h2_total,0,0)) {
        G.armed=G.baseline_ok=0; G.baseline_start=0; G.bad_pair=qpc_now();
        out.armed=0; out.reset=1;
    }
    gate_tls_set(2); /* H1 task continues through the native frame/publish */
    InterlockedExchange(&last_h1_tid,(LONG)tid);
    if (out.reset) InterlockedIncrement(&g_factor_epoch);
    if (out.tid_change) log_printf("GATE: update TID recaptured as %u (H1 now %lu); disarmed for at least 1 s",G.update_tid,(unsigned long)tid);
    if (out.backoff) {
        log_printf("GATE BACKOFF: three timeouts within 1 s; disarmed for 2 s, factors reset");
        if (G.session_off) log_printf("STATUS: L2/L3 PASSIVE (five backoffs in 60 s; session disabled)");
    }
    return out.armed && !out.backoff;
}
int lockstep_h2(DWORD tid)
{
    int bypass=gate_tls_get()==3;
    InterlockedExchange(&h2_tid,bypass ? 0 : (LONG)tid);
    LONG count=InterlockedIncrement(&h2_count);
    return !bypass && count==1 && tid==last_update_tid() && g_l3_tick;
}
uint64_t publish_detour(void *mailbox,void *a2,void *a3,void *a4)
{
    /* Native frame publishers following H1 provide an advisory mailbox.
       H1 independently selects the take mailbox/TID from sustained raw takes. */
    int tls=gate_tls_get(); DWORD tid=GetCurrentThreadId();
    int owned=tls==2 && tid==last_update_tid();
    __atomic_fetch_add(&publish_calls,1,__ATOMIC_RELAXED);
    if (owned) __atomic_fetch_add(&publish_owned,1,__ATOMIC_RELAXED);
    __atomic_store_n(&publish_mailbox,(uintptr_t)mailbox,__ATOMIC_RELEASE);
    LONG64 next=(LONG64)((uint64_t)(uint32_t)tls<<32 | tid);
    for (unsigned j=0;j<4;j++) {
        LONG64 pair=__atomic_load_n(&publish_pairs[j],__ATOMIC_RELAXED);
        if (!pair && __atomic_compare_exchange_n(&publish_pairs[j],&pair,next,0,
                                                 __ATOMIC_RELAXED,__ATOMIC_RELAXED)) break;
        if ((DWORD)pair==tid) {
            if (pair!=next) __atomic_store_n(&publish_pairs[j],next,__ATOMIC_RELAXED);
            break;
        }
    }
    uint64_t ret=((fn4)publish_tramp)(mailbox,a2,a3,a4);
    if (owned) {
        __atomic_store_n(&g_published_mailbox,(uintptr_t)mailbox,__ATOMIC_RELEASE);
    }
    return ret;
}
void lockstep_publish_snapshot(publish_diagnostics *out)
{
    out->calls=(uint64_t)__atomic_load_n(&publish_calls,__ATOMIC_RELAXED);
    out->owned=(uint64_t)__atomic_load_n(&publish_owned,__ATOMIC_RELAXED);
    out->mailbox=__atomic_load_n(&publish_mailbox,__ATOMIC_ACQUIRE);
    for (unsigned j=0;j<4;j++) {
        uint64_t pair=(uint64_t)__atomic_load_n(&publish_pairs[j],__ATOMIC_RELAXED);
        out->tid[j]=(DWORD)pair; out->tls[j]=(int)(pair>>32);
    }
}
uint64_t frame_detour(void *a1,void *a2,void *a3,void *a4)
{
    int tls=gate_tls_get();
    if (tls==1) return ((fn4)frame_tramp)(a1,a2,a3,a4);
    if (tls!=2) {
        uint64_t idx; trace_rec *r=trace_begin(&idx);
        r->type=RT_BYPASS; r->qpc=qpc_now(); r->tid=GetCurrentThreadId();
        r->aux=(uintptr_t)__builtin_return_address(0)-g_base; trace_end(r,idx);
        gate_tls_set(3); /* a frame without H1 cannot use a stale enabled tick */
    }
    uint64_t ret=((fn4)frame_tramp)(a1,a2,a3,a4);
    gate_tls_set(tls==3 ? 3 : 0);
    return ret;
}
static void xell_event(int exit,int64_t now)
{
    DWORD tid=GetCurrentThreadId();
    if (g_heartbeat_source==(exit ? 2 : 1)) {
        uint64_t index=(uint64_t)InterlockedIncrement64(&xell_write)-1;
        hb_rec *beat=&xell_ring[index&(HB_COUNT-1)];
        __atomic_store_n(&beat->stamp,0,__ATOMIC_RELEASE);
        beat->qpc=now; beat->mailbox=0; beat->tid=tid;
        if (tid==(DWORD)__atomic_load_n(&g_render_tid,__ATOMIC_ACQUIRE)) {
            InterlockedExchange64(&g_render_qpc,now); InterlockedIncrement64(&g_render_seq);
        }
        __atomic_store_n(&beat->stamp,index+1,__ATOMIC_RELEASE);
    }
    uint64_t idx; trace_rec *r=trace_begin(&idx);
    r->type=exit ? RT_XELL_EXIT : RT_XELL; r->qpc=now; r->tid=tid;
    r->frame=(uint64_t)g_frame; r->aux=g_xell_return-g_base;
    trace_end(r,idx);
}
uint64_t xell_detour(void *a1,void *a2,void *a3,void *a4)
{
    int match=(uintptr_t)__builtin_return_address(0)==g_xell_return && gate_tls_get()!=1;
    DWORD le=GetLastError();
    if (match) { int64_t now=qpc_now(); InterlockedExchange64(&g_last_xell_qpc,now); xell_event(0,now); }
    SetLastError(le);
    uint64_t ret=((fn4)xell_tramp)(a1,a2,a3,a4);
    le=GetLastError();
    if (match) xell_event(1,qpc_now());
    SetLastError(le);
    return ret;
}
void WINAPI sleep1_detour(DWORD ms)
{
    DWORD le=GetLastError(); int64_t start=qpc_now();
    InterlockedIncrement64(&g_sleep_polls);
    SetLastError(le); g_native_sleep(ms); le=GetLastError();
    InterlockedAdd64(&g_sleep_qpc,qpc_now()-start); SetLastError(le);
}
int sleep1_build(uint8_t *a,uintptr_t site,const uint8_t *ui,RUNTIME_FUNCTION **tbl,char *err,size_t n)
{
    size_t len; int32_t d; uintptr_t fn=(uintptr_t)sleep1_detour;
    if (stubs_transform_unwind(ui,a+0x100,&len,err,n)) return -1;
    memcpy(a,"\xFF\x15",2); d=12; memcpy(a+2,&d,4);
    memcpy(a+6,"\x0F\x84",2); d=(int32_t)(site+6-(uintptr_t)(a+12)); memcpy(a+8,&d,4);
    memcpy(a+12,"\x0F\x85",2); d=(int32_t)(site+6-(uintptr_t)(a+18)); memcpy(a+14,&d,4);
    memcpy(a+18,&fn,8);
    *tbl=(void *)(a+0x200); (*tbl)[0]=(RUNTIME_FUNCTION){0,18,0x100};
    return 0;
}
void heartbeat_drain_trace(void)
{
    static uint64_t rd;
    uint64_t end=(uint64_t)__atomic_load_n(&g_hb_write,__ATOMIC_ACQUIRE);
    hb_rec hb;
    if (end-rd>HB_COUNT) { log_printf("HB: raw recorder overflow %llu",(unsigned long long)(end-rd-HB_COUNT)); rd=end-HB_COUNT; }
    while (rd<end) {
        if (!hb_read_one(g_hb_ring,rd,&hb)) break;
        rd++;
        uint64_t idx; trace_rec *r=trace_begin(&idx);
        r->type=RT_HB; r->qpc=hb.qpc; r->tid=hb.tid; r->frame=hb.seq;
        r->aux=hb.packet; r->xell_qpc=(int64_t)hb.mailbox; r->flags=hb.flags;
        r->fps_a=(int32_t)hb.frame_no;
        r->aux2=(uint32_t)rd; trace_end(r,idx);
    }
}
