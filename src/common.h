/* Shared declarations for the nioh3fps DLL. C only; no globals need constructors. */
#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include "factor.h"
#include "stubs.h"

#define MOD_VERSION "0.3.0"
#ifndef BUILD_ID
#define BUILD_ID "dev"
#endif
/* Optional diagnostic build flag; S2L factors are controlled by the runtime gate. */
#ifndef PROBE_BUILD
#define PROBE_BUILD 0
#endif

/* ---------------- config (config.c) ---------------- */
typedef struct { int vk, ctrl, shift, alt; } keycombo;

typedef struct {
    int        enabled;        /* Enabled: 0 = DISABLED BY CONFIG (no hooks) */
    int        start_enabled;  /* StartEnabled: initial toggle state */
    int        lockstep, heartbeat_source; /* 0 take, 1 xell_entry, 2 xell_exit */
#ifndef RELEASE
    int        toggle_file;    /* debug-only background file edges; default 0 */
#endif
    keycombo   toggle_key, dump_key, marker_key;
    factor_cfg fc;             /* TimingMode, AvgFrames, SmoothK, ScaleMin, ScaleMax, SafeMode */
    int        log_level;      /* 0 = Info, 1 = Debug */
    int        ini_found;
} config_t;

extern config_t g_cfg;
void config_defaults(config_t *c);
void config_load(const wchar_t *path, config_t *c);
void config_log(const config_t *c);
const char *keycombo_str(const keycombo *k, char *buf, size_t n);

/* ---------------- globals (init.c) ---------------- */
extern HMODULE        g_self;
extern uintptr_t      g_base;            /* game exe base */
extern int64_t        g_qpf;
extern wchar_t        g_dlldir[MAX_PATH]; /* folder of this DLL (ini, probe csv) */
extern wchar_t        g_outdir[MAX_PATH]; /* folder for logs/traces (dll dir or %LOCALAPPDATA%\nioh3fps) */
extern volatile LONG  g_toggle;          /* runtime enabled state (F10), latched at hook entry */
extern volatile LONG  g_marker;          /* last F9 marker number */
extern volatile LONG64 g_frame;          /* H1 entry counter */
extern volatile LONG64 g_last_xell_qpc;
extern stubs_layout   g_stubs;
extern int            g_status_active;
extern uintptr_t      g_clock_global;    /* G = base + rva derived from the GetFps operand */

#ifdef LOCKSTEP_TEST
int64_t test_qpc_now(void);
static inline int64_t qpc_now(void) { return test_qpc_now(); }
#else
static inline int64_t qpc_now(void) { LARGE_INTEGER t; QueryPerformanceCounter(&t); return t.QuadPart; }
#endif

/* ---------------- log ring (logring.c) ---------------- */
void log_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
typedef void (*log_sink_fn)(const char *line, void *ctx);
int  logring_drain(log_sink_fn fn, void *ctx);   /* writer thread only */
void path_sanitize(const wchar_t *in, char *out, size_t n);  /* UTF-8, %USERPROFILE% replaced */

/* ---------------- flight recorder (trace.c) ---------------- */
enum {
    RT_H1 = 1, RT_H1_OVERLAP = 2, RT_H2 = 3, RT_H2_OVERLAP = 4,
    RT_CAM = 5, RT_CAM2 = 6, RT_MARKER = 7, RT_TOGGLE = 8, RT_XELL = 9,
    RT_HB = 10, RT_GATE_BEGIN = 11, RT_GATE_END = 12, RT_XELL_EXIT = 13,
    RT_BYPASS = 14
};

#pragma pack(push, 1)
typedef struct {
    volatile uint64_t seq;   /* 0: idx + 1, written last (seqlock) */
    uint32_t crc;            /* 8: CRC32 of bytes 12..127 */
    uint16_t type;           /* 12 */
    uint16_t point;          /* 14: 1 = P1/H1, 2 = P2/H2 */
    int64_t  qpc;            /* 16 */
    uint64_t frame;          /* 24: H1 frame counter */
    uint32_t tid;            /* 32 */
    int32_t  fps_a;          /* 36: H1 clk+0x618 / H2 S+0x10 */
    int32_t  fps_b;          /* 40: H1 clk+0x614 / H2 -1 */
    float    speed_req;      /* 44: H1 clk+0x628 / H2 S+0x14 */
    float    speed_eff;      /* 48: H1 clk+0x624 / H2 S+0x18 */
    float    r_would;        /* 52 */
    float    r_applied;      /* 56 */
    uint32_t flags;          /* 60: FF_* */
    double   dt;             /* 64 */
    double   debt;           /* 72 */
    double   time_lost;      /* 80 */
    double   time_gained;    /* 88 */
    int64_t  xell_qpc;       /* 96 */
    uint32_t ovl;            /* 104: overlap counter at entry */
    uint32_t enabled;        /* 108: latched toggle */
    uint64_t aux;            /* 112: H1 rcx==*G; CAM/CAM2/XELL ret RVA; MARKER no.; TOGGLE state */
    uint32_t aux2;           /* 120: H2: H1 counter at entry (H1 inside) */
    uint32_t marker;         /* 124: last marker number */
} trace_rec;

typedef struct {
    char     magic[8];       /* "N3FPSTR1" */
    uint32_t version;        /* 1 */
    uint32_t header_size;    /* 4096 */
    uint32_t rec_size;       /* 128 */
    uint32_t rec_count;      /* power of two */
    int64_t  qpf;
    int64_t  start_qpc;
    uint64_t game_base;
    uint32_t pid;
    uint32_t reserved0;
    uint64_t start_filetime;
    char     build_id[64];
    char     mod_version[32];
    uint8_t  pad[96];
    volatile int64_t write_index;   /* offset 256, own cache line */
} trace_hdr;
#pragma pack(pop)

