/* The single writer thread: all file I/O happens here (log, rotation, tick/histogram
   CSVs, trace snapshots). It also polls the hotkeys (edge-detected, only while the game
   window is in the foreground), drains the flight recorder to compute the 10 s
   summaries and the per-frame ordering statistics, and merges the per-thread probe
   counters once per second. Game threads never wait on it. */
#include "common.h"
#include "consumer_probe.h"
#include "lockstep.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <stdarg.h>

#define LOG_ROTATE_BYTES (5u * 1024 * 1024)
#ifndef RELEASE
/* per-tick CSV ~50 KB/s at 120 fps: 64 MB is ~20 min; one .old kept, so <= 128 MB on disk.
   hist CSV is per-second rows: 16 MB + .old. The collect script zips only the current
   ticks file to keep the report small. */
#define TICKS_ROTATE_BYTES (64ll * 1024 * 1024)
#define HIST_ROTATE_BYTES  (16ll * 1024 * 1024)
#define HIST_HEADER "t_s,target,rva,kind,ret_rva,count\n"
#endif

static HANDLE g_thread, g_evt_ready, g_evt_flushed;
static volatile LONG g_flush_req, g_dump_req;
static char g_dump_why[32];
static FILE *g_log;
#ifndef RELEASE
static FILE *g_ticks, *g_hist;
#endif
static uint64_t g_log_bytes;
static wchar_t g_log_path[MAX_PATH], g_log_old[MAX_PATH];
static int g_ready_ok;
#ifndef RELEASE
static int g_detail;              /* tick + histogram CSVs: probe build or LogLevel=Debug */
#endif

/* ---------------- file helpers ---------------- */

static void out_path(wchar_t *p, const wchar_t *name) { _snwprintf(p, MAX_PATH, L"%ls\\%ls", g_outdir, name); p[MAX_PATH - 1] = 0; }

#ifndef RELEASE
static void rotate(const wchar_t *name, const wchar_t *old)
{
    wchar_t a[MAX_PATH], b[MAX_PATH];
    out_path(a, name); out_path(b, old);
    if (GetFileAttributesW(a) != INVALID_FILE_ATTRIBUTES) MoveFileExW(a, b, MOVEFILE_REPLACE_EXISTING);
}
#endif

static int dir_writable(const wchar_t *dir)
{
    wchar_t p[MAX_PATH];
    HANDLE h;
    _snwprintf(p, MAX_PATH, L"%ls\\nioh3fps_write_test.tmp", dir);
    h = CreateFileW(p, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, NULL);
    if (h == INVALID_HANDLE_VALUE) return 0;
    CloseHandle(h);
    return 1;
}

static void log_sink(const char *line, void *ctx)
{
    (void)ctx;
    if (!g_log) return;
    fputs(line, g_log);
    fputc('\n', g_log);
    g_log_bytes += strlen(line) + 1;
    if (g_log_bytes > LOG_ROTATE_BYTES) {
        fclose(g_log);
        MoveFileExW(g_log_path, g_log_old, MOVEFILE_REPLACE_EXISTING);
        g_log = _wfopen(g_log_path, L"w");
        g_log_bytes = 0;
        if (g_log) fputs("(log rotated; previous part in nioh3fps.log.old)\n", g_log);
    }
}

