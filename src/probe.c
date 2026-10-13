/* S1 probes: entry counts, return-address histograms (GetFps / GetTimeStep) and
   ordering records (camera paths, xellSleep).

   Entry-count targets have unknown signatures (xmm or stack arguments, non-standard leaf
   conventions), so their detour is a generated 16-byte thunk that touches no register
   and no stack:
       F0 48 FF 05 rel32     lock inc qword [rip -> line_i.count]
       FF 25 rel32           jmp qword [rip -> line_i.tramp]
       CC CC
   Only rflags changes. The thunks share one UNWIND_INFO with no codes (a leaf: rsp is
   unchanged, the return address is at [rsp]), registered with RtlAddFunctionTable.
   Allocation: code 0x0000-0x1FFF (16 B per probe id), UNWIND_INFO at 0x2000,
   RUNTIME_FUNCTION table at 0x2010 (RX after finalize); data from 0x4000, one 64-byte
   line per probe id {count, tramp} (RW).
   Histogram and C-calling probes use the static thunks in probe_thunk.S. */
#include "common.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#define THUNK_STRIDE 16
#define THUNK_LEN    14
#define UNWIND_OFF   0x2000
#define RFTAB_OFF    0x2010
#define CODE_SIZE    0x4000
#define DATA_OFF     CODE_SIZE
#define LINE_SIZE    64
#define ALLOC_SIZE   (DATA_OFF + MAX_TARGETS * LINE_SIZE)
_Static_assert(MAX_TARGETS * THUNK_STRIDE <= UNWIND_OFF, "thunks overlap unwind info");
_Static_assert(RFTAB_OFF + MAX_TARGETS * 12 <= CODE_SIZE, "function table too large");

typedef struct { volatile uint64_t count; void *tramp; uint8_t pad[48]; } dline;

probe_ctx     *g_probes;
volatile LONG  g_nprobes;
int g_probe_legacy = -1, g_probe_cam = -1, g_probe_cam2 = -1;

__attribute__((aligned(64))) probe_sline g_sline[2 + N_CTHUNKS];
__attribute__((aligned(64))) probe_hent  g_probe_hist[2][HIST_SLOTS];
__attribute__((aligned(64))) volatile uint64_t g_probe_hist_ovf[2];

static uint8_t *g_mem;           /* generated thunks + unwind info + table + data lines */
static int      g_ncthunks;
static int      g_table_added;

extern void probe_hist_thunk0(void), probe_hist_thunk1(void);
extern void probe_cthunk0(void), probe_cthunk1(void), probe_cthunk2(void), probe_cthunk3(void);
static void (*const g_cthunk[N_CTHUNKS])(void) = { probe_cthunk0, probe_cthunk1, probe_cthunk2, probe_cthunk3 };
void probe_hit(probe_ctx *c, uintptr_t ret);

uint8_t *probe_thunk_mem(void) { return g_mem; }

