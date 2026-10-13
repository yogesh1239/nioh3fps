/* dinput8.dll proxy. DllMain pins the module and starts the init thread, nothing else.
   Each export lazily loads the system dinput8.dll on first call (never in DllMain)
   and fails with E_FAIL (DIERR_GENERIC) / NULL if that is impossible. */
#include "common.h"
#include <wchar.h>

void init_start(HMODULE self);

static HMODULE volatile g_real;

static FARPROC real_proc(const char *name)
{
    HMODULE m = g_real;
    if (!m) {
        wchar_t p[MAX_PATH + 16];
        UINT n = GetSystemDirectoryW(p, MAX_PATH);
        if (!n || n >= MAX_PATH) return NULL;
        wcscat(p, L"\\dinput8.dll");
        m = LoadLibraryW(p);
        if (!m) return NULL;
        if (InterlockedCompareExchangePointer((PVOID volatile *)&g_real, m, NULL) != NULL) {
            FreeLibrary(m);
            m = g_real;
        }
    }
    return GetProcAddress(m, name);
}

typedef HRESULT (WINAPI *DI8Create_t)(HINSTANCE, DWORD, REFIID, LPVOID *, void *);
typedef HRESULT (WINAPI *DllGetClassObject_t)(REFCLSID, REFIID, LPVOID *);
typedef HRESULT (WINAPI *HrVoid_t)(void);
typedef const void *(WINAPI *GetdfDIJoystick_t)(void);

HRESULT WINAPI Proxy_DI8Create(HINSTANCE h, DWORD ver, REFIID riid, LPVOID *out, void * outer)
{
    DI8Create_t f = (DI8Create_t)(void *)real_proc("DirectInput8Create");
    if (!f) { if (out) *out = NULL; return E_FAIL; }
    return f(h, ver, riid, out, outer);
}

HRESULT WINAPI Proxy_DllCanUnloadNow(void)
{
    HrVoid_t f = (HrVoid_t)(void *)real_proc("DllCanUnloadNow");
    return f ? f() : E_FAIL;
}

HRESULT WINAPI Proxy_DllGetClassObject(REFCLSID clsid, REFIID riid, LPVOID *out)
{
    DllGetClassObject_t f = (DllGetClassObject_t)(void *)real_proc("DllGetClassObject");
    if (!f) { if (out) *out = NULL; return E_FAIL; }
    return f(clsid, riid, out);
}

HRESULT WINAPI Proxy_DllRegisterServer(void)
{
    HrVoid_t f = (HrVoid_t)(void *)real_proc("DllRegisterServer");
    return f ? f() : E_FAIL;
}

HRESULT WINAPI Proxy_DllUnregisterServer(void)
{
    HrVoid_t f = (HrVoid_t)(void *)real_proc("DllUnregisterServer");
    return f ? f() : E_FAIL;
}

const void *WINAPI Proxy_GetdfDIJoystick(void)
{
    GetdfDIJoystick_t f = (GetdfDIJoystick_t)(void *)real_proc("GetdfDIJoystick");
    return f ? f() : NULL;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        HMODULE pinned;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_PIN | GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, (LPCWSTR)(void *)DllMain, &pinned);
        init_start(inst);
    }
    return TRUE;
}