_Static_assert(sizeof(trace_rec) == 128, "trace_rec must be 128 bytes");
_Static_assert(__builtin_offsetof(trace_hdr, write_index) == 256, "write_index at 256");
#define TRACE_HDR_SIZE  4096
#define TRACE_REC_COUNT 32768      /* 4 MB: H1/H2 + HB + gate + camera + xell entry/exit */

int        trace_open(const wchar_t *dir, char *msg, size_t n);  /* writer thread */
trace_rec *trace_begin(uint64_t *idx);
void       trace_end(trace_rec *r, uint64_t idx);
trace_hdr *trace_header(void);
trace_rec *trace_records(void);
uint32_t   crc32_buf(const void *p, size_t n);
void       crc32_init(void);
int        trace_rec_valid(const trace_rec *r, uint64_t idx);
int        trace_file_to_csv(const wchar_t *bin, const wchar_t *csv, char *msg, size_t n);
int        trace_snapshot_csv(const wchar_t *csv, char *msg, size_t n);
void       trace_csv_header(void *fp);
void       trace_csv_line(void *fp, const trace_rec *r, int64_t start_qpc, int64_t qpf, uint64_t base);

/* ---------------- writer thread (writer.c) ---------------- */
int  writer_start(void);           /* opens outputs, waits until ready */
void writer_request_flush(void);
void writer_request_dump(const char *why);
void writer_flush_wait(DWORD ms);

/* ---------------- probes (probe.c) ---------------- */
enum { PK_COUNT = 0, PK_HIST = 1, PK_ORDER = 2, PK_XELL = 3 };
#define MAX_TARGETS 512
#define HIST_SLOTS  512             /* probe_thunk.S hard-codes 512 (HIST_MASK) */
#define N_CTHUNKS   4
typedef struct probe_ctx {
    void      *tramp;        /* MinHook trampoline (copied into *tramp_slot) */
    uint32_t   id, kind;
    uint32_t   hist_slot;    /* PK_HIST: 0 = GetFps, 1 = GetTimeStep */
    uint32_t   rec_type;     /* PK_ORDER: RT_CAM / RT_CAM2 */
    uintptr_t  addr;         /* hooked address */
    uintptr_t  match_ret;    /* PK_XELL: only this return address is timestamped */
    void      *entry;        /* the thunk MinHook jumps to */
    volatile uint64_t *count;/* entry counter (incremented by the thunk) */
    void     **tramp_slot;   /* read by the thunk's final jmp [rip+...] */
    char       name[48];
    char       kind_s[24];
    char       source[24];
} probe_ctx;

/* one 64-byte line per static thunk (probe_thunk.S): lines 0-1 hist, 2.. C thunks */
typedef struct { volatile uint64_t count; void *tramp; probe_ctx *ctx; uint8_t pad[40]; } probe_sline;
_Static_assert(sizeof(probe_sline) == 64, "probe_sline must be one cache line");
typedef struct { volatile uint64_t key, count; } probe_hent;
extern probe_sline g_sline[2 + N_CTHUNKS];
extern probe_hent  g_probe_hist[2][HIST_SLOTS];
extern volatile uint64_t g_probe_hist_ovf[2];

extern probe_ctx  *g_probes;
extern volatile LONG g_nprobes;
int        probe_init(void);
probe_ctx *probe_add(const char *name, const char *kind_s, const char *source, uintptr_t addr, uint32_t kind, uint32_t hist_slot);
void      *probe_stub_for(probe_ctx *c);
void       probe_set_tramp(probe_ctx *c);   /* after MH_CreateHook filled c->tramp */
int        probe_finalize_stubs(void);
int        probe_find(uintptr_t addr);
#ifndef RELEASE
int        probe_load_csv(const wchar_t *path, uintptr_t base, uint32_t size_of_image);
void       probe_reserve(const uintptr_t *addr, int n);
#endif
/* writer side */
void       probe_merge(uint64_t *totals /* [MAX_TARGETS] */);
typedef void (*hist_fn)(int slot, uint64_t key, uint64_t count, void *ctx);
void       probe_hist_merge(hist_fn fn, void *ctx);
uint64_t   probe_hist_overflow(void);
int        probe_legacy_id(void);
extern int g_probe_legacy, g_probe_cam, g_probe_cam2;

/* ---------------- hooks (hooks.c) ---------------- */
uint64_t h1_detour(uint8_t *clk, void *a2, void *a3, void *a4);
uint64_t h2_detour(void *self, uint8_t *S, void *a3, void *a4);
extern void *h1_tramp, *h2_tramp;
extern volatile LONG64 g_ovl_h1_total, g_ovl_h2_total;

/* ---------------- crash capture (crash.c) ---------------- */
int  crash_init(void);
void crash_add_range(uintptr_t start, size_t size, const char *name);
void crash_add_alloc_of(const void *p, const char *name);

/* ---------------- AOB (aob.c) ---------------- */
typedef struct { uint8_t b[128]; uint8_t m[128]; int n; } aob_t;
int aob_parse(const char *s, aob_t *a);
int aob_match_at(const uint8_t *p, const aob_t *a);
int aob_scan(const uint8_t *start, size_t len, const aob_t *a, uintptr_t *hits, int maxhits);

/* ---------------- sysinfo (sysinfo.c) ---------------- */
void sysinfo_log_header(void);
void sysinfo_log_environment(const wchar_t *gamedir);
