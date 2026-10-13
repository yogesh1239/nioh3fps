/* Native test of src/factor.c against the review7 scenario matrix
   (design-time simulations, not shipped). Asserts:
   - r within [ScaleMin, ScaleMax] and never NaN, every tick, every config;
   - Raw: zero error outside clamps, hitches and passthrough ticks (exact in double;
     the stored float r adds only float32 rounding);
   - Smooth: |debt| <= 1 frame, and game-real drift since the last reset equals
     -(debt) - lost + gained, so drift beyond 1 frame only happens through logged
     clamp drops, and drops happen only on ticks where clampNaN changed r;
   - overlap path: the gated call leaves the state byte-identical;
   - toggle / fps-mode changes / dt <= 0 force passthrough ticks. */
#include "../src/factor.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define QPF 10000000LL
static int g_fail, g_checks;
#define CHECK(c, ...) do { g_checks++; if (!(c)) { g_fail++; if (g_fail < 40) { printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } } while (0)

/* Deterministic RNG. Python's random is not reproduced bit-exactly; the asserted
   properties do not depend on the exact samples. */
static uint64_t rs = 7;
static double urand(void) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return (double)(rs >> 11) / 9007199254740992.0; }
static double gauss(void) { double u1 = urand(), u2 = urand(); if (u1 < 1e-300) u1 = 1e-300; return sqrt(-2 * log(u1)) * cos(6.283185307179586 * u2); }

typedef struct { const char *name; double *d; int n; } scen;
static double *mk(int n) { return (double *)malloc(sizeof(double) * (size_t)n); }
static scen sc_const(const char *nm, double v, int n) { scen s = { nm, mk(n), n }; for (int i = 0; i < n; i++) s.d[i] = v; return s; }
static scen sc_dip(const char *nm, double f, double base, int n, int post)
{
    int tot = 200 + n + post; scen s = { nm, mk(tot), tot }; int k = 0;
    for (int i = 0; i < 200; i++) s.d[k++] = 1.0 / base;
    for (int i = 0; i < n; i++) s.d[k++] = 1.0 / f;
    for (int i = 0; i < post; i++) s.d[k++] = 1.0 / base;
    return s;
}
static scen sc_spike(const char *nm, double v)
{
    scen s = { nm, mk(231), 231 }; int k = 0;
    for (int i = 0; i < 200; i++) s.d[k++] = 1.0 / 90;
    s.d[k++] = v;
    for (int i = 0; i < 30; i++) s.d[k++] = 1.0 / 90;
    return s;
}

typedef struct { const char *name; factor_cfg c; } cfgdef;

static factor_cfg base_cfg(void) { factor_cfg c = { TM_RAW, 8, 0.2, 0.5, 2.0, 0 }; return c; }

static void run_scenario(const scen *s, const factor_cfg *c, const char *cname)
{
    factor_state st; factor_out o;
    long long t = 12345;
    double cum = 0, lost0 = 0, gained0 = 0, errU_d = 0, errU_f = 0, maxcum = 0, maxdebt = 0;
    const int fps = 120; const double inv = 1.0 / fps;
    int nclamp = 0, nhitch = 0, npass = 0;
    factor_init(&st);
    for (int i = 0; i < s->n; i++) {
        double lost_b = st.time_lost, gained_b = st.time_gained;
        float r;
        t += llround(s->d[i] * QPF);
        r = factor_update(&st, c, t, QPF, fps, 1, &o);
        CHECK(!isnan(r) && r >= (float)c->scale_min && r <= (float)c->scale_max,
              "%s %s %s tick %d r=%g out of [%g,%g]", cname, c->mode ? "Smooth" : "Raw", s->name, i, r, c->scale_min, c->scale_max);
        if (c->safe_mode) CHECK(r == 1.0f && (o.flags & FF_PASS), "safe mode r=%g", r);
        if (o.flags & FF_CLAMP) nclamp++;
        if (o.flags & (FF_PASS | FF_HITCH)) {
            if (o.flags & FF_HITCH) nhitch++; else npass++;
            cum = 0; lost0 = st.time_lost; gained0 = st.time_gained;
            CHECK(st.debt == 0.0 && st.hist_n == 0, "reset/hitch must clear state");
            if (o.flags & FF_HITCH) CHECK(r == (float)c->scale_max, "hitch r");
            continue;
        }
        cum += (double)r / fps - o.dt;
        if (fabs(cum) > maxcum) maxcum = fabs(cum);
        if (c->mode == TM_RAW) {
            CHECK(st.debt == 0.0 && st.time_lost >= 0.0 && st.time_gained >= 0.0, "Raw keeps no debt; v8 separately accounts dropped clamps/hitches");
            if (!(o.flags & FF_CLAMP)) {
                double rd = o.dt * fps;
                CHECK(r == (float)rd, "Raw r must equal (float)(dt*fps)");
                errU_d += rd / fps - o.dt;
                errU_f += (double)r / fps - o.dt;
            }
        } else {
            double ad = fabs(st.debt);
            if (ad > maxdebt) maxdebt = ad;
            CHECK(ad <= inv + 1e-9, "%s Smooth %s tick %d |debt|=%.9f > 1 frame", cname, s->name, i, ad);
            CHECK(fabs(cum + st.debt + (st.time_lost - lost0) - (st.time_gained - gained0)) < 1e-9,
                  "%s Smooth %s tick %d drift invariant broken", cname, s->name, i);
            if (st.time_lost != lost_b || st.time_gained != gained_b)
                CHECK((o.flags & FF_CLAMP) && (o.flags & FF_DEBTCLAMP), "%s %s drop without a real clamp at tick %d", cname, s->name, i);
        }
    }
    if (c->mode == TM_RAW) {
        CHECK(fabs(errU_d) < 1e-12, "%s Raw %s errU(double)=%g", cname, s->name, errU_d);
        CHECK(fabs(errU_f) < 1e-6, "%s Raw %s errU(float)=%g", cname, s->name, errU_f);
    }
    printf("  %-7s %-6s %-13s n=%-5d r=[%5.3f..] clamp=%-5d hitch=%-3d pass=%-3d max|game-real|=%7.3fms |debt|max=%6.3fms lost=%8.3fms gained=%8.3fms errU=%+.2eS\n",
           cname, c->mode ? "Smooth" : "Raw", s->name, s->n, (double)c->scale_min, nclamp, nhitch, npass,
           maxcum * 1e3, maxdebt * 1e3, st.time_lost * 1e3, st.time_gained * 1e3, errU_f);
}

