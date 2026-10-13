/* S2L installer: unique AOBs and decoded operands, per-feature fail-passive. */
#include "common.h"
#include "resolve.h"
#include "lockstep.h"
#include "consumer_probe.h"
#include "consumer_fix.h"
#include "toggle_file.h"
#include "MinHook.h"
#include <stdio.h>
#include <string.h>
#include <wchar.h>
HMODULE g_self;
uintptr_t g_base, g_clock_global;
int64_t g_qpf;
wchar_t g_dlldir[MAX_PATH], g_outdir[MAX_PATH];
volatile LONG g_toggle, g_marker;
volatile LONG64 g_frame, g_last_xell_qpc;
stubs_layout g_stubs;
int g_status_active;
void hooks_init_state(void);
static size_t image_size;
static void passive(const char *why) { log_printf("STATUS: PASSIVE (%s)",why); writer_request_flush(); }
static int readable(uintptr_t p,size_t n)
{
    MEMORY_BASIC_INFORMATION m;
    return VirtualQuery((void *)p,&m,sizeof m) && m.State==MEM_COMMIT &&
        !(m.Protect&(PAGE_GUARD|PAGE_NOACCESS)) && p+n<= (uintptr_t)m.BaseAddress+m.RegionSize;
}
static int text_readable(void)
{
    IMAGE_NT_HEADERS64 *nt=(void *)(g_base+((IMAGE_DOS_HEADER *)g_base)->e_lfanew);
    IMAGE_SECTION_HEADER *s=IMAGE_FIRST_SECTION(nt);
    for (int i=0;i<nt->FileHeader.NumberOfSections;i++) if (!memcmp(s[i].Name,".text",6)) {
        uintptr_t at=g_base+s[i].VirtualAddress, end=at+s[i].Misc.VirtualSize;
        while (at<end) {
            MEMORY_BASIC_INFORMATION m;
            if (!VirtualQuery((void *)at,&m,sizeof m) || m.State!=MEM_COMMIT ||
                (m.Protect&(PAGE_GUARD|PAGE_NOACCESS)) ||
                !(m.Protect&(PAGE_EXECUTE|PAGE_EXECUTE_READ|PAGE_EXECUTE_READWRITE|PAGE_EXECUTE_WRITECOPY))) return 0;
            at=(uintptr_t)m.BaseAddress+m.RegionSize;
        }
        return 1;
    }
    return 0;
}
static int wait_ready(void)
{
    DWORD start=GetTickCount(), last=0;
    wchar_t host[MAX_PATH], *name;
    GetModuleFileNameW(NULL,host,MAX_PATH); name=wcsrchr(host,L'\\'); name=name ? name+1 : host;
    for (;;) {
        DWORD now=GetTickCount();
        if (!last || now-last>=1000) {
            last=now;
            if (text_readable()) {
                resolve_image((void *)g_base,image_size,&g_resolved);
                if (g_resolved.core) {
                    log_printf("READY: unique decrypted core after %lu ms",(unsigned long)(now-start)); return 1;
                }
            }
            /* A non-game smoke host needs no SteamStub decryption budget. */
            if (_wcsicmp(name,L"Nioh3.exe")) return 1;
        }
        if (now-start>=120000) { log_printf("READY: 120 s timeout; unresolved features will remain passive"); return 1; }
        Sleep(50);
    }
}
static const uint8_t *host_unwind(uintptr_t site,char *why,size_t n)
{
    IMAGE_NT_HEADERS64 *nt=(void *)(g_base+((IMAGE_DOS_HEADER *)g_base)->e_lfanew);
    IMAGE_DATA_DIRECTORY d=nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
    if (d.VirtualAddress>=image_size || d.Size>image_size-d.VirtualAddress) return NULL;
    const RUNTIME_FUNCTION *rf=(void *)(g_base+d.VirtualAddress);
    for (size_t i=0;i<d.Size/sizeof *rf;i++) if (g_base+rf[i].BeginAddress<=site && site<g_base+rf[i].EndAddress) {
        if (rf[i].UnwindData>=image_size || image_size-rf[i].UnwindData<64) break;
        const uint8_t *ui=(void *)(g_base+rf[i].UnwindData); uint8_t dst[64]; size_t len;
        if (stubs_unwind_len(ui)>sizeof dst || stubs_transform_unwind(ui,dst,&len,why,n)) return NULL;
        log_printf("UNWIND site RVA 0x%llX: host 0x%lX..0x%lX metadata 0x%lX",
            (unsigned long long)(site-g_base),(unsigned long)rf[i].BeginAddress,(unsigned long)rf[i].EndAddress,(unsigned long)rf[i].UnwindData);
        return ui;
    }
    snprintf(why,n,"no supported .pdata entry for matched site"); return NULL;
}
static uint8_t *alloc_near(uintptr_t base,size_t n)
{
    uintptr_t a=(base+image_size+0xffff)&~(uintptr_t)0xffff;
    for (;a<base+0x70000000;a+=0x10000) {
        MEMORY_BASIC_INFORMATION m;
        if (!VirtualQuery((void *)a,&m,sizeof m)) break;
        if (m.State==MEM_FREE) { uint8_t *p=VirtualAlloc((void *)a,n,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE); if (p) return p; }
    }
    return NULL;
}
static int finalize(uint8_t *a,RUNTIME_FUNCTION *t,DWORD count)
{
    DWORD old;
    if (!VirtualProtect(a,STUB_PAGE,PAGE_EXECUTE_READ,&old)) return 0;
    FlushInstructionCache(GetCurrentProcess(),a,STUB_PAGE);
    if (!RtlAddFunctionTable(t,count,(DWORD64)(uintptr_t)a)) return 0;
    crash_add_range((uintptr_t)a,STUB_PAGE,"stub"); return 1;
}
static int hook(const char *name,uintptr_t target,void *detour,void **tramp)
{
    MH_STATUS st=MH_CreateHook((void *)target,detour,tramp);
    log_printf("HOOK %s RVA 0x%llX: %s",name,(unsigned long long)(target-g_base),MH_StatusToString(st));
    if (st==MH_OK && tramp && *tramp) crash_add_alloc_of(*tramp,"minhook");
    return st==MH_OK;
}
static int midhook(int sleep,char *why,size_t n)
{
    uintptr_t site=g_resolved.hook[sleep ? A_SLEEP : A_TAKE];
    const uint8_t *ui=host_unwind(site,why,n);
    uint8_t *a=ui ? alloc_near(g_base,STUB_PAGE) : NULL;
    RUNTIME_FUNCTION *t; void *unused=NULL;
    if (!a) return 0;
    if ((sleep ? sleep1_build(a,site,ui,&t,why,n) : heartbeat_build(a,site,ui,&t,why,n))) return 0;
    if (!finalize(a,t,1)) return 0;
    return hook(sleep ? "Sleep(1) call only" : "packet take",site,a,&unused);
}
static void builtin(int anchor,const char *name,uint32_t kind,uint32_t slot,uint32_t type,int *id)
{
    if (!g_resolved.hook[anchor]) { log_printf("STATUS: PROBE %s PASSIVE (anchor unavailable)",name); return; }
    probe_ctx *p=probe_add(name,kind==PK_ORDER ? "order" : kind==PK_HIST ? "retaddr-hist" : "count",
        "built-in",g_resolved.hook[anchor],kind,slot);
    if (p) { p->rec_type=type; if (id) *id=(int)p->id; }
}
static DWORD WINAPI init_thread(LPVOID arg)
{
    wchar_t ini[MAX_PATH],*slash; char why[256]="setup failed";
#ifndef RELEASE
    wchar_t csv[MAX_PATH];
#endif
    stubs_params sp={0}; void *unused=NULL; LARGE_INTEGER freq; (void)arg;
    QueryPerformanceFrequency(&freq); g_qpf=freq.QuadPart;
    g_base=(uintptr_t)GetModuleHandleW(NULL);
    IMAGE_NT_HEADERS64 *nt=(void *)(g_base+((IMAGE_DOS_HEADER *)g_base)->e_lfanew);
    image_size=nt->OptionalHeader.SizeOfImage;
    crc32_init(); GetModuleFileNameW(g_self,g_dlldir,MAX_PATH);
    slash=wcsrchr(g_dlldir,L'\\'); if (slash) *slash=0; wcscpy(g_outdir,g_dlldir);
    _snwprintf(ini,MAX_PATH,L"%ls\\nioh3fps.ini",g_dlldir);
#ifndef RELEASE
    _snwprintf(csv,MAX_PATH,L"%ls\\nioh3fps_probe.csv",g_dlldir);
#endif
    config_load(ini,&g_cfg); g_toggle=g_cfg.start_enabled; g_heartbeat_source=g_cfg.heartbeat_source;
    if (writer_start()) return 0;
    sysinfo_log_header(); config_log(&g_cfg); sysinfo_log_environment(g_dlldir);
    if (!g_cfg.enabled) { log_printf("STATUS: DISABLED BY CONFIG"); return 0; }
    if (crash_init()) log_printf("CRASH: handler setup unavailable");
    if (!wait_ready()) return 0;
    for (int i=0;i<A_COUNT;i++) log_printf("AOB %s: hits=%d RVA=0x%llX (hint 0x%X) crc32=%08X",resolve_name(i),g_resolved.hits[i],
        (unsigned long long)(g_resolved.match[i] ? g_resolved.match[i]-g_base : 0),
        resolve_hint(i),
        g_resolved.match[i] ? crc32_buf((void *)g_resolved.match[i],resolve_span(i)) : 0);
    log_printf("DECODE: G=0x%llX scene=0x%llX help_jobs=0x%llX fps=%X actual_fps=%X req=%X eff=%X pause=%X/%X S=%X/%X/%X",
        (unsigned long long)(g_resolved.clock_global ? g_resolved.clock_global-g_base : 0),
        (unsigned long long)(g_resolved.scene_global ? g_resolved.scene_global-g_base : 0),
        (unsigned long long)(g_resolved.help_jobs ? g_resolved.help_jobs-g_base : 0),
        g_resolved.fps,g_resolved.actual_fps,g_resolved.req,g_resolved.eff,g_resolved.pause_a,g_resolved.pause_b,
        g_resolved.s_fps,g_resolved.s_req,g_resolved.s_eff);
    log_printf("STATUS: P1/P2 %s",g_resolved.core ? "RESOLVED" : "PASSIVE (missing/ambiguous anchor or invalid operand)");
    log_printf("STATUS: L1 %s",g_resolved.heartbeat ? "RESOLVED" : "PASSIVE (packet-take anchor)");
    log_printf("STATUS: L2/L3 %s",g_resolved.gate ? "RESOLVED" : "PASSIVE (gate dependencies)");
    log_printf("STATUS: XELL %s",g_resolved.xell ? "RESOLVED" : "PASSIVE (call-site anchor)");
    if (!g_resolved.online) {
        log_printf("STATUS: PASSIVE online-flag unresolved"); writer_request_flush(); return 0;
    }
    if (!g_resolved.core) { passive("required AOB/operand resolution failed; host remains untouched"); return 0; }
    g_clock_global=g_resolved.clock_global;
    if (!readable(g_clock_global,8)) { passive("decoded clock global unreadable"); return 0; }
    if (!readable(g_resolved.online_global,8)) { passive("decoded online global unreadable"); return 0; }
    DWORD start=GetTickCount();
    while (!*(void *volatile *)g_clock_global) {
        if (GetTickCount()-start>=15u*60*1000) { passive("clock object still null after 15 min"); return 0; } Sleep(50);
    }
    g_gate_tls=TlsAlloc();
    if (g_gate_tls>=64) { passive("no inline native TLS slot (0..63); cannot guard re-entrancy"); return 0; }
    sp.p1_host_unwind=host_unwind(g_resolved.hook[A_P1],why,sizeof why);
    sp.p2_host_unwind=host_unwind(g_resolved.hook[A_P2],why,sizeof why);
    if (!sp.p1_host_unwind || !sp.p2_host_unwind) { passive(why); return 0; }
    sp.alloc=alloc_near(g_base,2*STUB_PAGE); if (!sp.alloc) { passive("no nearby stub pages"); return 0; }
    sp.p1_return=g_resolved.hook[A_P1]+16; sp.p2_return=g_resolved.hook[A_P2]+17;
    if (stubs_build(&sp,&g_stubs,why,sizeof why)) { passive(why); return 0; }
    stubs_s2l(&sp,&g_stubs,g_resolved.req,g_resolved.eff,g_gate_tls);
    if (!finalize(sp.alloc,g_stubs.table,g_stubs.count)) { passive("stub protection/unwind registration failed"); return 0; }
    hooks_init_state();
    MH_STATUS st=MH_Initialize(); if (st!=MH_OK) { passive(MH_StatusToString(st)); return 0; }
    if (!hook("H1",g_resolved.hook[A_CLOCK],(void *)h1_detour,&h1_tramp) ||
        !hook("H2",g_resolved.hook[A_H2],(void *)h2_detour,&h2_tramp) ||
        !hook("P1",g_resolved.hook[A_P1],g_stubs.p1_stub,&unused) ||
        !hook("P2",g_resolved.hook[A_P2],g_stubs.p2_stub,&unused)) {
        MH_Uninitialize(); passive("required core hook creation failed"); return 0;
    }
    int hb=g_resolved.heartbeat && midhook(0,why,sizeof why);
    log_printf("STATUS: L1 %s",hb ? "ACTIVE (awaiting update publish/mailbox and render TID latch)" : "PASSIVE (hook/unwind unavailable)");
    int sleep=0;
    if (g_resolved.sleep_iat && readable(g_resolved.sleep_iat,8)) {
        g_native_sleep=(void *)*(uintptr_t *)g_resolved.sleep_iat;
        if (g_native_sleep) sleep=midhook(1,why,sizeof why);
    }
    log_printf("STATUS: SLEEP1 %s",sleep ? "ACTIVE (only render call patched)" : "PASSIVE (import/anchor/unwind)");
    int xell=0;
    if (g_resolved.xell && readable(g_resolved.xell_iat,8)) {
        uintptr_t target=*(uintptr_t *)g_resolved.xell_iat;
        g_xell_return=g_resolved.hook[A_XELL]+6;
        if (target) xell=hook("xellSleep",target,(void *)xell_detour,&xell_tramp);
    }
    log_printf("STATUS: XELL %s",xell ? "ACTIVE (entry and exit logging)" : "PASSIVE (import target/hook)");
    g_l2_ready=hb && g_resolved.gate && (!g_heartbeat_source || xell);
    if (g_l2_ready) {
        g_l2_ready=hook("update publish",g_resolved.hook[A_PUBLISH],(void *)publish_detour,&publish_tramp) &&
            hook("native frame/bypass",g_resolved.hook[A_FRAME],(void *)frame_detour,&frame_tramp);
    }
    log_printf("STATUS: L2/L3 %s",!g_cfg.lockstep ? "DISABLED BY CONFIG" : g_l2_ready ? "ACTIVE (runtime arming required)" : "PASSIVE (dependency/hook unavailable; factors stay 1)");
    if (!probe_init()) {
        builtin(A_LEGACY,"legacy_tick_0x5a6bb0",PK_COUNT,0,0,&g_probe_legacy);
        builtin(A_CAM,"camera_0x567070",PK_ORDER,0,RT_CAM,&g_probe_cam);
        builtin(A_CAM2,"camera2_0x20dda0c",PK_ORDER,0,RT_CAM2,&g_probe_cam2);
#ifndef RELEASE
        if (g_cfg.log_level || GetFileAttributesW(csv)!=INVALID_FILE_ATTRIBUTES) {
            /* S2C-P owns GetFps, including the helper's second-level caller. */
            builtin(A_GETTIMESTEP,"GetTimeStep",PK_HIST,1,0,NULL);
        }
        uintptr_t reserved[]={g_resolved.hook[A_CLOCK],g_resolved.hook[A_H2],g_resolved.hook[A_P1],g_resolved.hook[A_P2],
            g_resolved.hook[A_TAKE],g_resolved.hook[A_PUBLISH],g_resolved.hook[A_FRAME],g_resolved.hook[A_SLEEP],
            g_resolved.hook[A_GETFPS]}; /* S2C-F owns GetFps even with an S1 probe CSV. */
        probe_reserve(reserved,9);
        if (GetFileAttributesW(csv)!=INVALID_FILE_ATTRIBUTES) probe_load_csv(csv,g_base,(uint32_t)image_size);
#endif
        if (!probe_finalize_stubs()) for (LONG i=0;i<g_nprobes;i++) {
            probe_ctx *p=&g_probes[i];
            int ok=hook(p->name,p->addr,probe_stub_for(p),&p->tramp);
            if (ok) probe_set_tramp(p);
            log_printf("STATUS: PROBE %s %s",p->name,ok ? "ACTIVE" : "PASSIVE (hook failed)");
        }
        else log_printf("STATUS: PROBES PASSIVE (protection/unwind)");
    } else log_printf("STATUS: PROBES PASSIVE (allocation)");
    consumer_probe_install((void *)g_base,image_size);
    if (!consumer_fix_install((void *)g_base,image_size)) g_l2_ready=0;
    st=MH_EnableHook(MH_ALL_HOOKS);
    if (st!=MH_OK) { MH_DisableHook(MH_ALL_HOOKS); g_l2_ready=0; passive(MH_StatusToString(st)); return 0; }
    g_status_active=1;
#ifndef RELEASE
    toggle_file_start();
#endif
#ifdef RELEASE
    log_printf("STATUS: ACTIVE");
#else
    log_printf("STATUS: ACTIVE (S2L experiment; offline test procedure; independent review before install)");
#endif
    writer_request_flush(); return 0;
}

void init_start(HMODULE self)
{
    HANDLE h;
    g_self = self;
    h = CreateThread(NULL, 0, init_thread, NULL, 0, NULL);
    if (h) CloseHandle(h);
}
