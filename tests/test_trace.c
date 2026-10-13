/* Flight-recorder test (src/trace.c): writes records through trace_begin/trace_end into a
   file-backed trace in the given folder, wrapping the ring, then tears one record (CRC no
   longer matches) and converts the file with the DLL's own converter. Checks the counts
   and that every surviving record is valid and in its slot. The Makefile then converts the
   same file with tools/trace2csv.py and requires an identical CSV.
   Usage: test_trace <scratch-dir>  (writes nioh3fps_trace.bin and test_trace_c.csv there) */
#include "../src/common.h"
#include <stdio.h>
#include <string.h>
#include <wchar.h>

int64_t g_qpf;

static int g_fail, g_checks;
#define CHECK(c, ...) do { g_checks++; if (!(c)) { g_fail++; printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); } } while (0)

int wmain(int argc, wchar_t **argv)
{
    wchar_t bin[MAX_PATH], csv[MAX_PATH];
    char msg[160];
    LARGE_INTEGER f;
    const uint64_t total = TRACE_REC_COUNT + 1000;
    unsigned valid = 0;
    if (argc < 2) return 2;
    QueryPerformanceFrequency(&f);
    g_qpf = f.QuadPart;
    crc32_init();
    CHECK(trace_open(argv[1], msg, sizeof msg) == 0, "trace_open: %s", msg);
    CHECK(strstr(msg, "file-backed") != NULL, "file-backed mapping: %s", msg);
    CHECK(strstr(msg,"VirtualLock=") && strstr(msg,"prefaulted"),"residency status: %s",msg);
    for (uint64_t i = 0; i < total; i++) {
        uint64_t idx;
        trace_rec *r = trace_begin(&idx);
        CHECK(idx == i, "index %llu", (unsigned long long)i);
        r->type = (uint16_t)(1 + i % 14);
        r->point = (uint16_t)(1 + i % 2);
        r->qpc = trace_header()->start_qpc + (int64_t)i * 1000;
        r->frame = i / 2;
        r->tid = 1234;
        r->fps_a = 120; r->fps_b = (int32_t)(i % 7) - 1;
        r->speed_req = 1.0f; r->speed_eff = 1.0f + (float)(i % 100) / 300.0f;
        r->r_would = 1.0f / 3.0f + (float)i * 1e-6f; r->r_applied = 1.0f;
        r->flags = (uint32_t)(i & 0xFF);
        r->dt = 1.0 / 120.0 + (double)(i % 13) * 1e-5;
        r->debt = -(double)(i % 5) * 1e-4; r->time_lost = 1e-3; r->time_gained = 2e-3;
        r->xell_qpc = (int64_t)i * 3; r->ovl = (uint32_t)(i % 3); r->enabled = 1;
        r->aux = 0x5ad6b2ULL + i; r->aux2 = (uint32_t)i; r->marker = (uint32_t)(i / 1000);
        trace_end(r, idx);
    }
    for (uint32_t s = 0; s < TRACE_REC_COUNT; s++) valid += trace_rec_valid(&trace_records()[s], s);
    CHECK(valid == TRACE_REC_COUNT, "all slots valid after wrap: %u", valid);
    CHECK(trace_records()[999].seq == total, "slot 999 holds the last record");
    CHECK(trace_records()[1000].seq == 1001,
          "slot 1000 holds the oldest surviving record (seq %llu)", (unsigned long long)trace_records()[1000].seq);
    /* tear one record: a field changed after the CRC was written */
    trace_records()[5].speed_eff += 1.0f;
    CHECK(!trace_rec_valid(&trace_records()[5], 5), "torn record detected");
    CHECK(!trace_rec_valid(&trace_records()[6], 7), "record in the wrong slot rejected");
    FlushViewOfFile(trace_header(), 0);

    _snwprintf(bin, MAX_PATH, L"%ls\\nioh3fps_trace.bin", argv[1]);
    _snwprintf(csv, MAX_PATH, L"%ls\\test_trace_c.csv", argv[1]);
    CHECK(trace_file_to_csv(bin, csv, msg, sizeof msg) == 0, "convert: %s", msg);
    {
        char want[80];
        snprintf(want, sizeof want, "%u valid records, 1 torn/partial dropped", TRACE_REC_COUNT - 1);
        CHECK(!strcmp(msg, want), "converter counts: '%s'", msg);
    }
    /* S1's 16384-record v1 traces remain readable after S2L doubles capacity. */
    wchar_t legacy[MAX_PATH],legacycsv[MAX_PATH];
    _snwprintf(legacy,MAX_PATH,L"%ls\\legacy.bin",argv[1]);
    _snwprintf(legacycsv,MAX_PATH,L"%ls\\legacy_c.csv",argv[1]);
    size_t legacy_size=TRACE_HDR_SIZE+16384*sizeof(trace_rec);
    uint8_t *legacy_img=malloc(legacy_size);
    memcpy(legacy_img,trace_header(),legacy_size);
    ((trace_hdr *)legacy_img)->rec_count=16384;
    FILE *lf=_wfopen(legacy,L"wb");
    CHECK(lf!=NULL,"legacy fixture");
    if (lf) { CHECK(fwrite(legacy_img,1,legacy_size,lf)==legacy_size,"legacy write"); fclose(lf); }
    CHECK(trace_file_to_csv(legacy,legacycsv,msg,sizeof msg)==0,"convert S1-sized trace: %s",msg);
    free(legacy_img);
    printf("%d checks, %d failures\n", g_checks, g_fail);
    printf(g_fail ? "TEST_TRACE: FAIL\n" : "TEST_TRACE: PASS\n");
    return g_fail ? 1 : 0;
}
