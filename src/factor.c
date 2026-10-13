/* Exact implementation of the PLAN v7.1 section 2 pseudo-code. */
#include "factor.h"
#include <string.h>
#include <math.h>

void factor_init(factor_state *s)
{
    memset(s, 0, sizeof *s);
    s->r = 1.0f;
}

float factor_clampnan(const factor_cfg *c, double x, int *changed, int *was_nan)
{
    double y;
    *was_nan = isnan(x) ? 1 : 0;
    if (*was_nan) { *changed = 1; return 1.0f; }
    y = x < c->scale_min ? c->scale_min : x;
    y = y > c->scale_max ? c->scale_max : y;
    *changed = (y != x);
    return (float)y;
}

static void hist_clear(factor_state *s) { s->hist_n = 0; s->hist_pos = 0; }

static void hist_push(factor_state *s, int n, double v)
{
    s->hist[s->hist_pos] = v;
    s->hist_pos = (s->hist_pos + 1) % n;
    if (s->hist_n < n) s->hist_n++;
}

static double hist_mean(const factor_state *s)
{
    double sum = 0.0;
    int i;
    for (i = 0; i < s->hist_n; i++) sum += s->hist[i];
    return sum / (double)s->hist_n;
}

float factor_update(factor_state *s, const factor_cfg *c, int64_t now, int64_t qpf,
                    int fps, int enabled, factor_out *o)
{
    uint32_t f = 0;
    int first = (s->qpc_prev == 0);
    double dt = first ? 0.0 : (double)(now - s->qpc_prev) / (double)qpf;
    int reset, hitch, changed, was_nan;

    s->qpc_prev = now;
    if (first) f |= FF_FIRST;
    if (!first && enabled != s->prev_enabled) f |= FF_TOGGLE;
    if (!first && fps != s->prev_fps) f |= FF_FPSCHG;
    if (fps != 120) f |= FF_FPSNOT120;
    if (!first && dt <= 0.0) f |= FF_DTNONPOS;
    reset = (f & (FF_FIRST | FF_TOGGLE | FF_FPSCHG | FF_FPSNOT120 | FF_DTNONPOS)) != 0;
    hitch = !reset && c->scale_max > 1.0 && dt >= c->scale_max / (double)fps;
    s->prev_fps = fps;
    s->prev_enabled = enabled;
    if (reset) f |= FF_RESET;
    if (!enabled) f |= FF_DISABLED;
    if (c->safe_mode) f |= FF_SAFE;

    if (!enabled || c->safe_mode || reset) {
        hist_clear(s); s->debt = 0.0; s->r = 1.0f;
        f |= FF_PASS;
    } else if (hitch) {
        hist_clear(s); s->debt = 0.0; s->r = (float)c->scale_max;
        f |= FF_HITCH;
    } else if (c->mode == TM_RAW) {
        s->r = factor_clampnan(c, dt * (double)fps, &changed, &was_nan);
        if (changed) f |= FF_CLAMP;
        if (was_nan) f |= FF_NAN;
    } else {
        double inv = 1.0 / (double)fps;
        double capped = dt < c->scale_max * inv ? dt : c->scale_max * inv;
        double target, owed, given, lo, hi;
        hist_push(s, c->avg_frames, capped);
        target = hist_mean(s);
        owed = s->debt + dt;
        given = target + c->smooth_k * (owed - target);
        lo = owed - inv; hi = owed + inv;
        given = given < lo ? lo : (given > hi ? hi : given);
        s->r = factor_clampnan(c, given * (double)fps, &changed, &was_nan);
        if (changed) f |= FF_CLAMP;
        if (was_nan) f |= FF_NAN;
        s->debt = owed - (double)s->r / (double)fps;
        if (changed && fabs(s->debt) > inv) {
            if (s->debt > 0) s->time_lost += s->debt - inv;
            else             s->time_gained += -s->debt - inv;
            s->debt = s->debt > inv ? inv : (s->debt < -inv ? -inv : s->debt);
            f |= FF_DEBTCLAMP;
        }
    }
    /* v8 M5: Raw clamps/hitches are intentionally dropped, and now accounted.
       Reset/passthrough intervals are excluded, as in the v7 accuracy check. */
    if (enabled && !c->safe_mode && !reset && (hitch || (c->mode==TM_RAW && (f&FF_CLAMP)))) {
        double delta=dt-(double)s->r/(double)fps;
        if (delta>0) s->time_lost+=delta;
        else s->time_gained-=delta;
    }
    if (o) { o->dt = dt; o->flags = f; }
    return s->r;
}
