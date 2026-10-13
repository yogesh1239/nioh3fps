/* H1 (clock entry 0x5ad5f8) and H2 (main update entry 0x39e4e4) detours.
   Each computes its own real-time factor from the QPC delta between its own entries
   (PLAN section 2), guarded by an interlocked overlap counter in the stub data page.
   Overlapping calls change no state and only log. The P1/P2 stubs read r1/r2 from
   the data page; S2L enables them only together with the runtime gate. */
#include "common.h"
#include "lockstep.h"
#include "resolve.h"
#include "consumer_probe.h"
#include "consumer_fix.h"

void *h1_tramp, *h2_tramp;
volatile LONG64 g_ovl_h1_total, g_ovl_h2_total;

static factor_state g_st1, g_st2;   /* touched only inside the non-overlapping section */

typedef uint64_t (*fn4_t)(void *, void *, void *, void *);

uint64_t h1_detour(uint8_t *clk, void *a2, void *a3, void *a4)
{
    if (gate_tls_get()==1) return ((fn4_t)h1_tramp)(clk,a2,a3,a4);
    DWORD le = GetLastError();
    long n = factor_gate_enter(g_stubs.ovl_h1);
    if (n==1) InterlockedExchange(&g_l3_tick,lockstep_h1(clk,a2,GetCurrentThreadId()));
    int64_t now = qpc_now();
    uint64_t frame = (uint64_t)InterlockedIncrement64(&g_frame);
    uint64_t idx;
    uint64_t ret;
    trace_rec *r = trace_begin(&idx);
    r->point = 1;
    r->qpc = now;
    r->frame = frame;
    r->tid = GetCurrentThreadId();
    r->ovl = (uint32_t)n;
    r->xell_qpc = g_last_xell_qpc;
    r->marker = (uint32_t)g_marker;
    r->fps_a = *(volatile int32_t *)(clk + g_resolved.fps);
    r->fps_b = *(volatile int32_t *)(clk + g_resolved.actual_fps);
    r->speed_req = *(volatile float *)(clk + g_resolved.req);
    r->speed_eff = *(volatile float *)(clk + g_resolved.eff);
    r->aux = (g_clock_global && *(uint8_t *volatile *)g_clock_global == clk) ? 1 : 0;
    if (n > 1) {
        /* another worker is inside: change nothing, P1 keeps using the current r1 */
        InterlockedIncrement64(&g_ovl_h1_total);
        r->type = RT_H1_OVERLAP;
        r->flags = FF_OVERLAP;
        r->r_would = g_st1.r;
        r->r_applied = *g_stubs.r1;
    } else {
        int en = g_l3_tick != 0;
        static LONG epoch;
        LONG current=InterlockedCompareExchange(&g_factor_epoch,0,0);
        if (epoch!=current) {
            double lost=g_st1.time_lost, gained=g_st1.time_gained;
            factor_init(&g_st1); g_st1.time_lost=lost; g_st1.time_gained=gained; epoch=current;
        }
        factor_out o;
        float rv = factor_update(&g_st1, &g_cfg.fc, now, g_qpf, r->fps_a, en, &o);
        *g_stubs.r1 = rv;
#ifndef LOCKSTEP_TEST
        consumer_fix_publish(rv,frame,en);
        consumer_probe_tick(now);
#endif
        r->type = RT_H1;
        r->enabled = (uint32_t)en;
        r->flags = o.flags;
        r->dt = o.dt;
        r->r_would = rv;
        r->r_applied = *g_stubs.r1;
        r->debt = g_st1.debt;
        r->time_lost = g_st1.time_lost;
        r->time_gained = g_st1.time_gained;
    }
    trace_end(r, idx);
    SetLastError(le);
    int tls_before=gate_tls_get();
    if (n>1) gate_tls_set(1); /* original math on an overlapping worker */
    ret = ((fn4_t)h1_tramp)(clk, a2, a3, a4);
    if (n>1) gate_tls_set(tls_before);
    factor_gate_leave(g_stubs.ovl_h1);
    return ret;
}

uint64_t h2_detour(void *self, uint8_t *S, void *a3, void *a4)
{
    if (gate_tls_get()==1) return ((fn4_t)h2_tramp)(self,S,a3,a4);
    DWORD le = GetLastError();
    int64_t now = qpc_now();
    long n = factor_gate_enter(g_stubs.ovl_h2);
    int paired_en=n==1 ? lockstep_h2(GetCurrentThreadId()) : 0;
    uint64_t idx;
    uint64_t ret;
    trace_rec *r = trace_begin(&idx);
    r->point = 2;
    r->qpc = now;
    r->frame = (uint64_t)g_frame;
    r->tid = GetCurrentThreadId();
    r->ovl = (uint32_t)n;
    r->aux2 = (uint32_t)*g_stubs.ovl_h1;      /* H1 currently inside on another thread? */
    r->xell_qpc = g_last_xell_qpc;
    r->marker = (uint32_t)g_marker;
    r->fps_a = *(volatile int32_t *)(S + g_resolved.s_fps);
    r->fps_b = -1;
    r->speed_req = *(volatile float *)(S + g_resolved.s_req);
    r->speed_eff = *(volatile float *)(S + g_resolved.s_eff);
    if (n > 1) {
        InterlockedIncrement64(&g_ovl_h2_total);
        r->type = RT_H2_OVERLAP;
        r->flags = FF_OVERLAP;
        r->r_would = g_st2.r;
        r->r_applied = *g_stubs.r2;
    } else {
        int en = paired_en;
        static LONG epoch;
        LONG current=InterlockedCompareExchange(&g_factor_epoch,0,0);
        if (epoch!=current) {
            double lost=g_st2.time_lost, gained=g_st2.time_gained;
            factor_init(&g_st2); g_st2.time_lost=lost; g_st2.time_gained=gained; epoch=current;
        }
        factor_out o;
        float rv = factor_update(&g_st2, &g_cfg.fc, now, g_qpf, r->fps_a, en, &o);
        *g_stubs.r2 = rv;
        r->type = RT_H2;
        r->enabled = (uint32_t)en;
        r->flags = o.flags;
        r->dt = o.dt;
        r->r_would = rv;
        r->r_applied = *g_stubs.r2;
        r->debt = g_st2.debt;
        r->time_lost = g_st2.time_lost;
        r->time_gained = g_st2.time_gained;
    }
    trace_end(r, idx);
    SetLastError(le);
    int tls_before=gate_tls_get();
    if (n>1) gate_tls_set(1);
    ret = ((fn4_t)h2_tramp)(self, S, a3, a4);
    if (n>1) gate_tls_set(tls_before);
    factor_gate_leave(g_stubs.ovl_h2);
    return ret;
}

void hooks_init_state(void)
{
    factor_init(&g_st1);
    factor_init(&g_st2);
    lockstep_init();
}