int probe_init(void)
{
    g_probes = VirtualAlloc(NULL, sizeof(probe_ctx) * MAX_TARGETS, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    g_mem = VirtualAlloc(NULL, ALLOC_SIZE, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!g_probes || !g_mem) return -1;
    memset(g_mem, 0xCC, CODE_SIZE);
    crash_add_alloc_of(g_mem, "probe thunks");
    return 0;
}

/* Called from probe_cthunkN (probe_thunk.S) on game threads; every volatile register is
   saved there. Must not run AVX code (it would clobber ymm upper halves), hence plain C
   built without -mavx and no CRT memset/memcpy on this path (see trace_begin). */
void probe_hit(probe_ctx *c, uintptr_t ret)
{
    DWORD le = GetLastError();
    switch (c->kind) {
    case PK_ORDER: {
        uint64_t idx;
        trace_rec *r = trace_begin(&idx);
        r->type = (uint16_t)c->rec_type;
        r->qpc = qpc_now();
        r->frame = (uint64_t)g_frame;
        r->tid = GetCurrentThreadId();
        r->aux = ret - g_base;
        r->xell_qpc = g_last_xell_qpc;
        r->marker = (uint32_t)g_marker;
        trace_end(r, idx);
        break;
    }
    case PK_XELL:
        if (ret == c->match_ret) {
            uint64_t idx;
            int64_t q = qpc_now();
            trace_rec *r;
            InterlockedExchange64(&g_last_xell_qpc, q);
            r = trace_begin(&idx);
            r->type = RT_XELL;
            r->qpc = q;
            r->frame = (uint64_t)g_frame;
            r->tid = GetCurrentThreadId();
            r->aux = ret - g_base;
            r->xell_qpc = q;
            r->marker = (uint32_t)g_marker;
            trace_end(r, idx);
        }
        break;
    default:
        break;
    }
    SetLastError(le);
}

static void put_rel32(uint8_t *at, const uint8_t *next_ip, const volatile void *target)
{
    int32_t d = (int32_t)((const volatile uint8_t *)target - next_ip);
    memcpy(at, &d, 4);
}

probe_ctx *probe_add(const char *name, const char *kind_s, const char *source, uintptr_t addr, uint32_t kind, uint32_t hist_slot)
{
    LONG id = g_nprobes;
    probe_ctx *c;
    if (id >= MAX_TARGETS) return NULL;
    c = &g_probes[id];
    memset(c, 0, sizeof *c);
    c->id = (uint32_t)id;
    c->kind = kind;
    c->addr = addr;
    c->hist_slot = hist_slot;
    snprintf(c->name, sizeof c->name, "%s", name);
    snprintf(c->kind_s, sizeof c->kind_s, "%s", kind_s);
    snprintf(c->source, sizeof c->source, "%s", source);
    if (kind == PK_COUNT) {
        uint8_t *t = g_mem + (size_t)id * THUNK_STRIDE;
        dline *l = (dline *)(g_mem + DATA_OFF + (size_t)id * LINE_SIZE);
        t[0] = 0xF0; t[1] = 0x48; t[2] = 0xFF; t[3] = 0x05; put_rel32(t + 4, t + 8, &l->count);   /* lock inc qword [rip+x] */
        t[8] = 0xFF; t[9] = 0x25; put_rel32(t + 10, t + 14, &l->tramp);                          /* jmp qword [rip+x] */
        c->entry = t;
        c->count = &l->count;
        c->tramp_slot = &l->tramp;
    } else if (kind == PK_HIST) {
        if (hist_slot > 1 || g_sline[hist_slot].ctx) return NULL;
        c->entry = (void *)(hist_slot ? probe_hist_thunk1 : probe_hist_thunk0);
        c->count = &g_sline[hist_slot].count;
        c->tramp_slot = &g_sline[hist_slot].tramp;
        g_sline[hist_slot].ctx = c;
    } else {
        probe_sline *l;
        if (g_ncthunks >= N_CTHUNKS) return NULL;
        l = &g_sline[2 + g_ncthunks];
        c->entry = (void *)g_cthunk[g_ncthunks++];
        c->count = &l->count;
        c->tramp_slot = &l->tramp;
        l->ctx = c;
    }
    g_nprobes = id + 1;
    return c;
}

void *probe_stub_for(probe_ctx *c) { return c->entry; }

/* the thunk's final jmp reads the slot; set before MH_EnableHook */
void probe_set_tramp(probe_ctx *c) { *c->tramp_slot = c->tramp; }

/* Writes the shared leaf UNWIND_INFO and the sorted function table of the generated
   thunks, makes the code pages RX and registers the table. 0, -1 VirtualProtect,
   -2 RtlAddFunctionTable. */
int probe_finalize_stubs(void)
{
    DWORD old, n = 0;
    uint8_t *u = g_mem + UNWIND_OFF;
    RUNTIME_FUNCTION *rf = (RUNTIME_FUNCTION *)(g_mem + RFTAB_OFF);
    u[0] = 1; u[1] = 0; u[2] = 0; u[3] = 0;     /* version 1, no flags, prolog 0, 0 codes, no frame register */
    for (LONG i = 0; i < g_nprobes; i++) {
        if (g_probes[i].kind != PK_COUNT) continue;
        rf[n].BeginAddress = (DWORD)((size_t)i * THUNK_STRIDE);
        rf[n].EndAddress = rf[n].BeginAddress + THUNK_LEN;
        rf[n].UnwindData = UNWIND_OFF;
        n++;
    }
    if (!VirtualProtect(g_mem, CODE_SIZE, PAGE_EXECUTE_READ, &old)) return -1;
    FlushInstructionCache(GetCurrentProcess(), g_mem, CODE_SIZE);
    if (n && !g_table_added) {
        if (!RtlAddFunctionTable(rf, n, (DWORD64)(uintptr_t)g_mem)) return -2;
        g_table_added = 1;
    }
    return 0;
}

int probe_find(uintptr_t addr)
{
    for (LONG i = 0; i < g_nprobes; i++) if (g_probes[i].addr == addr) return (int)i;
    return -1;
}

/* ---------------- writer side ---------------- */

void probe_merge(uint64_t *totals)
{
    LONG n = g_nprobes;
    memset(totals, 0, sizeof(uint64_t) * MAX_TARGETS);
    for (LONG i = 0; i < n; i++) totals[i] = g_probes[i].count ? *g_probes[i].count : 0;
}

void probe_hist_merge(hist_fn fn, void *ctx)
{
    for (int s = 0; s < 2; s++)
        for (int i = 0; i < HIST_SLOTS; i++) {
            uint64_t k = g_probe_hist[s][i].key;
            if (k) fn(s, k, g_probe_hist[s][i].count, ctx);
        }
}

uint64_t probe_hist_overflow(void) { return g_probe_hist_ovf[0] + g_probe_hist_ovf[1]; }

int probe_legacy_id(void) { return g_probe_legacy; }

/* ---------------- nioh3fps_probe.csv ---------------- */
#ifndef RELEASE
/* columns (by header name, any order): rva, kind, name, source_finding, prologue_bytes_hex,
   hookable, hook_rva, aob, aob_offset, notes. The aob must match at hook_rva + aob_offset
   (docs/s05/README.md); prologue_bytes_hex, when present, must equal the bytes at hook_rva. */

static uintptr_t g_reserved[16];
static int g_nreserved;
void probe_reserve(const uintptr_t *addr,int n)
{
    g_nreserved=n<16 ? n : 16;
    memcpy(g_reserved,addr,(size_t)g_nreserved*sizeof *addr);
}

static int csv_split(char *line, char **f, int maxf)
{
    int n = 0;
    char *p = line, *w;
    while (n < maxf) {
        f[n++] = w = p;
        if (*p == '"') {
            p++;
            for (;;) {
                if (!*p) break;
                if (*p == '"') { if (p[1] == '"') { *w++ = '"'; p += 2; continue; } p++; break; }
                *w++ = *p++;
            }
            while (*p && *p != ',') p++;
        } else {
            while (*p && *p != ',') *w++ = *p++;
        }
        if (*p == ',') { *w = 0; p++; continue; }
        *w = 0;
        break;
    }
    return n;
}

static void trim(char *s)
{
    char *e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1])) *--e = 0;
    while (*s && isspace((unsigned char)*s)) memmove(s, s + 1, strlen(s));
}

