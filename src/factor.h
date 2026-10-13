/* Real-time factor computation (PLAN section 2). Pure: no globals, no OS calls. */
#pragma once
#include <stdint.h>

enum { TM_RAW = 0, TM_SMOOTH = 1 };
#define FACTOR_HIST_MAX 32

typedef struct {
    int    mode;        /* TM_RAW / TM_SMOOTH */
    int    avg_frames;  /* [1, 32] */
    double smooth_k;    /* (0, 1] */
    double scale_min;   /* [0.5, 1.0] */
    double scale_max;   /* [1.0, 2.0] */
    int    safe_mode;
} factor_cfg;

typedef struct {
    int64_t qpc_prev;   /* 0 = none */
    int     prev_fps;
    int     prev_enabled;
    double  hist[FACTOR_HIST_MAX];
    int     hist_n, hist_pos;
    double  debt;        /* seconds owed to the game */
    float   r;           /* stored as a 32-bit float, like the data page */
    double  time_lost, time_gained;
} factor_state;

/* out flags: why this tick did what it did */
#define FF_FIRST      0x0001u
#define FF_TOGGLE     0x0002u  /* enabled != prev_enabled */
#define FF_FPSCHG     0x0004u  /* fps != prev_fps */
#define FF_FPSNOT120  0x0008u
#define FF_DTNONPOS   0x0010u
#define FF_DISABLED   0x0020u
#define FF_SAFE       0x0040u
#define FF_RESET      0x0080u  /* reset condition was true */
#define FF_PASS       0x0100u  /* passthrough tick (r = 1.0) */
#define FF_HITCH      0x0200u
#define FF_CLAMP      0x0400u  /* r changed by clampNaN */
#define FF_DEBTCLAMP  0x0800u  /* Smooth: |debt| clamped, time_lost/gained grew */
#define FF_NAN        0x1000u
#define FF_OVERLAP    0x2000u  /* set by the hook, never by factor_update */

typedef struct {
    double   dt;
    uint32_t flags;
} factor_out;

void  factor_init(factor_state *s);
float factor_clampnan(const factor_cfg *c, double x, int *changed, int *was_nan);
/* One tick at a patch point. Returns the new r (also in s->r). */
float factor_update(factor_state *s, const factor_cfg *c, int64_t now, int64_t qpf,
                    int fps, int enabled, factor_out *o);

/* Overlap gate (section 2): increment on entry; >1 means another worker is inside
   this hook and the call must change no state. The caller always decrements after
   the original returns. Uses GCC atomics so the native test exercises the same code. */
static inline long factor_gate_enter(volatile long *ctr) { return __atomic_add_fetch(ctr, 1, __ATOMIC_SEQ_CST); }
static inline void factor_gate_leave(volatile long *ctr) { __atomic_sub_fetch(ctr, 1, __ATOMIC_SEQ_CST); }
