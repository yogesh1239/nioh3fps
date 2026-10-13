/* Crash capture (PLAN section 6): no UnhandledExceptionFilter. One vectored handler
   acts only when the faulting IP is inside our DLL, the stub pages or the MinHook
   trampolines. It signals a pre-created dump thread (MiniDumpWriteDump + log line),
   waits up to 10 s, then returns EXCEPTION_CONTINUE_SEARCH so the game's own
   handling still runs. */
#include "common.h"
#include <stdio.h>
#include <string.h>
#include <dbghelp.h>

typedef BOOL (WINAPI *MiniDumpWriteDump_t)(HANDLE, DWORD, HANDLE, MINIDUMP_TYPE,
                                           PMINIDUMP_EXCEPTION_INFORMATION, PMINIDUMP_USER_STREAM_INFORMATION,
                                           PMINIDUMP_CALLBACK_INFORMATION);

typedef struct { uintptr_t start, end; char name[24]; } range_t;

static range_t g_ranges[64];
static volatile LONG g_nranges;
static HANDLE g_evt_dump, g_evt_done, g_thread;
static DWORD g_dump_tid;
static volatile LONG g_once;
static EXCEPTION_POINTERS *volatile g_ep;
static volatile DWORD g_fault_tid;
static MiniDumpWriteDump_t g_mdwd;

void crash_add_range(uintptr_t start, size_t size, const char *name)
{
    LONG i;
    for (i = 0; i < g_nranges; i++)
        if (g_ranges[i].start == start && g_ranges[i].end == start + size) return;
    if (g_nranges >= 64) return;
    i = g_nranges;
    g_ranges[i].start = start;
    g_ranges[i].end = start + size;
    snprintf(g_ranges[i].name, sizeof g_ranges[i].name, "%s", name);
    MemoryBarrier();
    g_nranges = i + 1;
}

void crash_add_alloc_of(const void *p, const char *name)
{
    MEMORY_BASIC_INFORMATION mbi;
    if (p && VirtualQuery(p, &mbi, sizeof mbi))
        crash_add_range((uintptr_t)mbi.BaseAddress, mbi.RegionSize, name);
}

static const range_t *find_range(uintptr_t ip)
{
    for (LONG i = 0; i < g_nranges; i++)
        if (ip >= g_ranges[i].start && ip < g_ranges[i].end) return &g_ranges[i];
    return NULL;
}

static LONG CALLBACK veh(EXCEPTION_POINTERS *ep)
{
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    uintptr_t ip = (uintptr_t)ep->ContextRecord->Rip;
    if ((code & 0xF0000000u) != 0xC0000000u) return EXCEPTION_CONTINUE_SEARCH;
    if (!find_range(ip)) return EXCEPTION_CONTINUE_SEARCH;
    if (GetCurrentThreadId() == g_dump_tid) return EXCEPTION_CONTINUE_SEARCH;
    if (InterlockedCompareExchange(&g_once, 1, 0) != 0) return EXCEPTION_CONTINUE_SEARCH;
    g_ep = ep;
    g_fault_tid = GetCurrentThreadId();
    SetEvent(g_evt_dump);
    WaitForSingleObject(g_evt_done, 10000);
    return EXCEPTION_CONTINUE_SEARCH;
}

static DWORD WINAPI dump_thread(LPVOID arg)
{
    (void)arg;
    WaitForSingleObject(g_evt_dump, INFINITE);
    {
        EXCEPTION_POINTERS *ep = g_ep;
        uintptr_t ip = (uintptr_t)ep->ContextRecord->Rip;
        const range_t *r = find_range(ip);
        SYSTEMTIME st;
        wchar_t path[MAX_PATH];
        char where[96], fname[MAX_PATH * 3];
        HANDLE f;
        BOOL ok = FALSE;
        MINIDUMP_EXCEPTION_INFORMATION mei;
        GetLocalTime(&st);
        _snwprintf(path, MAX_PATH, L"%ls\\nioh3fps_crash_%04u%02u%02u_%02u%02u%02u.dmp", g_outdir,
                   st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
        if (r && r->start == (uintptr_t)g_self) snprintf(where, sizeof where, "nioh3fps(dinput8.dll)+0x%llx", (unsigned long long)(ip - r->start));
        else if (r) snprintf(where, sizeof where, "%s+0x%llx", r->name, (unsigned long long)(ip - r->start));
        else snprintf(where, sizeof where, "%p", (void *)ip);
        f = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (f != INVALID_HANDLE_VALUE && g_mdwd) {
            mei.ThreadId = g_fault_tid;
            mei.ExceptionPointers = ep;
            mei.ClientPointers = FALSE;
            ok = g_mdwd(GetCurrentProcess(), GetCurrentProcessId(), f,
                        (MINIDUMP_TYPE)(MiniDumpWithIndirectlyReferencedMemory | MiniDumpWithThreadInfo |
                                        MiniDumpWithUnloadedModules | MiniDumpWithProcessThreadData),
                        &mei, NULL, NULL);
        }
        if (f != INVALID_HANDLE_VALUE) CloseHandle(f);
        path_sanitize(path, fname, sizeof fname);
        log_printf("CRASH: exception 0x%08lX at %s (game base %p) thread %lu; minidump %s: %s",
                   ep->ExceptionRecord->ExceptionCode, where, (void *)g_base, (unsigned long)g_fault_tid,
                   ok ? "written" : "FAILED", fname);
        writer_request_dump("crash");
        writer_flush_wait(3000);
        SetEvent(g_evt_done);
    }
    return 0;
}

int crash_init(void)
{
    wchar_t sys[MAX_PATH];
    UINT n = GetSystemDirectoryW(sys, MAX_PATH);
    HMODULE dh = NULL;
    IMAGE_NT_HEADERS64 *nt = (IMAGE_NT_HEADERS64 *)((uint8_t *)g_self + ((IMAGE_DOS_HEADER *)g_self)->e_lfanew);
    if (n && n < MAX_PATH - 16) {
        wcscat(sys, L"\\dbghelp.dll");
        dh = LoadLibraryW(sys);
    }
    if (dh) g_mdwd = (MiniDumpWriteDump_t)(void *)GetProcAddress(dh, "MiniDumpWriteDump");
    crash_add_range((uintptr_t)g_self, nt->OptionalHeader.SizeOfImage, "nioh3fps");
    g_evt_dump = CreateEventW(NULL, FALSE, FALSE, NULL);
    g_evt_done = CreateEventW(NULL, TRUE, FALSE, NULL);
    g_thread = CreateThread(NULL, 256 * 1024, dump_thread, NULL, 0, &g_dump_tid);
    if (!g_evt_dump || !g_evt_done || !g_thread) return -1;
    if (!AddVectoredExceptionHandler(1, veh)) return -1;
    log_printf("CRASH: vectored handler installed (scoped to our pages), dump thread %lu, dbghelp %s",
               (unsigned long)g_dump_tid, g_mdwd ? "loaded" : "NOT available");
    return 0;
}
