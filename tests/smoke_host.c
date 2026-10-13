/* Smoke test host (not the game): loads a copy of dinput8.dll from a scratch folder,
   calls the forwarded DirectInput8Create, lets the init and writer threads run for a few
   seconds and exits. The host exe has none of the game's bytes, so the expected result
   is a log with the header and config while readiness keeps polling (PASSIVE after the
   120 s timeout if the host lived that long). Usage: smoke_host <dir-with-dinput8.dll> [seconds] */
#include <windows.h>
#include <stdio.h>
#include <wchar.h>
#include <string.h>

typedef HRESULT (WINAPI *DI8Create_t)(HINSTANCE, DWORD, const GUID *, void **, void *);
static const GUID IID_IDirectInput8W_ = { 0xBF798031, 0x483A, 0x4DA2, { 0xAA, 0x99, 0x5D, 0x64, 0xED, 0x36, 0x97, 0x00 } };

int wmain(int argc, wchar_t **argv)
{
    wchar_t p[MAX_PATH];
    HMODULE m;
    DI8Create_t f;
    void *di = NULL;
    HRESULT hr;
    int secs = argc > 2 ? _wtoi(argv[2]) : 4;
    if (argc < 2) return 2;
    _snwprintf(p, MAX_PATH, L"%ls\\dinput8.dll", argv[1]);
    m = LoadLibraryW(p);
    if (!m) { printf("LoadLibrary failed %lu\n", GetLastError()); return 1; }
    f = (DI8Create_t)(void *)GetProcAddress(m, "DirectInput8Create");
    if (!f) { printf("no export\n"); return 1; }
    hr = f(GetModuleHandleW(NULL), 0x0800, &IID_IDirectInput8W_, &di, NULL);
    printf("DirectInput8Create -> 0x%08lX, %p\n", (unsigned long)hr, di);
    if (SUCCEEDED(hr) && di) {
        /* IUnknown::Release */
        ULONG (WINAPI *rel)(void *) = (ULONG (WINAPI *)(void *))(*(void ***)di)[2];
        rel(di);
    }
    Sleep((DWORD)secs * 1000);
    _snwprintf(p,MAX_PATH,L"%ls\\nioh3fps.log",argv[1]);
    FILE *log=_wfopen(p,L"rb");
    char buf[16384]={0};
    if (log) { size_t n=fread(buf,1,sizeof buf-1,log); buf[n]=0; fclose(log); }
    int passive=strstr(buf,"STATUS: PASSIVE")!=NULL;
    printf("non-game host PASSIVE: %s\n",passive ? "yes" : "no");
    _snwprintf(p, MAX_PATH, L"%ls\\dinput8.dll", argv[1]);
    printf("FreeLibrary (pinned, must stay loaded): %d\n", FreeLibrary(m));
    printf("module still loaded: %s\n", GetModuleHandleW(p) ? "yes" : "no");
    int ok=SUCCEEDED(hr) && passive && GetModuleHandleW(p)!=NULL;
    printf("TEST_SMOKE: %s\n",ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
