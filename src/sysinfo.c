/* Log header: mod/build, game exe, Windows, CPU; environment listing of the game folder. */
#include "common.h"
#include "expected.h"
#include <stdio.h>
#include <string.h>
#include <wchar.h>
#include <cpuid.h>

typedef LONG (WINAPI *RtlGetVersion_t)(OSVERSIONINFOW *);

static void exe_file_version(char *out, size_t n)
{
    HMODULE exe = GetModuleHandleW(NULL);
    HRSRC r = FindResourceW(exe, MAKEINTRESOURCEW(1), MAKEINTRESOURCEW(16) /* RT_VERSION */);
    HGLOBAL g = r ? LoadResource(exe, r) : NULL;
    const uint8_t *p = g ? LockResource(g) : NULL;
    DWORD sz = r ? SizeofResource(exe, r) : 0;
    snprintf(out, n, "unknown");
    for (DWORD i = 0; p && i + sizeof(VS_FIXEDFILEINFO) <= sz; i += 4) {
        const VS_FIXEDFILEINFO *ff = (const VS_FIXEDFILEINFO *)(p + i);
        if (ff->dwSignature == 0xFEEF04BD) {
            snprintf(out, n, "%lu.%lu.%lu.%lu", (unsigned long)HIWORD(ff->dwFileVersionMS), (unsigned long)LOWORD(ff->dwFileVersionMS),
                     (unsigned long)HIWORD(ff->dwFileVersionLS), (unsigned long)LOWORD(ff->dwFileVersionLS));
            break;
        }
    }
}

void sysinfo_log_header(void)
{
    char ver[64], cpu[49] = { 0 }, exe[MAX_PATH * 3], self[MAX_PATH * 3], out[MAX_PATH * 3];
    wchar_t w[MAX_PATH];
    OSVERSIONINFOW ov = { sizeof ov, 0, 0, 0, 0, { 0 } };
    RtlGetVersion_t rgv = (RtlGetVersion_t)(void *)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion");
    uintptr_t base = (uintptr_t)GetModuleHandleW(NULL);
    IMAGE_NT_HEADERS64 *nt = (IMAGE_NT_HEADERS64 *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
    unsigned int r[4];
    SYSTEM_INFO si;

    log_printf("==== nioh3fps %s build %s (%s %s) ====", MOD_VERSION, BUILD_ID, __DATE__, __TIME__);
    GetModuleFileNameW(g_self, w, MAX_PATH); path_sanitize(w, self, sizeof self);
    GetModuleFileNameW(NULL, w, MAX_PATH); path_sanitize(w, exe, sizeof exe);
    path_sanitize(g_outdir, out, sizeof out);
    log_printf("mod: %s", self);
    log_printf("output folder: %s", out);
    exe_file_version(ver, sizeof ver);
    log_printf("game: %s version %s, PE timestamp %08lX (expected %08X), SizeOfImage %08lX (unpacked reference %08X), base %p",
               exe, ver, (unsigned long)nt->FileHeader.TimeDateStamp, EXP_PE_TIMESTAMP,
               (unsigned long)nt->OptionalHeader.SizeOfImage, EXP_SIZE_OF_IMAGE, (void *)base);
    if (nt->FileHeader.TimeDateStamp != EXP_PE_TIMESTAMP)
        log_printf("WARNING: PE timestamp differs from the supported build; the AOB checks decide");
    if (rgv) rgv(&ov);
    log_printf("windows: %lu.%lu build %lu", (unsigned long)ov.dwMajorVersion, (unsigned long)ov.dwMinorVersion, (unsigned long)ov.dwBuildNumber);
    if (__get_cpuid_max(0x80000000, NULL) >= 0x80000004) {
        for (unsigned i = 0; i < 3; i++) {
            __cpuid(0x80000002 + i, r[0], r[1], r[2], r[3]);
            memcpy(cpu + 16 * i, r, 16);
        }
    }
    GetNativeSystemInfo(&si);
    log_printf("cpu: %s (%lu logical processors)", cpu[0] ? cpu : "unknown", (unsigned long)si.dwNumberOfProcessors);
    log_printf("qpc frequency: %lld Hz", (long long)g_qpf);
}

static int ends_with(const wchar_t *s, const wchar_t *suf)
{
    size_t a = wcslen(s), b = wcslen(suf);
    return a >= b && !_wcsicmp(s + a - b, suf);
}

void sysinfo_log_environment(const wchar_t *gamedir)
{
    wchar_t pat[MAX_PATH];
    WIN32_FIND_DATAW fd;
    HANDLE h;
    char name[MAX_PATH * 3], dir[MAX_PATH * 3];
    path_sanitize(gamedir, dir, sizeof dir);
    log_printf("environment: game folder %s", dir);
    _snwprintf(pat, MAX_PATH, L"%ls\\*", gamedir);
    h = FindFirstFileW(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
        WideCharToMultiByte(CP_UTF8, 0, fd.cFileName, -1, name, sizeof name, NULL, NULL);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            log_printf("environment:   [dir] %s", name);
        else if (ends_with(fd.cFileName, L".dll") || ends_with(fd.cFileName, L".asi") || ends_with(fd.cFileName, L".ini") ||
                 ends_with(fd.cFileName, L".exe") || ends_with(fd.cFileName, L".csv"))
            log_printf("environment:   %s %llu bytes", name,
                       ((unsigned long long)fd.nFileSizeHigh << 32) | fd.nFileSizeLow);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}
