/* Offline check of the entry-count target loader (probe_load_csv in src/probe.c) against the
   real image: maps the decrypted exe's sections into memory at their RVAs (a read-only copy,
   nothing executes), registers its .pdata so RtlLookupFunctionEntry sees it, adds the
   built-in GetFps/GetTimeStep histogram probes like init.c, then loads the CSV and prints
   every log line. Passes when the loader accepts every target the CSV marks hookable and
   the only non-accepted rows are the built-in duplicates.
   Usage: test_probecsv <exe> <probe_targets.csv> */
#include "../src/common.h"
#include "../src/expected.h"
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

/* stand-ins for the DLL globals probe.c references */
uintptr_t g_base;
volatile LONG g_marker;
volatile LONG64 g_frame, g_last_xell_qpc;
int64_t g_qpf;
static int g_lines, g_accepted = -1, g_dup = -1, g_skipped = -1;

void log_printf(const char *fmt, ...)
{
    char b[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    puts(b);
    g_lines++;
    sscanf(b, "PROBE CSV: %d targets accepted, %d already built-in, %d skipped", &g_accepted, &g_dup, &g_skipped);
}
void crash_add_alloc_of(const void *p, const char *name) { (void)p; (void)name; }
trace_rec *trace_begin(uint64_t *idx) { static trace_rec r; *idx = 0; return &r; }
void trace_end(trace_rec *r, uint64_t idx) { (void)r; (void)idx; }

int wmain(int argc, wchar_t **argv)
{
    FILE *fp;
    uint8_t *file, *img;
    long sz;
    IMAGE_NT_HEADERS64 *nt;
    IMAGE_SECTION_HEADER *sec;
    IMAGE_DATA_DIRECTORY pd;
    int rows = 0;
    char line[4096];
    if (argc < 3) return 2;
    fp = _wfopen(argv[1], L"rb");
    if (!fp) { printf("cannot open exe\n"); return 1; }
    fseek(fp, 0, SEEK_END); sz = ftell(fp); fseek(fp, 0, SEEK_SET);
    file = malloc((size_t)sz);
    if (fread(file, 1, (size_t)sz, fp) != (size_t)sz) return 1;
    fclose(fp);
    nt = (IMAGE_NT_HEADERS64 *)(file + ((IMAGE_DOS_HEADER *)file)->e_lfanew);
    img = VirtualAlloc(NULL, nt->OptionalHeader.SizeOfImage, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    memcpy(img, file, nt->OptionalHeader.SizeOfHeaders);
    sec = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; i++)
        memcpy(img + sec[i].VirtualAddress, file + sec[i].PointerToRawData,
               sec[i].SizeOfRawData < sec[i].Misc.VirtualSize ? sec[i].SizeOfRawData : sec[i].Misc.VirtualSize);
    pd = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
    if (!RtlAddFunctionTable((PRUNTIME_FUNCTION)(img + pd.VirtualAddress), pd.Size / sizeof(RUNTIME_FUNCTION), (DWORD64)(uintptr_t)img)) {
        printf("RtlAddFunctionTable failed\n");
        return 1;
    }
    g_base = (uintptr_t)img;

    if (probe_init()) { printf("probe_init failed\n"); return 1; }
    probe_add("GetFps", "retaddr-hist", "built-in", (uintptr_t)img + EXP_GETFPS_RVA, PK_HIST, 0);
    probe_add("GetTimeStep", "retaddr-hist", "built-in", (uintptr_t)img + EXP_GETTIMESTEP_RVA, PK_HIST, 1);

    fp = _wfopen(argv[2], L"rb");
    if (!fp) { printf("cannot open csv\n"); return 1; }
    while (fgets(line, sizeof line, fp)) if (line[0] && line[0] != '\r' && line[0] != '\n' && line[0] != '#') rows++;
    fclose(fp);
    rows--;   /* header */

    probe_load_csv(argv[2], (uintptr_t)img, nt->OptionalHeader.SizeOfImage);
    printf("csv rows %d: accepted %d, built-in %d, skipped %d\n", rows, g_accepted, g_dup, g_skipped);
    if (g_accepted >= 0 && g_skipped == 0 && g_accepted + g_dup == rows && g_dup == 2) {
        printf("TEST_PROBECSV: PASS\n");
        return 0;
    }
    printf("TEST_PROBECSV: FAIL\n");
    return 1;
}
