/* Flight recorder: ~30 s ring of 128-byte records in a file-backed mapping
   (nioh3fps_trace.bin). Writers reserve a slot with an interlocked increment, clear
   seq, write the fields and the CRC32, then write seq last (seqlock). Dirty pages of a
   mapped file survive a process crash, so the previous session's trace is kept as
   nioh3fps_trace.prev.bin and converted to CSV at the next start. */
#include "common.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static trace_hdr *g_hdr;
static trace_rec *g_recs;
static uint32_t   g_crctab[256];

void crc32_init(void)
{
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        g_crctab[i] = c;
    }
}

uint32_t crc32_buf(const void *p, size_t n)
{
    const uint8_t *b = p;
    uint32_t c = 0xFFFFFFFFu;
    while (n--) c = g_crctab[(c ^ *b++) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

trace_hdr *trace_header(void) { return g_hdr; }
trace_rec *trace_records(void) { return g_recs; }

static void init_header(trace_hdr *h)
{
    FILETIME ft;
    memset(h, 0, TRACE_HDR_SIZE);
    memcpy(h->magic, "N3FPSTR1", 8);
    h->version = 1;
    h->header_size = TRACE_HDR_SIZE;
    h->rec_size = sizeof(trace_rec);
    h->rec_count = TRACE_REC_COUNT;
    h->qpf = g_qpf;
    h->start_qpc = qpc_now();
    h->game_base = (uint64_t)(uintptr_t)GetModuleHandleW(NULL);
    h->pid = GetCurrentProcessId();
    GetSystemTimeAsFileTime(&ft);
    h->start_filetime = ((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
    snprintf(h->build_id, sizeof h->build_id, "%s", BUILD_ID);
    snprintf(h->mod_version, sizeof h->mod_version, "%s", MOD_VERSION);
}

int trace_open(const wchar_t *dir, char *msg, size_t n)
{
    wchar_t cur[MAX_PATH], prev[MAX_PATH];
    size_t total = TRACE_HDR_SIZE + (size_t)TRACE_REC_COUNT * sizeof(trace_rec);
    HANDLE f, m;
    void *view = NULL;
    _snwprintf(cur, MAX_PATH, L"%ls\\nioh3fps_trace.bin", dir);
    _snwprintf(prev, MAX_PATH, L"%ls\\nioh3fps_trace.prev.bin", dir);
    if (GetFileAttributesW(cur) != INVALID_FILE_ATTRIBUTES)
        MoveFileExW(cur, prev, MOVEFILE_REPLACE_EXISTING);
    f = CreateFileW(cur, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f != INVALID_HANDLE_VALUE) {
        m = CreateFileMappingW(f, NULL, PAGE_READWRITE, 0, (DWORD)total, NULL);
        if (m) {
            view = MapViewOfFile(m, FILE_MAP_ALL_ACCESS, 0, 0, total);
            CloseHandle(m);
        }
        CloseHandle(f);
    }
    if (view) {
        snprintf(msg, n, "flight recorder: file-backed, %u records x %u B", TRACE_REC_COUNT, (unsigned)sizeof(trace_rec));
    } else {
        view = VirtualAlloc(NULL, total, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        snprintf(msg, n, "flight recorder: file mapping failed (error %lu), memory only", GetLastError());
        if (!view) return -1;
    }
    g_hdr = view;
    g_recs = (trace_rec *)((uint8_t *)view + TRACE_HDR_SIZE);
    /* Commit write faults during initialization, including mapped-file dirty
       faults. VirtualLock keeps these pages resident when the quota permits. */
    SYSTEM_INFO si; GetSystemInfo(&si);
    volatile uint8_t *pages=view;
    for (size_t off=0;off<total;off+=si.dwPageSize) pages[off]=0;
    BOOL locked=VirtualLock(view,total);
    DWORD lock_error=locked ? 0 : GetLastError();
    if (!locked) { /* fallback: touch every page again after the failed lock */
        for (size_t off=0;off<total;off+=si.dwPageSize) pages[off]=0;
    }
    size_t used=n ? strlen(msg) : 0;
    if (used<n) snprintf(msg+used,n-used,"; VirtualLock=%s (error %lu), prefaulted",
                         locked ? "OK" : "FAILED",(unsigned long)lock_error);
    init_header(g_hdr);
    return 0;
}

trace_rec *trace_begin(uint64_t *idx)
{
    uint64_t i = (uint64_t)__atomic_fetch_add(&g_hdr->write_index, 1, __ATOMIC_SEQ_CST);
    trace_rec *r = &g_recs[i & (TRACE_REC_COUNT - 1)];
    __atomic_store_n(&r->seq, 0, __ATOMIC_RELEASE);
    __atomic_signal_fence(__ATOMIC_SEQ_CST);
    /* explicit stores, not memset: this runs inside probe thunks, and the CRT memset may
       use AVX (vzeroupper would clobber the ymm upper halves of the hooked call's args) */
    for (size_t k = 1; k < sizeof *r / 8; k++) ((volatile uint64_t *)r)[k] = 0;
    *idx = i;
    return r;
}

void trace_end(trace_rec *r, uint64_t idx)
{
    r->crc = crc32_buf((const uint8_t *)r + 12, sizeof *r - 12);
    __atomic_thread_fence(__ATOMIC_RELEASE);
    __atomic_store_n(&r->seq, idx + 1, __ATOMIC_RELEASE);
}

int trace_rec_valid(const trace_rec *r, uint64_t slot)
{
    if (!r->seq || ((r->seq - 1) & (TRACE_REC_COUNT - 1)) != slot) return 0;
    return r->crc == crc32_buf((const uint8_t *)r + 12, sizeof *r - 12);
}

static const char *type_name(int t)
{
    static const char *n[] = { "?", "H1", "H1_OVERLAP", "H2", "H2_OVERLAP", "CAM", "CAM2", "MARKER", "TOGGLE", "XELL", "HB", "GATE_BEGIN", "GATE_END", "XELL_EXIT", "BYPASS" };
    return (t >= 1 && t <= 14) ? n[t] : "?";
}

void trace_csv_header(void *fp)
{
    fputs("seq,type,point,t_s,qpc,frame,tid,fps_a,fps_b,speed_req,speed_eff,r_would,r_applied,flags,"
          "dt_ms,debt_ms,time_lost_ms,time_gained_ms,xell_qpc,ovl,enabled,aux,aux2,marker,hb_seq,hb_packet,hb_mailbox,hb_repeat,hb_other,gate_seq,gate_wait_us,gate_timeout\n", (FILE *)fp);
}

void trace_csv_line(void *fp, const trace_rec *r, int64_t start_qpc, int64_t qpf, uint64_t base)
{
    (void)base;
    fprintf((FILE *)fp, "%llu,%s,%u,%.6f,%lld,%llu,%u,%d,%d,%.6g,%.6g,%.7g,%.7g,0x%04X,%.4f,%.4f,%.4f,%.4f,%lld,%u,%u,0x%llX,%u,%u,%llu,0x%llX,0x%llX,%u,%u,%llu,%.3f,%u\n",
            (unsigned long long)r->seq, type_name(r->type), r->point,
            qpf ? (double)(r->qpc - start_qpc) / (double)qpf : 0.0, (long long)r->qpc,
            (unsigned long long)r->frame, r->tid, r->fps_a, r->fps_b, (double)r->speed_req, (double)r->speed_eff,
            (double)r->r_would, (double)r->r_applied, r->flags, r->dt * 1e3, r->debt * 1e3,
            r->time_lost * 1e3, r->time_gained * 1e3, (long long)r->xell_qpc, r->ovl, r->enabled,
            (unsigned long long)r->aux, r->aux2, r->marker,
            (unsigned long long)(r->type==RT_HB ? r->frame : 0),
            (unsigned long long)(r->type==RT_HB ? r->aux : 0),
            (unsigned long long)(r->type==RT_HB ? (uint64_t)r->xell_qpc : 0),
            r->type==RT_HB ? !!(r->flags&2) : 0, r->type==RT_HB ? !!(r->flags&1) : 0,
            (unsigned long long)(r->type==RT_GATE_BEGIN || r->type==RT_GATE_END ? r->aux : 0),
            r->type==RT_GATE_END ? r->dt*1e6 : 0, r->type==RT_GATE_END ? !!(r->flags&1) : 0);
}

static int cmp_seq(const void *a, const void *b)
{
    uint64_t x = (*(const trace_rec *const *)a)->seq, y = (*(const trace_rec *const *)b)->seq;
    return x < y ? -1 : x > y;
}

/* write all valid records of an image (header + records) as CSV, ordered by seq */
static int image_to_csv(const uint8_t *img, size_t size, const wchar_t *csv, char *msg, size_t n)
{
    const trace_hdr *h = (const trace_hdr *)img;
    const trace_rec *recs;
    const trace_rec **v;
    FILE *fp;
    uint32_t cnt, nv = 0, bad = 0;
    if (size < TRACE_HDR_SIZE || memcmp(h->magic, "N3FPSTR1", 8) || h->rec_size != sizeof(trace_rec) ||
        !h->rec_count || (h->rec_count&(h->rec_count-1)) || h->rec_count>524288 ||
        size < TRACE_HDR_SIZE + (size_t)h->rec_count * h->rec_size) {
        snprintf(msg, n, "not a v1 trace image");
        return -1;
    }
    cnt = h->rec_count;
    recs = (const trace_rec *)(img + TRACE_HDR_SIZE);
    v = malloc(sizeof *v * cnt);
    if (!v) return -1;
    for (uint32_t i = 0; i < cnt; i++) {
        if (recs[i].seq && ((recs[i].seq-1)&(cnt-1))==i &&
            recs[i].crc==crc32_buf((const uint8_t *)&recs[i]+12,sizeof(trace_rec)-12)) v[nv++] = &recs[i];
        else if (recs[i].seq) bad++;
    }
    qsort(v, nv, sizeof *v, cmp_seq);
    fp = _wfopen(csv, L"w");
    if (!fp) { free(v); snprintf(msg, n, "cannot create CSV"); return -1; }
    fprintf(fp, "# nioh3fps trace build=%.64s version=%.32s pid=%u qpf=%lld write_index=%lld valid=%u dropped_torn=%u\n",
            h->build_id, h->mod_version, h->pid, (long long)h->qpf, (long long)h->write_index, nv, bad);
    trace_csv_header(fp);
    for (uint32_t i = 0; i < nv; i++) trace_csv_line(fp, v[i], h->start_qpc, h->qpf, h->game_base);
    fclose(fp);
    free(v);
    snprintf(msg, n, "%u valid records, %u torn/partial dropped", nv, bad);
    return 0;
}

int trace_file_to_csv(const wchar_t *bin, const wchar_t *csv, char *msg, size_t n)
{
    HANDLE f = CreateFileW(bin, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    LARGE_INTEGER sz;
    uint8_t *buf;
    DWORD rd = 0;
    int rc;
    if (f == INVALID_HANDLE_VALUE) { snprintf(msg, n, "no file"); return -1; }
    if (!GetFileSizeEx(f, &sz) || sz.QuadPart > 64 * 1024 * 1024) { CloseHandle(f); snprintf(msg, n, "bad size"); return -1; }
    buf = malloc((size_t)sz.QuadPart + 1);
    if (!buf || !ReadFile(f, buf, (DWORD)sz.QuadPart, &rd, NULL)) { CloseHandle(f); free(buf); snprintf(msg, n, "read failed"); return -1; }
    CloseHandle(f);
    rc = image_to_csv(buf, rd, csv, msg, n);
    free(buf);
    return rc;
}

int trace_snapshot_csv(const wchar_t *csv, char *msg, size_t n)
{
    size_t total = TRACE_HDR_SIZE + (size_t)TRACE_REC_COUNT * sizeof(trace_rec);
    uint8_t *buf = malloc(total);
    int rc;
    if (!buf) return -1;
    memcpy(buf, g_hdr, total);
    rc = image_to_csv(buf, total, csv, msg, n);
    free(buf);
    return rc;
}