static int parse_hex(const char *s, uint64_t *v)
{
    char *end;
    if (!*s) return 0;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s += 2;
    *v = strtoull(s, &end, 16);
    return end != s && !*end;
}

static int truthy(const char *s)
{
    return !_stricmp(s, "1") || !_stricmp(s, "yes") || !_stricmp(s, "true") || !_stricmp(s, "y");
}

int probe_load_csv(const wchar_t *path, uintptr_t base, uint32_t size_of_image)
{
    FILE *fp = _wfopen(path, L"rb");
    char line[4096], *f[16];
    int col[10], ln = 0, added = 0, skipped = 0, dup = 0, have_header = 0;
    static const char *names[10] = { "rva", "kind", "name", "source_finding", "prologue_bytes_hex", "hookable", "hook_rva", "aob", "notes", "aob_offset" };
    IMAGE_NT_HEADERS64 *nt = (IMAGE_NT_HEADERS64 *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
    IMAGE_SECTION_HEADER *sec = IMAGE_FIRST_SECTION(nt);
    uintptr_t ts=0,te=0;
    for (int j=0;j<nt->FileHeader.NumberOfSections;j++) if (!memcmp(sec[j].Name,".text",6)) {
        ts=base+sec[j].VirtualAddress; te=ts+sec[j].Misc.VirtualSize;
    }
    (void)size_of_image;
    if (!ts) { if (fp) fclose(fp); log_printf("PROBE CSV: no .text; passive"); return 0; }
    if (!fp) { log_printf("PROBE CSV: nioh3fps_probe.csv not found; only built-in probes"); return 0; }
    while (fgets(line, sizeof line, fp)) {
        int nf;
        char *v[10];
        uint64_t rva = 0, hrva = 0, aoff = 0;
        uintptr_t addr;
        aob_t a;
        char why[160] = "";
        ln++;
        line[strcspn(line, "\r\n")] = 0;
        if (ln == 1 && (uint8_t)line[0] == 0xEF && (uint8_t)line[1] == 0xBB && (uint8_t)line[2] == 0xBF) memmove(line, line + 3, strlen(line + 3) + 1);
        if (!line[0] || line[0] == '#') continue;
        nf = csv_split(line, f, 16);
        for (int i = 0; i < nf; i++) trim(f[i]);
        if (!have_header) {
            for (int k = 0; k < 10; k++) {
                col[k] = -1;
                for (int i = 0; i < nf; i++) if (!_stricmp(f[i], names[k])) col[k] = i;
            }
            if (col[0] < 0 || col[5] < 0 || col[6] < 0 || col[7] < 0) {
                log_printf("PROBE CSV: header lacks rva/hookable/hook_rva/aob columns; file ignored");
                fclose(fp);
                return 0;
            }
            have_header = 1;
            continue;
        }
        for (int k = 0; k < 10; k++) v[k] = (col[k] >= 0 && col[k] < nf) ? f[col[k]] : (char *)"";
        if (!parse_hex(v[0], &rva)) snprintf(why, sizeof why, "bad rva '%s'", v[0]);
        else if (!truthy(v[5])) snprintf(why, sizeof why, "hookable=%s", v[5]);
        else if (!parse_hex(v[6][0] ? v[6] : v[0], &hrva)) snprintf(why, sizeof why, "bad hook_rva '%s'", v[6]);
        else if (v[9][0] && (!parse_hex(v[9], &aoff) || aoff > 0x10000)) snprintf(why, sizeof why, "bad aob_offset '%s'", v[9]);
        else {
            aob_t pb;
            addr = base + (uintptr_t)hrva;
            if (v[7][0]) {
                if (aob_parse(v[7], &a)) snprintf(why, sizeof why, "unparsable aob");
            } else if (v[4][0] && hrva == rva && !aoff) {
                if (aob_parse(v[4], &a)) snprintf(why, sizeof why, "unparsable prologue_bytes_hex");
            } else snprintf(why, sizeof why, "no aob");
            if (!why[0] && a.n < 5) snprintf(why, sizeof why, "aob shorter than 5 bytes");
            if (!why[0]) {
                uintptr_t hit=0;
                int nh=aob_scan((void *)ts,(size_t)(te-ts),&a,&hit,1);
                if (nh!=1) snprintf(why,sizeof why,"AOB has %d matches (requires unique)",nh);
                else {
                    addr=hit-(uintptr_t)aoff;
                    if (addr<ts || addr+5>te) snprintf(why,sizeof why,"decoded hook outside .text");
                }
            }
            if (!why[0] && v[4][0]) {
                if (aob_parse(v[4], &pb) || pb.n < 5) snprintf(why, sizeof why, "unparsable prologue_bytes_hex");
                else if (addr + (uintptr_t)pb.n > te || !aob_match_at((const uint8_t *)addr, &pb)) snprintf(why, sizeof why, "prologue_bytes_hex mismatch at hook_rva");
            }
            if (!why[0]) {
                /* a target the built-in probes already hook (GetFps/GetTimeStep histograms) */
                int same = -1;
                for (LONG i = 0; i < g_nprobes; i++) if (g_probes[i].addr == addr) same = (int)i;
                if (same >= 0) {
                    dup++;
                    log_printf("PROBE CSV line %d (%s): already hooked by built-in probe %s (%s)", ln, v[2][0] ? v[2] : v[0],
                               g_probes[same].name, v[1]);
                    continue;
                }
            }
            if (!why[0]) {
                DWORD64 ib = 0;
                PRUNTIME_FUNCTION fe = RtlLookupFunctionEntry((DWORD64)addr, &ib, NULL);
                if (fe && ib + fe->BeginAddress != addr) snprintf(why, sizeof why, "hook_rva is not a function start (.pdata begins at %#lx)", (unsigned long)fe->BeginAddress);
            }
            if (!why[0]) {
                for (LONG i = 0; i < g_nprobes; i++)
                    if (addr + 5 > g_probes[i].addr && g_probes[i].addr + 5 > addr) {
                        snprintf(why, sizeof why, "overlaps probe %s", g_probes[i].name);
                        break;
                    }
            }
            if (!why[0]) {
                for (int i=0;i<g_nreserved;i++)
                    if (g_reserved[i] && addr+5>g_reserved[i] && g_reserved[i]+17>addr)
                        snprintf(why,sizeof why,"overlaps a core/lockstep hook");
            }
            if (!why[0]) {
                char nm[48];
                snprintf(nm, sizeof nm, "%s", v[2][0] ? v[2] : v[0]);
                if (!probe_add(nm, v[1][0] ? v[1] : "count", v[3], addr, PK_COUNT, 0)) snprintf(why, sizeof why, "too many targets (max %d)", MAX_TARGETS);
            }
        }
        if (why[0]) { skipped++; log_printf("PROBE CSV line %d (%s): skipped: %s", ln, v[2][0] ? v[2] : v[0], why); }
        else added++;
    }
    fclose(fp);
    log_printf("PROBE CSV: %d targets accepted, %d already built-in, %d skipped", added, dup, skipped);
    return added;
}
#endif /* !RELEASE: optional probe CSV loader */