static void test_overlap(const scen *s, int mode)
{
    factor_cfg c = base_cfg(); factor_state st; factor_out o;
    volatile long ctr = 0; long long t = 12345; int bad = 0, n = 0;
    c.mode = mode; factor_init(&st);
    for (int i = 0; i < s->n && i < 3000; i++) {
        t += llround(s->d[i] * QPF);
        if (i >= 5 && (i - 5) % 37 == 0) {
            factor_state before = st;
            ctr = 1;  /* another worker is inside the hook */
            if (factor_gate_enter(&ctr) > 1) {
                /* overlap path: change nothing, P1/P2 keep the current r */
            } else {
                factor_update(&st, &c, t + 5, QPF, 120, 1, &o);
            }
            factor_gate_leave(&ctr);
            CHECK(ctr == 1, "overlap path must restore the counter");
            ctr = 0;
            n++;
            if (memcmp(&before, &st, sizeof st) != 0) bad++;
        }
        if (factor_gate_enter(&ctr) == 1) factor_update(&st, &c, t, QPF, 120, 1, &o);
        else bad++;
        factor_gate_leave(&ctr);
        CHECK(ctr == 0, "counter must return to 0");
    }
    CHECK(bad == 0, "overlap changed state %d times", bad);
    printf("  overlap %-6s injected=%d state-changes=%d\n", mode ? "Smooth" : "Raw", n, bad);
}

static void test_toggle_fps(void)
{
    factor_cfg c = base_cfg(); factor_state st; factor_out o; long long t = 12345;
    uint32_t fl[220];
    c.mode = TM_SMOOTH; factor_init(&st);
    for (int i = 0; i < 220; i++) {
        int en = !(i >= 100 && i < 120);
        int fps = (i >= 50 && i < 70) ? 60 : 120;
        float r;
        t += llround(QPF / 100.0);
        r = factor_update(&st, &c, t, QPF, fps, en, &o);
        fl[i] = o.flags;
        CHECK(!isnan(r) && r >= 0.5f && r <= 2.0f, "toggle r");
        if (o.flags & FF_PASS) CHECK(r == 1.0f, "passthrough r must be 1.0");
    }
    CHECK(fl[0] & FF_FIRST, "first tick");
    CHECK(!(fl[48] & FF_PASS) && !(fl[49] & FF_PASS), "48,49 normal");
    CHECK((fl[50] & FF_FPSCHG) && (fl[50] & FF_PASS), "50 fps change");
    CHECK((fl[51] & FF_FPSNOT120) && (fl[52] & FF_PASS), "51/52 fps!=120");
    CHECK((fl[70] & FF_FPSCHG) && (fl[70] & FF_PASS) && !(fl[71] & FF_PASS), "70 back to 120 resets once");
    CHECK(!(fl[99] & FF_PASS), "99 normal");
    CHECK((fl[100] & FF_TOGGLE) && (fl[100] & FF_PASS) && (fl[101] & FF_DISABLED), "100 toggle off");
    CHECK((fl[120] & FF_TOGGLE) && (fl[120] & FF_PASS) && !(fl[121] & FF_PASS), "120 toggle on: one passthrough tick");
    factor_init(&st);
    factor_update(&st, &c, 1000, QPF, 120, 1, &o);
    factor_update(&st, &c, 1000, QPF, 120, 1, &o);
    CHECK((o.flags & FF_DTNONPOS) && (o.flags & FF_PASS), "dt==0 reset");
    factor_update(&st, &c, 900, QPF, 120, 1, &o);
    CHECK((o.flags & FF_DTNONPOS) && st.r == 1.0f, "dt<0 reset");
    printf("  toggle / fps-change / dt<=0 sequence checked\n");
}

