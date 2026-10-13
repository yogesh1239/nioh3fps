/* Lock-free MPSC text ring for log lines. Producers are our own threads (init, dump,
   writer); game threads never format text. Only the writer thread drains it. */
#include "common.h"
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <wchar.h>

#define LOG_SLOTS 512
#define LOG_TEXT  496

typedef struct {
    volatile LONG64 seq;      /* idx + 1 when complete, 0 while being written */
    char text[LOG_TEXT];
    char pad[512 - 8 - LOG_TEXT];
} logslot;

static logslot g_ring[LOG_SLOTS];
static volatile LONG64 g_widx;   /* next index to reserve */
static LONG64 g_ridx;            /* writer only */
static DWORD  g_stall_since;
static LONG64 g_lost;

void log_printf(const char *fmt, ...)
{
    SYSTEMTIME st;
    va_list ap;
    LONG64 idx = InterlockedIncrement64(&g_widx) - 1;
    logslot *s = &g_ring[idx % LOG_SLOTS];
    int n;
    InterlockedExchange64(&s->seq, 0);
    GetLocalTime(&st);
    n = snprintf(s->text, LOG_TEXT, "%02u:%02u:%02u.%03u ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    va_start(ap, fmt);
    vsnprintf(s->text + n, LOG_TEXT - (size_t)n, fmt, ap);
    va_end(ap);
    MemoryBarrier();
    InterlockedExchange64(&s->seq, idx + 1);
}

int logring_drain(log_sink_fn fn, void *ctx)
{
    char buf[LOG_TEXT + 64];
    int count = 0;
    for (;;) {
        LONG64 w = g_widx, s;
        logslot *sl;
        if (g_ridx >= w) break;
        if (w - g_ridx > LOG_SLOTS) {               /* producers lapped us */
            g_lost += w - g_ridx - LOG_SLOTS;
            g_ridx = w - LOG_SLOTS;
        }
        sl = &g_ring[g_ridx % LOG_SLOTS];
        s = sl->seq;
        if (s == g_ridx + 1) {
            memcpy(buf, sl->text, LOG_TEXT);
            buf[LOG_TEXT - 1] = 0;
            MemoryBarrier();
            if (sl->seq == s) { fn(buf, ctx); count++; }
            else g_lost++;
            g_ridx++;
            g_stall_since = 0;
        } else if (s > g_ridx + 1) {                /* overwritten by a later lap */
            g_lost++; g_ridx++;
        } else {                                    /* still being written */
            DWORD now = GetTickCount();
            if (!g_stall_since) g_stall_since = now;
            if (now - g_stall_since > 2000) { g_lost++; g_ridx++; g_stall_since = 0; continue; }
            break;
        }
    }
    if (g_lost) {
        snprintf(buf, sizeof buf, "LOG: %lld lines lost (ring overflow)", (long long)g_lost);
        fn(buf, ctx);
        g_lost = 0;
    }
    return count;
}

void path_sanitize(const wchar_t *in, char *out, size_t n)
{
    wchar_t prof[MAX_PATH], tmp[1024];
    DWORD k = GetEnvironmentVariableW(L"USERPROFILE", prof, MAX_PATH);
    size_t pl = (k > 0 && k < MAX_PATH) ? wcslen(prof) : 0;
    if (pl && !_wcsnicmp(in, prof, pl)) {
        _snwprintf(tmp, 1024, L"%%USERPROFILE%%%ls", in + pl);
        tmp[1023] = 0;
        in = tmp;
    }
    if (!WideCharToMultiByte(CP_UTF8, 0, in, -1, out, (int)n, NULL, NULL)) snprintf(out, n, "?");
}
