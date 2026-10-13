/* v8 bounded help-jobs gate; platform-free and exercised with a fake clock. */
#include "gate.h"
#include <string.h>
void gate_init(gate_state *s) { memset(s,0,sizeof *s); }
void gate_run(gate_state *s, const gate_input *i, const gate_io *io, gate_result *o)
{
    int eligible, was_armed=s->armed;
    int64_t now=i->now;
    memset(o,0,sizeof *o);
    if (i->in_gate) return; /* includes no counters, pairing or consume */
    s->ticks++;
    if (i->online_flag) {
        if (!s->online) o->online_edge=1;
        s->online=1; s->baseline_ok=0; s->baseline_start=0;
    } else if (s->online) {
        s->online=0; o->online_edge=-1;
        if (s->blocked_until<now+2*i->qpf) s->blocked_until=now+2*i->qpf;
    }
    if (i->previous_h2==1 && i->paired_tid) {
        if (s->update_tid && s->update_tid!=i->paired_tid) {
            if (s->blocked_until<now+i->qpf) s->blocked_until=now+i->qpf;
            o->tid_change=1; s->baseline_ok=0;
            s->baseline_start=0;
        }
        s->update_tid=i->paired_tid;
    } else { s->bad_pair=now; s->baseline_ok=0; s->baseline_start=0; }
    /* Validate native 120 Hz over a complete unarmed two-second window.
       An armed gate changes that rate, so retain its baseline until disarmed. */
    if (!was_armed && !s->online && i->previous_h2==1 && i->paired_tid==i->tid) {
        if (!s->baseline_start) { s->baseline_start=now; s->baseline_ticks=0; }
        else s->baseline_ticks++;
        if (now-s->baseline_start>=2*i->qpf) {
            double rate=(double)s->baseline_ticks*i->qpf/(double)(now-s->baseline_start);
            /* Pairing is checked on every tick above; a missing/extra H2
               restarts the window. Counting it again would be tautological. */
            s->baseline_ok=rate>=115 && rate<=125;
            s->baseline_start=now; s->baseline_ticks=0;
        }
    }
    eligible=i->enabled && i->conditions && !s->online && s->baseline_ok &&
        i->previous_h2==1 && i->paired_tid==i->tid && s->update_tid==i->tid &&
        now>=s->blocked_until && !s->session_off && (!s->bad_pair || now-s->bad_pair>=2*i->qpf);
    if (!eligible) {
        s->armed=0;
        if (was_armed) { o->reset=1; s->baseline_ok=0; s->baseline_start=0; }
        s->last_seq=io->seq(io->ctx);
        return;
    }
    s->armed=1; s->armed_ticks++; o->armed=1;
    o->seq=io->seq(io->ctx);
    if (!was_armed) { s->last_seq=o->seq; o->reset=1; return; } /* bootstrap */
    if (o->seq==s->last_seq) {
        int64_t budget=i->median_interval*5/2;
        if (budget>i->qpf/20) budget=i->qpf/20;
        if (budget<=0) { s->armed=0; o->armed=0; o->reset=1; return; }
        int64_t deadline=now+budget, start=now;
        o->waited=1; s->waits++;
        if (io->event) io->event(io->ctx,1,o->seq,now,0,0);
        while (io->seq(io->ctx)==s->last_seq && (now=io->clock(io->ctx))<deadline)
            io->help(io->ctx);
        now=io->clock(io->ctx); o->seq=io->seq(io->ctx); o->wait_qpc=now-start;
        if (o->seq==s->last_seq) {
            o->timeout=1; s->timeouts++;
            s->timeout_at[s->nt++%3]=now;
            if (s->nt>=3 && now-s->timeout_at[s->nt%3]<=i->qpf) {
                o->backoff=1; o->reset=1; s->backoffs++; s->armed=0;
                s->baseline_ok=0; s->baseline_start=0; s->blocked_until=now+2*i->qpf;
                s->nt=0; memset(s->timeout_at,0,sizeof s->timeout_at);
                s->backoff_at[s->nb++%5]=now;
                if (s->nb>=5 && now-s->backoff_at[s->nb%5]<=60*i->qpf) s->session_off=1;
            }
        }
        if (io->event) io->event(io->ctx,0,o->seq,now,o->wait_qpc,o->timeout);
    }
    s->last_seq=o->seq; /* consume latest: a burst is never banked */
}