static void test_clampnan(void)
{
    factor_cfg c = base_cfg(); int ch, nn;
    float r = factor_clampnan(&c, NAN, &ch, &nn);
    CHECK(r == 1.0f && ch && nn, "clampNaN(NaN) = 1.0");
    r = factor_clampnan(&c, 5.0, &ch, &nn); CHECK(r == 2.0f && ch, "clamp hi");
    r = factor_clampnan(&c, 0.1, &ch, &nn); CHECK(r == 0.5f && ch, "clamp lo");
    r = factor_clampnan(&c, 1.25, &ch, &nn); CHECK(r == 1.25f && !ch, "no clamp");
}

/* Smooth post-dip trajectory (plan r7 note: about 1.6 -> 1.4 -> 1.0 -> 0.83) */
static void show_post_dip(void)
{
    scen s = sc_dip("120-65x100", 65, 120, 100, 80);
    factor_cfg c = base_cfg(); factor_state st; factor_out o; long long t = 12345;
    c.mode = TM_SMOOTH; factor_init(&st);
    printf("  Smooth 120-65x100 post-dip r:");
    for (int i = 0; i < s.n; i++) {
        float r; t += llround(s.d[i] * QPF);
        r = factor_update(&st, &c, t, QPF, 120, 1, &o);
        if (i >= 300 && i < 310) printf(" %.2f", r);
    }
    printf("\n");
}

int main(void)
{
    scen S[32]; int ns = 0;
    S[ns++] = sc_const("steady120", 1.0 / 120, 600);
    S[ns++] = sc_const("steady80", 1.0 / 80, 600);
    { scen s = { "alt80/120", mk(600), 600 }; for (int i = 0; i < 600; i++) s.d[i] = (i & 1) ? 1.0 / 120 : 1.0 / 80; S[ns++] = s; }
    S[ns++] = sc_dip("120-50x100", 50, 120, 100, 80);
    S[ns++] = sc_dip("90-50x100", 50, 90, 100, 80);
    S[ns++] = sc_dip("120-65x100", 65, 120, 100, 80);
    S[ns++] = sc_dip("120-62x100", 62, 120, 100, 80);
    S[ns++] = sc_dip("120-70x100", 70, 120, 100, 80);
    S[ns++] = sc_dip("120-60.5x100", 60.5, 120, 100, 80);
    S[ns++] = sc_dip("120-61x300", 61, 120, 300, 80);
    S[ns++] = sc_spike("hitch300", 0.3);
    S[ns++] = sc_spike("spike200", 0.2);
    { scen s = { "jitter90", mk(20000), 20000 }; for (int i = 0; i < 20000; i++) { double v = 1.0 / 90 + (urand() * 0.006 - 0.003); s.d[i] = v < 0.0005 ? 0.0005 : v; } S[ns++] = s; }
    { scen s = { "heavyjit", mk(20000), 20000 }; for (int i = 0; i < 20000; i++) { double v = 1.0 / 100 + gauss() * 0.004; s.d[i] = v < 0.0001 ? 0.0001 : v; } S[ns++] = s; }
    { scen s = { "fast300fps", mk(250), 250 }; for (int i = 0; i < 250; i++) s.d[i] = i < 200 ? 1.0 / 300 : 1.0 / 120; S[ns++] = s; }
    { scen s = { "zero_dt", mk(23), 23 }; for (int i = 0; i < 23; i++) s.d[i] = (i >= 10 && i < 13) ? 0.0 : 1.0 / 120; S[ns++] = s; }

    cfgdef C[8]; int nc = 0;
    C[nc].name = "default"; C[nc++].c = base_cfg();
    C[nc].name = "SMax1";   C[nc].c = base_cfg(); C[nc++].c.scale_max = 1.0;
    C[nc].name = "SMin1";   C[nc].c = base_cfg(); C[nc++].c.scale_min = 1.0;
    C[nc].name = "N1";      C[nc].c = base_cfg(); C[nc++].c.avg_frames = 1;
    C[nc].name = "K1";      C[nc].c = base_cfg(); C[nc++].c.smooth_k = 1.0;
    C[nc].name = "N32";     C[nc].c = base_cfg(); C[nc++].c.avg_frames = 32;
    C[nc].name = "safe";    C[nc].c = base_cfg(); C[nc++].c.safe_mode = 1;

    printf("scenario matrix: %d scenarios x %d configs x 2 modes (r stored as float32, as in the DLL)\n", ns, nc);
    for (int ci = 0; ci < nc; ci++)
        for (int m = 0; m < 2; m++)
            for (int si = 0; si < ns; si++) {
                factor_cfg c = C[ci].c; c.mode = m;
                run_scenario(&S[si], &c, C[ci].name);
            }
    test_overlap(&S[12], TM_RAW);
    test_overlap(&S[12], TM_SMOOTH);
    test_toggle_fps();
    test_clampnan();
    show_post_dip();
    printf("%d checks, %d failures\n", g_checks, g_fail);
    printf(g_fail ? "TEST_FACTOR: FAIL\n" : "TEST_FACTOR: PASS\n");
    return g_fail ? 1 : 0;
}