static void wlog(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void wlog(const char *fmt, ...)
{
    /* the writer's own lines go through the ring too, so ordering is preserved */
    char buf[480];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    log_printf("%s", buf);
}

/* ---------------- trace snapshots ---------------- */

static void dump_now(const char *why)
{
    SYSTEMTIME st;
    wchar_t p[MAX_PATH], name[96];
    char msg[160], fn[MAX_PATH * 3];
    GetLocalTime(&st);
    _snwprintf(name, 96, L"nioh3fps_trace_dump_%04u%02u%02u_%02u%02u%02u_%hs.csv",
               st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, why);
    out_path(p, name);
    trace_snapshot_csv(p, msg, sizeof msg);
    path_sanitize(p, fn, sizeof fn);
    wlog("TRACE DUMP (%s): %s: %s", why, fn, msg);
}

void writer_request_dump(const char *why)
{
    snprintf(g_dump_why, sizeof g_dump_why, "%s", why);
    InterlockedExchange(&g_dump_req, 1);
}

void writer_request_flush(void) { InterlockedExchange(&g_flush_req, 1); }

void writer_flush_wait(DWORD ms)
{
    if (!g_thread || GetCurrentThreadId() == GetThreadId(g_thread)) return;
    ResetEvent(g_evt_flushed);
    InterlockedExchange(&g_flush_req, 1);
    WaitForSingleObject(g_evt_flushed, ms);
}

/* ---------------- statistics from the flight recorder ---------------- */

typedef struct {
    uint64_t n, n_dt, clamp, hitch, pass, pr[7], fpschg, overlap, debtclamp, nanc, fpsv[4];
    double sum_dt, min_dt, max_dt, r_sum, ra_sum, lost, gained;
    float r_min, r_max;
} pstat;

typedef struct { char pat[16]; uint64_t n; } patc;

typedef struct {
    pstat p[2];
    uint64_t frames, h2hist[4], camhist[3], cam2hist[3], h2_during_h1, cam, cam2, xell, markers, records;
    patc pats[48]; int npats; uint64_t pat_other;
} window_t;

static window_t W;
static struct {
    uint64_t armed, waits, timeouts, backoffs, hb, other, null, repeat, hist[5], bypass;
    double sum_wait, max_wait;
} WG;
static int64_t h1_times[1024], previous_hb;
static unsigned h1_time_pos;
static uint32_t last_backoffs;
static struct { uint64_t frames, h2hist[4], ovl[2], lost_records, asserts; } S;   /* session totals */
static struct { int have; uint64_t h2, cam, cam2, xell; char pat[16]; int plen; } F;  /* current H1 frame */
static uint64_t g_rd, g_lost_records;
static DWORD g_rd_stall;
static int g_dumps; static DWORD g_last_assert_dump;
static uint64_t g_ovl_lines;

static void pstat_reset(pstat *p) { memset(p, 0, sizeof *p); p->min_dt = 1e9; p->r_min = 1e9f; p->r_max = -1e9f; }

static void window_reset(void)
{
    memset(&W, 0, sizeof W);
    memset(&WG,0,sizeof WG);
    pstat_reset(&W.p[0]);
    pstat_reset(&W.p[1]);
}

static void assert_dump(const char *what)
{
    DWORD now = GetTickCount();
    S.asserts++;
    if (g_dumps < 5 && (!g_last_assert_dump || now - g_last_assert_dump > 60000)) {
        g_dumps++;
        g_last_assert_dump = now;
        wlog("ASSERT: %s; dumping the flight recorder (%d/5)", what, g_dumps);
        dump_now("assert");
    }
}

static void pat_add(const char *p)
{
    for (int i = 0; i < W.npats; i++) if (!strcmp(W.pats[i].pat, p)) { W.pats[i].n++; return; }
    if (W.npats < 48) { snprintf(W.pats[W.npats].pat, 16, "%s", p); W.pats[W.npats++].n = 1; }
    else W.pat_other++;
}

static void frame_push(char c)
{
    if (F.plen < 14) F.pat[F.plen++] = c;
    else F.pat[14] = '+', F.plen = 15;
    F.pat[F.plen] = 0;
}

static void process_rec(const trace_rec *r)
{
    W.records++;
#ifndef RELEASE
    if (g_detail && g_ticks) {
        trace_hdr *h = trace_header();
        trace_csv_line(g_ticks, r, h->start_qpc, h->qpf, h->game_base);
    }
#endif
    switch (r->type) {
    case RT_H1: case RT_H2: {
        pstat *p = &W.p[r->type == RT_H1 ? 0 : 1];
        uint32_t f = r->flags;
        p->n++;
        if (!(f & (FF_FIRST | FF_DTNONPOS))) {
            p->n_dt++; p->sum_dt += r->dt;
            if (r->dt < p->min_dt) p->min_dt = r->dt;
            if (r->dt > p->max_dt) p->max_dt = r->dt;
        }
        p->r_sum += r->r_would; p->ra_sum += r->r_applied;
        if (r->r_would < p->r_min) p->r_min = r->r_would;
        if (r->r_would > p->r_max) p->r_max = r->r_would;
        if (f & FF_CLAMP) p->clamp++;
        if (f & FF_HITCH) p->hitch++;
        if (f & FF_NAN) p->nanc++;
        if (f & FF_DEBTCLAMP) p->debtclamp++;
        if (f & FF_FPSCHG) p->fpschg++;
        if (f & FF_PASS) {
            p->pass++;
            if (f & FF_FIRST) p->pr[0]++;
            if (f & FF_TOGGLE) p->pr[1]++;
            if (f & FF_FPSCHG) p->pr[2]++;
            if (f & FF_FPSNOT120) p->pr[3]++;
            if (f & FF_DTNONPOS) p->pr[4]++;
            if (f & FF_DISABLED) p->pr[5]++;
            if (f & FF_SAFE) p->pr[6]++;
        }
        p->fpsv[r->fps_a == 30 ? 0 : r->fps_a == 60 ? 1 : r->fps_a == 120 ? 2 : 3]++;
        p->lost = r->time_lost; p->gained = r->time_gained;
        if (r->type == RT_H1) {
            WG.armed+=r->enabled!=0;
            h1_times[h1_time_pos++%1024]=r->qpc;
            if (F.have) {
                W.frames++; S.frames++;
                W.h2hist[F.h2 > 3 ? 3 : F.h2]++; S.h2hist[F.h2 > 3 ? 3 : F.h2]++;
                W.camhist[F.cam > 2 ? 2 : F.cam]++;
                W.cam2hist[F.cam2 > 2 ? 2 : F.cam2]++;
                pat_add(F.pat);
                if (F.h2 >= 2) {
                    char m[96];
                    snprintf(m, sizeof m, "H2 entered %llu times between two H1 ticks (frame %llu)",
                             (unsigned long long)F.h2, (unsigned long long)r->frame - 1);
                    assert_dump(m);
                }
            }
            memset(&F, 0, sizeof F);
            F.have = 1;
            frame_push('1');
        } else {
            F.h2++;
            frame_push('2');
            if (r->aux2) W.h2_during_h1++;
        }
        break;
    }
    case RT_H1_OVERLAP: case RT_H2_OVERLAP: {
        int k = r->type == RT_H1_OVERLAP ? 0 : 1;
        W.p[k].overlap++; S.ovl[k]++;
        if (g_ovl_lines++ < 50)
            wlog("OVERLAP: %s entered while another worker was inside (tid %u, counter %u, frame %llu)",
                 k ? "H2" : "H1", r->tid, r->ovl, (unsigned long long)r->frame);
        assert_dump(k ? "H2 overlap" : "H1 overlap");
        break;
    }
    case RT_HB:
        if (r->flags&4) WG.null++;
        else if (r->flags&1) WG.other++;
        else {
            WG.hb++; WG.repeat+=!!(r->flags&2);
            if (previous_hb && r->qpc>previous_hb) {
                unsigned ticks=0;
                for (int j=0;j<1024;j++) if (h1_times[j]>previous_hb && h1_times[j]<=r->qpc) ticks++;
                WG.hist[ticks>4 ? 4 : ticks]++;
            }
            previous_hb=r->qpc;
        }
        break;
    case RT_GATE_END:
        WG.waits++; WG.sum_wait+=r->dt;
        if (r->dt>WG.max_wait) WG.max_wait=r->dt;
        WG.timeouts+=!!(r->flags&1);
        WG.backoffs+=r->aux2-last_backoffs; last_backoffs=r->aux2;
        break;
    case RT_BYPASS: WG.bypass++; break;
    case RT_CAM:  W.cam++;  F.cam++;  frame_push('C'); break;
    case RT_CAM2: W.cam2++; F.cam2++; frame_push('D'); break;
    case RT_XELL: W.xell++; F.xell++; frame_push('X'); break;
    case RT_MARKER: W.markers++; frame_push('M'); break;
    default: break;
    }
}

static void drain_records(void)
{
    trace_hdr *h = trace_header();
    trace_rec *recs = trace_records(), copy;
    uint64_t w;
    if (!h) return;
    w = (uint64_t)__atomic_load_n(&h->write_index, __ATOMIC_ACQUIRE);
    if (w - g_rd > TRACE_REC_COUNT) { g_lost_records += w - g_rd - TRACE_REC_COUNT; g_rd = w - TRACE_REC_COUNT; }
    while (g_rd < w) {
        trace_rec *r = &recs[g_rd & (TRACE_REC_COUNT - 1)];
        uint64_t s = __atomic_load_n(&r->seq, __ATOMIC_ACQUIRE);
        if (s == g_rd + 1) {
            memcpy(&copy, r, sizeof copy);
            __atomic_thread_fence(__ATOMIC_ACQUIRE);
            if (__atomic_load_n(&r->seq, __ATOMIC_ACQUIRE) == s && trace_rec_valid(&copy, g_rd & (TRACE_REC_COUNT - 1)))
                process_rec(&copy);
            else g_lost_records++;
            g_rd++; g_rd_stall = 0;
        } else if (s > g_rd + 1) {
            g_lost_records++; g_rd++;
        } else {
            DWORD now = GetTickCount();
            if (!g_rd_stall) g_rd_stall = now;
            if (now - g_rd_stall > 1000) { g_lost_records++; g_rd++; g_rd_stall = 0; continue; }
            break;
        }
    }
}

/* ---------------- probe counters / histograms ---------------- */

#ifndef RELEASE
typedef struct { uint64_t key; int slot; uint64_t cur, last; } hrow;
#define HMAP 4096
static hrow *g_hmap;
#endif
static uint64_t g_ptot[MAX_TARGETS], g_plast[MAX_TARGETS], g_pwin[MAX_TARGETS];
static double g_t0;

#ifndef RELEASE
static void hist_cb(int slot, uint64_t key, uint64_t cnt, void *ctx)
{
    (void)ctx;
    uint32_t h = (uint32_t)((key * 0x9E3779B97F4A7C15ull) >> 40) & (HMAP - 1);
    for (int k = 0; k < HMAP; k++) {
        hrow *e = &g_hmap[(h + (uint32_t)k) & (HMAP - 1)];
        if (e->key == key && e->slot == slot) { e->cur += cnt; return; }
        if (!e->key) { e->key = key; e->slot = slot; e->cur = cnt; return; }
    }
}
#endif

static void per_second(double t)
{
#ifdef RELEASE
    (void)t;
#endif
    LONG n = g_nprobes;
    probe_merge(g_ptot);
    for (LONG i = 0; i < n; i++) {
        uint64_t d = g_ptot[i] - g_plast[i];
#ifndef RELEASE
        if (d && g_detail && g_hist)
            fprintf(g_hist, "%.1f,%s,0x%llX,%s,,%llu\n", t, g_probes[i].name,
                    (unsigned long long)(g_probes[i].addr - g_base), g_probes[i].kind_s, (unsigned long long)d);
#endif
        g_pwin[i] += d;
        g_plast[i] = g_ptot[i];
    }
#ifndef RELEASE
    if (g_hmap) {
        for (int i = 0; i < HMAP; i++) g_hmap[i].cur = 0;
        probe_hist_merge(hist_cb, NULL);
        for (int i = 0; i < HMAP; i++) {
            hrow *e = &g_hmap[i];
            if (!e->key) continue;
            if (e->cur > e->last && g_detail && g_hist) {
                uintptr_t k = (uintptr_t)e->key;
                int in_exe = k >= g_base && k - g_base < 0x10000000;
                fprintf(g_hist, "%.1f,%s,0x%X,retaddr,0x%llX%s,%llu\n", t, e->slot ? "GetTimeStep" : "GetFps",
                        e->slot ? 0x46484u : 0x46594u, (unsigned long long)(in_exe ? k - g_base : k), in_exe ? "" : "(abs)",
                        (unsigned long long)(e->cur - e->last));
            }
            e->last = e->cur;
        }
    }
    if (g_hist) fflush(g_hist);
#endif
}

/* ---------------- summaries ---------------- */

static void summary(double secs)
{
    consumer_probe_summary(secs);
    static LONG64 last_polls, last_sleep;
    LONG64 polls=InterlockedCompareExchange64(&g_sleep_polls,0,0), sleep=InterlockedCompareExchange64(&g_sleep_qpc,0,0);
    wlog("GATE %.1fs: armed=%.2f%% waits=%llu mean/max wait=%.3f/%.3fms timeouts=%llu backoffs=%llu "
         "ticks/heartbeat 0/1/2/3/4+=%llu/%llu/%llu/%llu/%llu heartbeat:XELL=%.5f repeats:heartbeat=%.5f "
         "hb=%llu hb_other=%llu hb_null=%llu sleep1 polls=%lld ms=%.3f bypass-H1=%llu "
         "latched_mailbox=%p latched_tid=%lu relatches=%lld latched_share=%.5f",
         secs,W.p[0].n ? 100.0*WG.armed/W.p[0].n : 0,(unsigned long long)WG.waits,
         WG.waits ? 1e3*WG.sum_wait/WG.waits : 0,1e3*WG.max_wait,(unsigned long long)WG.timeouts,
         (unsigned long long)WG.backoffs,(unsigned long long)WG.hist[0],(unsigned long long)WG.hist[1],
         (unsigned long long)WG.hist[2],(unsigned long long)WG.hist[3],(unsigned long long)WG.hist[4],
         W.xell ? (double)WG.hb/W.xell : 0, WG.hb ? (double)WG.repeat/WG.hb : 0,
         (unsigned long long)WG.hb,(unsigned long long)WG.other,(unsigned long long)WG.null,(long long)(polls-last_polls),
         1e3*(double)(sleep-last_sleep)/g_qpf,(unsigned long long)WG.bypass,
         (void *)__atomic_load_n(&g_expected_mailbox,__ATOMIC_ACQUIRE),
         (unsigned long)__atomic_load_n(&g_render_tid,__ATOMIC_ACQUIRE),
         (long long)InterlockedCompareExchange64(&g_hb_relatches,0,0),
         WG.hb+WG.other ? (double)WG.hb/(WG.hb+WG.other) : 0);
    last_polls=polls; last_sleep=sleep;
    /* Keep publisher diagnostics on a companion GATE line: the text ring's
       fixed 496-byte slots cannot hold both summaries without truncation. */
    static uint64_t last_publish_calls, last_publish_owned;
    publish_diagnostics pd; lockstep_publish_snapshot(&pd);
    wlog("GATE %.1fs: publish calls=%llu owned=%llu tid0=%lu tls0=%d tid1=%lu tls1=%d "
         "tid2=%lu tls2=%d tid3=%lu tls3=%d mb=%p",
         secs,(unsigned long long)(pd.calls-last_publish_calls),(unsigned long long)(pd.owned-last_publish_owned),
         (unsigned long)pd.tid[0],pd.tls[0],(unsigned long)pd.tid[1],pd.tls[1],
         (unsigned long)pd.tid[2],pd.tls[2],(unsigned long)pd.tid[3],pd.tls[3],(void *)pd.mailbox);
    last_publish_calls=pd.calls; last_publish_owned=pd.owned;
    static const char *pn[2] = { "H1/P1", "H2/P2" };
    char line[480];
    for (int k = 0; k < 2; k++) {
        pstat *p = &W.p[k];
        double avg = p->n_dt && p->sum_dt > 0 ? (double)p->n_dt / p->sum_dt : 0;
        wlog("SUMMARY %.1fs %s: ticks=%llu real fps avg/min/max=%.1f/%.1f/%.1f r_would avg/min/max=%.4f/%.4f/%.4f r_applied avg=%.4f "
             "clamp=%llu hitch=%llu nan=%llu passthrough=%llu (first %llu toggle %llu fps-change %llu fps!=120 %llu dt<=0 %llu disabled %llu safe %llu) "
             "fps-mode changes=%llu overlaps=%llu debt-clamps=%llu time_lost=%.3fms time_gained=%.3fms game fps 30/60/120/other=%llu/%llu/%llu/%llu",
             secs, pn[k], (unsigned long long)p->n, avg, p->max_dt > 0 ? 1.0 / p->max_dt : 0, p->min_dt < 1e9 && p->min_dt > 0 ? 1.0 / p->min_dt : 0,
             p->n ? p->r_sum / (double)p->n : 0, p->n ? (double)p->r_min : 0, p->n ? (double)p->r_max : 0, p->n ? p->ra_sum / (double)p->n : 0,
             (unsigned long long)p->clamp, (unsigned long long)p->hitch, (unsigned long long)p->nanc, (unsigned long long)p->pass,
             (unsigned long long)p->pr[0], (unsigned long long)p->pr[1], (unsigned long long)p->pr[2], (unsigned long long)p->pr[3],
             (unsigned long long)p->pr[4], (unsigned long long)p->pr[5], (unsigned long long)p->pr[6],
             (unsigned long long)p->fpschg, (unsigned long long)p->overlap, (unsigned long long)p->debtclamp,
             p->lost * 1e3, p->gained * 1e3,
             (unsigned long long)p->fpsv[0], (unsigned long long)p->fpsv[1], (unsigned long long)p->fpsv[2], (unsigned long long)p->fpsv[3]);
    }
    {
        int used[48] = { 0 }, len = 0;
        len += snprintf(line + len, sizeof line - (size_t)len, "top patterns:");
        for (int t = 0; t < 6; t++) {
            int best = -1;
            for (int i = 0; i < W.npats; i++) if (!used[i] && (best < 0 || W.pats[i].n > W.pats[best].n)) best = i;
            if (best < 0) break;
            used[best] = 1;
            len += snprintf(line + len, sizeof line - (size_t)len, " %s=%llu", W.pats[best].pat, (unsigned long long)W.pats[best].n);
            if (len >= (int)sizeof line - 32) break;
        }
        wlog("ORDER %.1fs: frames=%llu H2/frame 0:%llu 1:%llu 2:%llu 3+:%llu | CAM(0x567070)/frame 0:%llu 1:%llu 2+:%llu | "
             "CAM2(0x20dda0c)/frame 0:%llu 1:%llu 2+:%llu | H2-while-H1-inside=%llu | events CAM=%llu CAM2=%llu xellSleep=%llu | %s (1=H1 2=H2 C=CAM D=CAM2 X=xell M=marker)",
             secs, (unsigned long long)W.frames, (unsigned long long)W.h2hist[0], (unsigned long long)W.h2hist[1],
             (unsigned long long)W.h2hist[2], (unsigned long long)W.h2hist[3],
             (unsigned long long)W.camhist[0], (unsigned long long)W.camhist[1], (unsigned long long)W.camhist[2],
             (unsigned long long)W.cam2hist[0], (unsigned long long)W.cam2hist[1], (unsigned long long)W.cam2hist[2],
             (unsigned long long)W.h2_during_h1, (unsigned long long)W.cam, (unsigned long long)W.cam2, (unsigned long long)W.xell, line);
    }
    {
        uint64_t leg = g_probe_legacy >= 0 ? g_pwin[g_probe_legacy] : 0;
        uint64_t cam = g_probe_cam >= 0 ? g_pwin[g_probe_cam] : 0, cam2 = g_probe_cam2 >= 0 ? g_pwin[g_probe_cam2] : 0;
        uint64_t gf = 0, gt = 0;
        for (LONG i = 0; i < g_nprobes; i++) {
            if (g_probes[i].kind == PK_HIST && g_probes[i].hist_slot == 0) gf = g_pwin[i];
            if (g_probes[i].kind == PK_HIST && g_probes[i].hist_slot == 1) gt = g_pwin[i];
        }
        wlog("PROBE %.1fs: legacy tick 0x5a6bb0 = %llu (%.2f/s, game-time accuracy) | entry counts 0x567070=%llu 0x20dda0c=%llu GetFps=%llu GetTimeStep=%llu | "
             "probes=%ld hist-overflow=%llu records=%llu lost=%llu",
             secs, (unsigned long long)leg, secs > 0 ? (double)leg / secs : 0, (unsigned long long)cam, (unsigned long long)cam2,
             (unsigned long long)gf, (unsigned long long)gt, (long)g_nprobes, (unsigned long long)probe_hist_overflow(),
             (unsigned long long)W.records, (unsigned long long)g_lost_records);
    }
    wlog("SESSION: frames=%llu H2/frame 0:%llu 1:%llu 2:%llu 3+:%llu overlaps H1/H2=%llu/%llu (interlocked totals %lld/%lld) assertions=%llu records lost=%llu",
         (unsigned long long)S.frames, (unsigned long long)S.h2hist[0], (unsigned long long)S.h2hist[1], (unsigned long long)S.h2hist[2],
         (unsigned long long)S.h2hist[3], (unsigned long long)S.ovl[0], (unsigned long long)S.ovl[1],
         (long long)g_ovl_h1_total, (long long)g_ovl_h2_total, (unsigned long long)S.asserts, (unsigned long long)g_lost_records);
    memset(g_pwin, 0, sizeof g_pwin);
    window_reset();
}

/* ---------------- hotkeys ---------------- */

static int game_foreground(void)
{
    HWND w = GetForegroundWindow();
    DWORD pid = 0;
    if (!w) return 0;
    GetWindowThreadProcessId(w, &pid);
    return pid == GetCurrentProcessId();
}

static int combo_down(const keycombo *k)
{
    int ctrl, shift, alt;
    if (!k->vk || !(GetAsyncKeyState(k->vk) & 0x8000)) return 0;
    ctrl = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
    shift = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
    alt = (GetAsyncKeyState(VK_MENU) & 0x8000) != 0;
    return ctrl == k->ctrl && shift == k->shift && alt == k->alt;
}

static void write_event(int type, uint64_t aux, uint32_t enabled)
{
    uint64_t idx;
    trace_rec *r;
    if (!trace_header()) return;
    r = trace_begin(&idx);
    r->type = (uint16_t)type;
    r->qpc = qpc_now();
    r->frame = (uint64_t)g_frame;
    r->tid = GetCurrentThreadId();
    r->aux = aux;
    r->enabled = enabled;
    r->marker = (uint32_t)g_marker;
    trace_end(r, idx);
}

static void poll_keys(void)
{
    static int prev_t, prev_d, prev_m;
    int fg = game_foreground();
    int t = fg && combo_down(&g_cfg.toggle_key);
    int d = fg && combo_down(&g_cfg.dump_key);
    int m = fg && combo_down(&g_cfg.marker_key);
    if (t && !prev_t && g_status_active) {
        LONG v = InterlockedXor(&g_toggle,1)^1;
        write_event(RT_TOGGLE, (uint64_t)v, (uint32_t)v);
        wlog("TOGGLE: %s (latched at the next H1/H2 entry; one passthrough tick at each point)", v ? "ON" : "OFF");
    }
    if (d && !prev_d) dump_now("key");
    if (m && !prev_m) {
        LONG n = InterlockedIncrement(&g_marker);
        write_event(RT_MARKER, (uint64_t)n, (uint32_t)g_toggle);
        wlog("MARKER %ld at frame %lld", (long)n, (long long)g_frame);
    }
    prev_t = t; prev_d = d; prev_m = m;
}

/* ---------------- thread ---------------- */

#ifndef RELEASE
static FILE *open_csv(const wchar_t *name, const wchar_t *old, const char *header)
{
    wchar_t p[MAX_PATH];
    FILE *f;
    rotate(name, old);
    out_path(p, name);
    f = _wfopen(p, L"w");
    if (f && header) fputs(header, f);
    return f;
}
#endif

static DWORD WINAPI writer_main(LPVOID arg)
{
    char msg[200];
    wchar_t prevbin[MAX_PATH], prevcsv[MAX_PATH];
    DWORD last_sec, last_sum, last_drain = 0;
    int64_t t_start;
    (void)arg;

    if (!dir_writable(g_outdir)) {
        wchar_t la[MAX_PATH];
        DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", la, MAX_PATH);
        if (n && n < MAX_PATH - 16) {
            _snwprintf(g_outdir, MAX_PATH, L"%ls\\nioh3fps", la);
            CreateDirectoryW(g_outdir, NULL);
        }
    }
    out_path(g_log_path, L"nioh3fps.log");
    out_path(g_log_old, L"nioh3fps.log.old");
    if (GetFileAttributesW(g_log_path) != INVALID_FILE_ATTRIBUTES) MoveFileExW(g_log_path, g_log_old, MOVEFILE_REPLACE_EXISTING);
    g_log = _wfopen(g_log_path, L"w");
    g_ready_ok = trace_open(g_outdir, msg, sizeof msg) == 0;
    wlog("%s", msg);
#ifndef RELEASE
    g_detail = PROBE_BUILD || g_cfg.log_level >= 1;
    if (g_detail) {
        g_ticks = open_csv(L"nioh3fps_ticks.csv", L"nioh3fps_ticks.csv.old", NULL);
        if (g_ticks) trace_csv_header(g_ticks);
        g_hist = open_csv(L"nioh3fps_hist.csv", L"nioh3fps_hist.csv.old", HIST_HEADER);
        g_hmap = VirtualAlloc(NULL, sizeof(hrow) * HMAP, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    }
#endif
    window_reset();
    SetEvent(g_evt_ready);

    out_path(prevbin, L"nioh3fps_trace.prev.bin");
    out_path(prevcsv, L"nioh3fps_trace.prev.csv");
    if (GetFileAttributesW(prevbin) != INVALID_FILE_ATTRIBUTES) {
        trace_file_to_csv(prevbin, prevcsv, msg, sizeof msg);
        wlog("previous session flight recorder -> nioh3fps_trace.prev.csv: %s", msg);
    }

    t_start = qpc_now();
    g_t0 = 0;
    last_sec = last_sum = GetTickCount();
    for (;;) {
        DWORD now;
        Sleep(10);
        now = GetTickCount();
        poll_keys();
        if (InterlockedExchange(&g_dump_req, 0)) dump_now(g_dump_why[0] ? g_dump_why : "request");
        if (now - last_drain >= 50 || g_flush_req) {
            last_drain = now;
            heartbeat_drain_trace();
            consumer_probe_drain();
            drain_records();
            if (now - last_sec >= 1000) {
                consumer_probe_second((double)(now-last_sec)/1000.0);
                per_second((double)(qpc_now() - t_start) / (double)g_qpf);
                last_sec += 1000;
                if (now - last_sec > 5000) last_sec = now;
            }
            if (now - last_sum >= 10000) {
                summary((double)(now - last_sum) / 1000.0);
                last_sum = now;
            }
            if (logring_drain(log_sink, NULL) && g_log) fflush(g_log);
#ifndef RELEASE
            if (g_ticks) {
                fflush(g_ticks);
                if (_ftelli64(g_ticks) > TICKS_ROTATE_BYTES) {
                    fclose(g_ticks);
                    g_ticks = open_csv(L"nioh3fps_ticks.csv", L"nioh3fps_ticks.csv.old", NULL);
                    if (g_ticks) trace_csv_header(g_ticks);
                    wlog("nioh3fps_ticks.csv rotated to .old at %lld MB", TICKS_ROTATE_BYTES >> 20);
                }
            }
            if (g_hist && _ftelli64(g_hist) > HIST_ROTATE_BYTES) {
                fclose(g_hist);
                g_hist = open_csv(L"nioh3fps_hist.csv", L"nioh3fps_hist.csv.old", HIST_HEADER);
                wlog("nioh3fps_hist.csv rotated to .old at %lld MB", HIST_ROTATE_BYTES >> 20);
            }
#endif
            if (InterlockedExchange(&g_flush_req, 0)) {
                if (g_log) fflush(g_log);
                SetEvent(g_evt_flushed);
            }
        }
    }
    return 0;
}

int writer_start(void)
{
    g_evt_ready = CreateEventW(NULL, TRUE, FALSE, NULL);
    g_evt_flushed = CreateEventW(NULL, TRUE, FALSE, NULL);
    g_thread = CreateThread(NULL, 0, writer_main, NULL, 0, NULL);
    if (!g_thread) return -1;
    WaitForSingleObject(g_evt_ready, 15000);
    return g_ready_ok ? 0 : -1;
}
