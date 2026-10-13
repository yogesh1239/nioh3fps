#ifndef RELEASE
#include "common.h"
#include "toggle_file.h"
#include <wchar.h>
static DWORD WINAPI toggle_thread(void *arg)
{
    wchar_t path[MAX_PATH],*slash;
    toggle_file_edges edges={0}; (void)arg;
    GetModuleFileNameW(NULL,path,MAX_PATH);
    slash=wcsrchr(path,L'\\');
    if (!slash || (size_t)(slash-path)+20>=MAX_PATH) return 0;
    wcscpy(slash+1,L"nioh3fps.toggle");
    for (;;) {
        DWORD attrs=GetFileAttributesW(path),error=attrs==INVALID_FILE_ATTRIBUTES ? GetLastError() : 0;
        /* Access/AV errors are not disappearance edges. */
        if (attrs!=INVALID_FILE_ATTRIBUTES || error==ERROR_FILE_NOT_FOUND || error==ERROR_PATH_NOT_FOUND) {
            int state=toggle_file_edge(&edges,attrs!=INVALID_FILE_ATTRIBUTES && !(attrs&FILE_ATTRIBUTE_DIRECTORY));
            if (state>=0) {
                InterlockedExchange(&g_toggle,state); /* H1 already latches this at its tick boundary. */
                log_printf("TOGGLE file %s",state ? "ON" : "OFF");
            }
        }
        Sleep(1000);
    }
}
void toggle_file_start(void)
{
    if (!g_cfg.toggle_file) return;
    HANDLE h=CreateThread(NULL,0,toggle_thread,NULL,CREATE_SUSPENDED,NULL);
    if (!h) { log_printf("STATUS: ToggleFile PASSIVE (thread creation failed)"); return; }
    SetThreadPriority(h,THREAD_PRIORITY_LOWEST);
    ResumeThread(h); CloseHandle(h);
}
#endif
