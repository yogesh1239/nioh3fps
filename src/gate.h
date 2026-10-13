#pragma once
#include <stdint.h>
typedef struct {
    uint64_t last_seq;
    int armed, session_off, reset, baseline_ok, online;
    uint32_t update_tid;
    int64_t baseline_start, blocked_until, bad_pair;
    unsigned baseline_ticks;
    int64_t timeout_at[3], backoff_at[5];
    unsigned nt, nb;
    uint64_t ticks, armed_ticks, waits, timeouts, backoffs;
} gate_state;
typedef struct {
    int enabled, conditions, in_gate, previous_h2;
    uint32_t tid, paired_tid;
    int64_t now, qpf, median_interval;
    int32_t online_flag;
} gate_input;
typedef struct {
    void *ctx;
    int64_t (*clock)(void *);
    uint64_t (*seq)(void *);
    void (*help)(void *);
    void (*event)(void *, int begin, uint64_t seq, int64_t now, int64_t waited, int timeout);
} gate_io;
typedef struct { int armed, waited, timeout, backoff, tid_change, reset, online_edge; int64_t wait_qpc; uint64_t seq; } gate_result;
void gate_init(gate_state *s);
void gate_run(gate_state *s, const gate_input *in, const gate_io *io, gate_result *out);
