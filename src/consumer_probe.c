#include "common.h"
#include "consumer_probe.h"
#include "consumer_fix.h"
#include "lockstep.h"
#include "resolve.h"
#include "MinHook.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
enum { CG_FPS,CG_HELPER,CG_XINPUT,CG_XSTEPS,CG_CHROFF,CG_PH,CG_PHOFF,
       CG_PHRET,CG_PHSTEP,CG_GOV,CG_SETTING,CG_STORE,CG_XACC };
typedef struct { const char *name,*pattern; int off,kind; } cp_anchor;
#include "consumer_anchors.h"
#define CP_ANCHORS (sizeof cp_anchors/sizeof *cp_anchors)
#define CP_KNOWN (sizeof cp_known_aobs/sizeof *cp_known_aobs)
typedef struct {
    uintptr_t match,site; int active,base_reg,src_reg,xmm;
    uint32_t field,caller_offset;
} cp_context;
static cp_context contexts[CP_ANCHORS];
static cp_table fps,helper_calls,ph_values;
static uint64_t fps_effective[CP_SLOTS][CP_STATES],helper_effective[CP_SLOTS][CP_STATES];
typedef struct { int64_t sum[2]; uint64_t valid[2],samples; uint32_t last[2]; } x_stat;
static x_stat x_inputs[2][CP_STATES];
static uintptr_t known[CP_KNOWN],helper_ret,ph_ret,gov_global;
static unsigned helper_frame,chr_offset,ph_offset,ph_count_offset;
static uint64_t ticks[CP_STATES],steps[2][CP_STATES],calls[2][CP_STATES],ph_steps[CP_STATES];
static int x_ready,ph_ready;
static unsigned tick_state=CP_UNARMED;
static uint64_t exposure[CP_STATES];
static cp_table gov_values;
#define GOV_EVENTS 4096
typedef struct { uint64_t stamp; uintptr_t caller; uint32_t id,old,value,state; } gov_event;
static gov_event gov_ring[GOV_EVENTS];
static uint64_t gov_write,gov_read,gov_lost,gov_counts[3][CP_STATES];
void *cp_fps_tramp,*cp_ph_tramp,*cp_phstep_tramp;
extern void cp_fps_entry(void),cp_ph_entry(void),cp_phstep_entry(void),cp_mid_leaf(void);
extern void cf_fps_entry(void),cf_mid_leaf(void),cf_x_entry(void);
static unsigned state(void)
{
    return __atomic_load_n(&tick_state,__ATOMIC_RELAXED);
}
static const char *const states[]={"ON","OFF","unarmed"};
static uint32_t u32(uintptr_t p) { uint32_t v; memcpy(&v,(void *)p,4); return v; }
static uintptr_t relative(uintptr_t p,int disp,int end) { return p+end+(int32_t)u32(p+disp); }
static uint64_t reg(const uint64_t *r,int i)
{
    static const int map[]={14,13,12,11,-1,10,9,8,7,6,5,4,3,2,1,0};
    return i==4 ? (uintptr_t)(r+15) : r[map[i]];
}
/* Convert finite f32 bits to 24-bit fractional units using integer arithmetic.
   This keeps varying dt samples out of a lifetime histogram, and avoids changing
   MXCSR exception flags on the game thread. Extreme/non-finite samples retain
   their raw last value but are excluded from the means. */
static void x_sample(x_stat *stat,unsigned i,uint32_t bits)
{
    unsigned raw=(bits>>23)&255; int exponent=(int)raw-127;
    __atomic_store_n(&stat->last[i],bits,__ATOMIC_RELAXED);
    if (raw==255 || exponent>15) return;
    uint64_t mantissa=raw ? (bits&0x7fffff)|0x800000 : bits&0x7fffff;
    int shift=exponent+1;
    int64_t units=shift>=0 ? (int64_t)(mantissa<<shift) : -shift>=64 ? 0 : (int64_t)(mantissa>>-shift);
    if (bits>>31) units=-units;
    __atomic_fetch_add(&stat->sum[i],units,__ATOMIC_RELAXED);
    __atomic_fetch_add(&stat->valid[i],1,__ATOMIC_RELAXED);
}
/* C compiled without AVX; only arithmetic, bounded atomics and valid game-owned
   operands. No CRT, Win32 calls, allocation, locks or logging on this path. */
void consumer_probe_hit(unsigned id,const uint64_t *r,const uint8_t *x,const uintptr_t *stack)
{
    unsigned s=state(); cp_context *c=&contexts[id];
    int kind=cp_anchors[id].kind;
    if (kind==CG_FPS) {
        uintptr_t ret=*stack,outer=cp_outer_return(stack,helper_ret,helper_frame);
        cp_slot_hit(&fps,ret,0,s);
        if (outer) cp_slot_hit(&helper_calls,ret,outer,s);
        return;
    }
    if (kind==CG_PH || kind==CG_PHSTEP) {
        if (kind==CG_PHSTEP) {
            if (*stack==ph_ret) __atomic_fetch_add(&ph_steps[s],1,__ATOMIC_RELAXED);
        } else if (ph_ready) {
            uintptr_t scene=reg(r,1);
            uint32_t bits=*(uint32_t *)(scene+ph_offset),n=*(uint32_t *)(scene+ph_count_offset);
            cp_slot_hit(&ph_values,(uintptr_t)bits+1,n,s);
        }
        return;
    }
    stack++; /* mid-site helper adds its own return address */
    if (kind==CG_XINPUT) {
        if (!x_ready) return;
        unsigned path=(r[-1]&0x40) ? 1 : 0; /* ZF from test [r9],0xf: 0 = X, 1 = override */
        uint32_t producer=*(uint32_t *)reg(r,3),extra=*(uint32_t *)(reg(r,9)+chr_offset);
        x_stat *stat=&x_inputs[path][s];
        x_sample(stat,0,producer); x_sample(stat,1,extra);
        __atomic_fetch_add(&stat->samples,1,__ATOMIC_RELAXED); return;
    }
    if (kind==CG_XSTEPS) {
        if (!x_ready) return;
        uintptr_t chr=*(uintptr_t *)reg(r,7);
        unsigned path=(*(uint8_t *)chr&15) ? 0 : 1;
        __atomic_fetch_add(&calls[path][s],1,__ATOMIC_RELAXED);
        __atomic_fetch_add(&steps[path][s],(uint32_t)reg(r,0),__ATOMIC_RELAXED); return;
    }
    if (kind==CG_SETTING || kind==CG_STORE) {
        uintptr_t target=kind==CG_SETTING ? gov_global : (uintptr_t)reg(r,c->base_reg)+c->field;
        if (!target) return;
        if (kind==CG_STORE && (uintptr_t)reg(r,c->base_reg)!=*(uintptr_t *)g_clock_global) return;
        uint32_t old=*(uint32_t *)target;
        uint32_t value=c->xmm ? *(const uint32_t *)(x+16*c->src_reg) : (uint32_t)reg(r,c->src_reg);
        if (kind==CG_STORE && c->field==g_resolved.fps && old==value) return;
        uintptr_t caller=*(const uintptr_t *)((const uint8_t *)stack+c->caller_offset);
        unsigned type=kind==CG_SETTING ? 0 : c->field==g_resolved.fps ? 1 : 2;
        __atomic_fetch_add(&gov_counts[type][s],1,__ATOMIC_RELAXED);
        uint64_t i=__atomic_fetch_add(&gov_write,1,__ATOMIC_RELAXED);
        gov_event *event=&gov_ring[i&(GOV_EVENTS-1)];
        __atomic_store_n(&event->stamp,0,__ATOMIC_RELAXED);
        event->caller=caller; event->id=id; event->old=old; event->value=value; event->state=s;
        __atomic_store_n(&event->stamp,i+1,__ATOMIC_RELEASE);
    }
}
void consumer_probe_tick(int64_t now)
{
    static int64_t previous;
    if (previous && now>previous) __atomic_fetch_add(&exposure[state()],(uint64_t)(now-previous),__ATOMIC_RELAXED);
    previous=now;
    unsigned s=!__atomic_load_n(&g_toggle,__ATOMIC_RELAXED) ? CP_OFF :
        __atomic_load_n(&g_l3_tick,__ATOMIC_RELAXED) ? CP_ON : CP_UNARMED;
    __atomic_store_n(&tick_state,s,__ATOMIC_RELAXED);
    __atomic_fetch_add(&ticks[s],1,__ATOMIC_RELAXED);
}

static uintptr_t unique(uintptr_t text,size_t n,const char *pattern)
{
    aob_t a; uintptr_t p=0;
    return !aob_parse(pattern,&a) && aob_scan((void *)text,n,&a,&p,1)==1 ? p : 0;
}
static uintptr_t caller_return(uintptr_t text,size_t n,const char *pattern,uintptr_t target)
{
    uintptr_t p=unique(text,n,pattern);
    return p && *(const uint8_t *)p==0xe8 && relative(p,1,5)==target ? p+5 : 0;
}
/* Clone active host unwind operations, dropping language handlers. Chained
   body regions are flattened in unwind order; leaf functions use an empty record.
   Derive the caller stack displacement from ALLOC/PUSH codes, never a fixed frame. */
static int unwind(uint8_t *image,size_t size,uintptr_t site,uint8_t *dst,uint32_t *frame)
{
    IMAGE_NT_HEADERS64 *nt=(void *)(image+((IMAGE_DOS_HEADER *)image)->e_lfanew);
    IMAGE_DATA_DIRECTORY d=nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
    if (d.VirtualAddress>size || d.Size>size-d.VirtualAddress) return 0;
    RUNTIME_FUNCTION *f=(void *)(image+d.VirtualAddress); uintptr_t base=(uintptr_t)image;
    *frame=0; memcpy(dst,"\1\0\0\0",4);
    for (unsigned j=0;j<d.Size/sizeof *f;j++) if (site>=base+f[j].BeginAddress && site<base+f[j].EndAddress) {
        uint32_t info_rva=f[j].UnwindData; unsigned total=0;
        for (unsigned depth=0;depth<16;depth++) {
          if (info_rva>size || size-info_rva<4) return 0;
          const uint8_t *u=image+info_rva; size_t len=stubs_unwind_len(u);
          if ((u[0]&7)!=1 || len>size-info_rva || total+u[2]>250 ||
              (!depth && site-base-f[j].BeginAddress<u[1])) return 0;
          if (u[3] && dst[3] && u[3]!=dst[3]) return 0;
          if (u[3]) dst[3]=u[3];
          memcpy(dst+4+2*total,u+4,2*u[2]);
          for (unsigned k=0;k<u[2];) {
            unsigned op=u[5+2*k]&15,info=u[5+2*k]>>4,ns=1;
            dst[4+2*(total+k)]=0;
            if (op==0) *frame+=8;
            else if (op==2) *frame+=8*info+8;
            else if (op==1) {
                ns=info ? 3 : 2;
                if (k+ns>u[2]) return 0;
                uint32_t a=(uint32_t)u[6+2*k]|((uint32_t)u[7+2*k]<<8);
                *frame+=info ? a|((uint32_t)u[8+2*k]<<16)|((uint32_t)u[9+2*k]<<24) : a*8;
            } else if (op==4 || op==8) ns=2;
            else if (op==5 || op==9) ns=3;
            else if (op!=3) return 0;
            if (k+ns>u[2]) return 0;
            k+=ns;
          }
          total+=u[2]; dst[2]=(uint8_t)total;
          if (!(u[0]&0x20)) {
              if (total&1) memset(dst+4+2*total,0,2);
              return 1;
          }
          if (len+12>size-info_rva) return 0;
          memcpy(&info_rva,u+len+8,4);
        }
        return 0; /* cyclic or excessively deep chain */
    }
    return 1;
}
static uint8_t *near_alloc(uintptr_t base)
{
    for (uintptr_t at=(base+0x10000)&~(uintptr_t)0xffff;at<base+0x60000000;at+=0x10000) {
        MEMORY_BASIC_INFORMATION m;
        if (VirtualQuery((void *)at,&m,sizeof m) && m.State==MEM_FREE) {
            uint8_t *p=VirtualAlloc((void *)at,4096,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE);
            if (p) return p;
        }
    }
    return NULL;
}
static int place_mid_at(uint8_t *image,size_t size,cp_context *c,const char *name,unsigned id,void *leaf)
{
    uint8_t uw[512];
    if (!unwind(image,size,c->site,uw,&c->caller_offset)) { log_printf("STATUS: PASSIVE %s (host unwind)",name); return 0; }
    uint8_t *a=near_alloc((uintptr_t)image); void *tramp=NULL;
    if (!a) { log_printf("STATUS: PASSIVE %s (near allocation)",name); return 0; }
    /* Call preserving helper, then JE/JNE to the original trampoline. Conditional
       transfers avoid the virtual unwinder's tail-jump epilogue heuristic. */
    memcpy(a,"\xff\x15\x0c\0\0\0\x0f\x84\0\0\0\0\x0f\x85\0\0\0\0",18);
    uintptr_t fn=(uintptr_t)leaf; memcpy(a+18,&fn,8); memcpy(a+26,&id,4);
    memcpy(a+0x100,uw,stubs_unwind_len(uw));
    RUNTIME_FUNCTION *rf=(void *)(a+0x300); *rf=(RUNTIME_FUNCTION){0,18,0x100};
    DWORD old;
    /* MinHook requires both target and detour to be executable at creation. */
    if (!VirtualProtect(a,4096,PAGE_EXECUTE_READ,&old)) { VirtualFree(a,0,MEM_RELEASE); return 0; }
    MH_STATUS result=MH_CreateHook((void *)c->site,a,&tramp);
    if (result!=MH_OK) { log_printf("STATUS: PASSIVE %s (%s)",name,MH_StatusToString(result)); VirtualFree(a,0,MEM_RELEASE); return 0; }
    int64_t d1=(intptr_t)tramp-(intptr_t)(a+12),d2=(intptr_t)tramp-(intptr_t)(a+18);
    if (d1<INT32_MIN || d1>INT32_MAX || d2<INT32_MIN || d2>INT32_MAX) { log_printf("STATUS: PASSIVE %s (trampoline distance)",name); goto fail; }
    if (!VirtualProtect(a,4096,PAGE_READWRITE,&old)) goto fail;
    int32_t b=(int32_t)d1; memcpy(a+8,&b,4); b=(int32_t)d2; memcpy(a+14,&b,4);
    if (!VirtualProtect(a,4096,PAGE_EXECUTE_READ,&old) || !RtlAddFunctionTable(rf,1,(DWORD64)(uintptr_t)a)) { log_printf("STATUS: PASSIVE %s (protect/register)",name); goto fail; }
    FlushInstructionCache(GetCurrentProcess(),a,4096); crash_add_alloc_of(a,"consumer mid probe");
    crash_add_alloc_of(tramp,"consumer trampoline"); return 1;
fail:
    MH_RemoveHook((void *)c->site); VirtualFree(a,0,MEM_RELEASE); return 0;
}
static int place_mid(uint8_t *image,size_t size,unsigned id)
{
    return place_mid_at(image,size,&contexts[id],cp_anchors[id].name,id,(void *)cp_mid_leaf);
}
static int entry(cp_context *c,void *thunk,void **tramp)
{
    if (MH_CreateHook((void *)c->site,thunk,tramp)!=MH_OK) return 0;
    crash_add_alloc_of(*tramp,"consumer trampoline"); return 1;
}

typedef struct { unsigned index,policy,core; const char *name; } cf_direct;
typedef struct { const char *name,*pattern; unsigned policy,core; } cf_outer;
typedef struct { const char *name,*pattern; unsigned offset,kind; } cf_patch;
#include "consumer_fix_anchors.h"
#define CF_OUTERS (sizeof cf_outers/sizeof *cf_outers)
#define CF_PATCHES (sizeof cf_patches/sizeof *cf_patches)
static cf_site direct_sites[CP_KNOWN],outer_sites[CF_OUTERS];
static unsigned direct_count,outer_count;
static cp_context fix_contexts[CF_PATCHES];
static void *x_tramp;
int g_consumers_ready;
static int site_order(const void *a,const void *b)
{
    uintptr_t x=((const cf_site *)a)->ret,y=((const cf_site *)b)->ret;
    return x<y ? -1 : x>y;
}
static int fix_armed(cf_tick tick)
{
    /* TLS matches P1/P2 on re-entrant/overlapping native calls. */
    return g_consumers_ready && tick.armed && g_l3_tick && gate_tls_get()!=1;
}
static int effective_fps(uintptr_t ret,uintptr_t outer,int native,cf_tick tick)
{
    if (!fix_armed(tick) || native!=120) return native;
    const cf_site *s=NULL; int i;
    if (ret==helper_ret) {
        i=cf_find(outer_sites,outer_count,outer);
        if (i>=0) s=&outer_sites[i];
    } else {
        i=cf_find(direct_sites,direct_count,ret);
        if (i>=0) s=&direct_sites[i];
    }
    return s ? cf_tick_rate(tick,s->ordinal,s->policy) : native;
}
/* These preserving assembly entries return through the real caller stack.
   Calling the original leaf retains null-clock and native fallback behaviour. */
int consumer_fps_value(const uintptr_t *stack)
{
    cf_tick tick=consumer_fix_snapshot();
    int native=((int (*)(void))cp_fps_tramp)();
    uintptr_t ret=*stack,outer=cp_outer_return(stack,helper_ret,helper_frame);
    int value=effective_fps(ret,outer,native,tick); unsigned s=state();
    int i=cp_slot_hit(&fps,ret,0,s);
    if (i>=0) __atomic_fetch_add(&fps_effective[i][s],(uint64_t)value,__ATOMIC_RELAXED);
    if (outer) {
        i=cp_slot_hit(&helper_calls,ret,outer,s);
        if (i>=0) __atomic_fetch_add(&helper_effective[i][s],(uint64_t)value,__ATOMIC_RELAXED);
    }
    return value;
}
float consumer_x_value(void)
{
    cf_tick tick=consumer_fix_snapshot();
    float native=((float (*)(void))x_tramp)();
    if (!fix_armed(tick) || !g_clock_global) return native;
    const uint8_t *clk=*(const uint8_t *volatile *)g_clock_global;
    return clk ? cf_x_speed(*(const float *)(clk+g_resolved.eff),
                           *(const float *)(clk+g_resolved.req),1) : native;
}
float consumer_mid_factor(unsigned id)
{
    cf_tick tick=consumer_fix_snapshot();
    if (id>=CF_PATCHES || !fix_armed(tick)) return 0;
    /* Match GetFps's decoded native FPS field, not the governor's setting. */
    const uint8_t *clk=g_clock_global ? *(const uint8_t *volatile *)g_clock_global : NULL;
    if (!clk || *(const int32_t *)(clk+g_resolved.actual_fps)!=120) return 1.0f;
    return id==4 ? 1.0f/tick.r : tick.r;
}
int consumer_fix_install(uint8_t *image,size_t size)
{
    uintptr_t ts; size_t tn; unsigned char placed[CP_KNOWN]={0};
    IMAGE_NT_HEADERS64 *nt=(void *)(image+((IMAGE_DOS_HEADER *)image)->e_lfanew);
    IMAGE_SECTION_HEADER *sec=IMAGE_FIRST_SECTION(nt); ts=0; tn=0;
    for (unsigned i=0;i<nt->FileHeader.NumberOfSections;i++) if (!memcmp(sec[i].Name,".text",6)) {
        ts=(uintptr_t)image+sec[i].VirtualAddress; tn=sec[i].Misc.VirtualSize;
    }
    int ok=contexts[0].active && helper_ret && helper_frame;
    if (!ok) log_printf("STATUS: PASSIVE S2C-F core GetFps/helper (hook/frame dependency)");
    direct_count=CP_KNOWN;
    for (unsigned i=0;i<CP_KNOWN;i++) {
        direct_sites[i]=(cf_site){known[i],i==37 || i==38 ? CF_DENY : CF_KNOWN,0,i};
        const char *name="known native caller";
        for (unsigned j=0;j<sizeof cf_directs/sizeof *cf_directs;j++) if (cf_directs[j].index==i) {
            direct_sites[i].policy=cf_directs[j].policy; direct_sites[i].core=cf_directs[j].core;
            name=cf_directs[j].name;
        }
        if ((i==37 || i==38) && direct_sites[i].policy!=CF_DENY) {
            log_printf("STATUS: PASSIVE S2C-F (mode denylist policy mismatch at site %u; L2/L3 disabled)",i);
            ok=0;
        }
        placed[i]=known[i]!=0;
        if (direct_sites[i].policy<=CF_FLOAT) log_printf("STATUS: %s S2C-F %s %s ret=0x%llX policy=%u",
            placed[i] ? "RESOLVED" : "PASSIVE",direct_sites[i].core ? "core" : "rare",name,
            (unsigned long long)(known[i] ? known[i]-g_base : 0),direct_sites[i].policy);
    }
    ok=cf_core_ok(direct_sites,placed,CP_KNOWN) && ok;
    for (unsigned i=0;i<direct_count;i++) if (direct_sites[i].ret && direct_sites[i].policy<=CF_FLOAT) {
        if ((known[37] && direct_sites[i].ret==known[37]) ||
            (known[38] && direct_sites[i].ret==known[38])) {
            log_printf("STATUS: PASSIVE S2C-F (fix aliases a mode denylist caller; L2/L3 disabled)");
            ok=0;
        }
    }
    qsort(direct_sites,direct_count,sizeof *direct_sites,site_order);
    outer_count=CF_OUTERS;
    uintptr_t helper=contexts[1].match;
    for (unsigned i=0;i<CF_OUTERS;i++) {
        uintptr_t p=ts ? caller_return(ts,tn,cf_outers[i].pattern,helper) : 0;
        outer_sites[i]=(cf_site){p,cf_outers[i].policy,cf_outers[i].core,CP_KNOWN+i};
        if (!p && cf_outers[i].core) ok=0;
        log_printf("STATUS: %s S2C-F %s %s policy=%u",p ? "RESOLVED" : "PASSIVE",
            cf_outers[i].core ? "core" : "rare",cf_outers[i].name,cf_outers[i].policy);
    }
    qsort(outer_sites,outer_count,sizeof *outer_sites,site_order);
    for (unsigned i=0;i<CF_PATCHES;i++) {
        cp_context *c=&fix_contexts[i]; c->match=ts ? unique(ts,tn,cf_patches[i].pattern) : 0;
        c->site=c->match ? c->match+cf_patches[i].offset : 0;
        if (c->match && i==0) {
            /* The X leaf must read the same clock/effective field as P1. */
            if (relative(c->match,3,7)!=g_clock_global || u32(c->match+16)!=g_resolved.eff) c->match=0;
        }
        c->active=c->match && (i==0 ? entry(c,(void *)cf_x_entry,&x_tramp) :
            place_mid_at(image,size,c,cf_patches[i].name,i,(void *)cf_mid_leaf));
        if (!c->active) ok=0;
        log_printf("STATUS: %s S2C-F core %s",c->active ? "PLACED" : "PASSIVE",cf_patches[i].name);
    }
    /* Hooks may remain installed for diagnostics; this immutable session flag
       and L2 readiness make a partial core entirely passive. */
    g_consumers_ready=ok;
    log_printf("STATUS: S2C-F core %s (all-or-nothing)",ok ? "READY" : "PASSIVE; L2/L3 disabled for session");
    return ok;
}
void consumer_probe_install(uint8_t *image,size_t size)
{
    IMAGE_NT_HEADERS64 *nt=(void *)(image+((IMAGE_DOS_HEADER *)image)->e_lfanew);
    IMAGE_SECTION_HEADER *sec=IMAGE_FIRST_SECTION(nt); uintptr_t ts=0; size_t tn=0;
    for (unsigned i=0;i<nt->FileHeader.NumberOfSections;i++) if (!memcmp(sec[i].Name,".text",6)) {
        ts=(uintptr_t)image+sec[i].VirtualAddress; tn=sec[i].Misc.VirtualSize;
    }
    if (!ts || ts<(uintptr_t)image || ts-(uintptr_t)image>size || tn>size-(ts-(uintptr_t)image)) return;
    for (unsigned i=0;i<CP_ANCHORS;i++) {
        cp_context *c=&contexts[i]; c->match=unique(ts,tn,cp_anchors[i].pattern);
        if (!c->match) { log_printf("STATUS: PASSIVE %s (AOB missing/ambiguous)",cp_anchors[i].name); continue; }
        c->site=c->match+cp_anchors[i].off;
    }
    if (contexts[1].match && !cp_helper_decode((void *)contexts[1].match,9,contexts[1].match,contexts[0].match,&helper_ret,&helper_frame))
        log_printf("STATUS: PASSIVE P-G helper (invalid frame/call operand)");
    if (contexts[4].match) chr_offset=u32(contexts[4].match+0x11);
    if (contexts[5].match) ph_offset=u32(contexts[5].match+0xf);
    if (contexts[6].match) ph_count_offset=u32(contexts[6].match+3);
    if (contexts[7].match && relative(contexts[7].match,1,5)==contexts[8].match) ph_ret=contexts[7].match+5;
    if (contexts[9].match) {
        uintptr_t p=relative(contexts[9].match,3,7);
        if (p>=(uintptr_t)image && p-(uintptr_t)image<=size-4) gov_global=p;
    }
    for (unsigned i=0;i<CP_KNOWN;i++) {
        uintptr_t p=caller_return(ts,tn,cp_known_aobs[i],contexts[0].match);
        if (p) known[i]=p;
        else log_printf("STATUS: PASSIVE P-G known site %u (AOB/call target)",i);
    }
    for (unsigned i=0;i<CP_ANCHORS;i++) {
        cp_context *c=&contexts[i]; int kind=cp_anchors[i].kind;
        if (!c->match || cp_anchors[i].off<0) continue;
        if (kind==CG_FPS) c->active=entry(c,cf_fps_entry,&cp_fps_tramp);
        else if (kind==CG_PH) c->active=ph_offset && ph_count_offset && entry(c,cp_ph_entry,&cp_ph_tramp);
        else if (kind==CG_PHSTEP) c->active=ph_ret && entry(c,cp_phstep_entry,&cp_phstep_tramp);
        else {
            if (kind==CG_SETTING) {
                if (!gov_global || relative(c->match,2,6)!=gov_global) {
                    log_printf("STATUS: PASSIVE P-Gov setting (global operand mismatch)"); continue;
                }
                c->src_reg=(*(uint8_t *)(c->match+1)>>3)&7;
            }
            if (kind==CG_STORE) {
                const uint8_t *p=(void *)c->site; int k=0,rex=0;
                if (p[k]==0xf3) { c->xmm=1; k++; }
                if ((p[k]&0xf0)==0x40) rex=p[k++];
                if (c->xmm) { if (p[k++]!=0x0f || p[k++]!=0x11) continue; }
                else if (p[k++]!=0x89) continue;
                unsigned mod=p[k++];
                if ((mod&0xc0)!=0x80 || (mod&7)==4) continue;
                c->base_reg=(mod&7)+((rex&1) ? 8 : 0); c->src_reg=((mod>>3)&7)+((rex&4) ? 8 : 0);
                c->field=u32((uintptr_t)p+k);
                if (c->field!=g_resolved.fps && c->field!=g_resolved.req) continue;
            }
            c->active=place_mid(image,size,i);
        }
        log_printf("STATUS: %s %s RVA=0x%llX",c->active ? "ACTIVE" : "PASSIVE",cp_anchors[i].name,(unsigned long long)(c->site-g_base));
    }
    x_ready=contexts[2].active && contexts[3].active && chr_offset && contexts[20].match &&
        relative(contexts[3].match+7,1,5)==contexts[20].match;
    ph_ready=contexts[5].active && ph_offset && ph_count_offset;
    if (!x_ready) {
        for (unsigned i=2;i<=3;i++) if (contexts[i].active) { MH_RemoveHook((void *)contexts[i].site); contexts[i].active=0; }
        log_printf("STATUS: PASSIVE P-X (incomplete input/accumulator dependencies)");
    }
    log_printf("S2C-P decode: helper_frame=0x%X helper_ret=0x%llX chr_extra=%X scene_step=%X scene_substeps=%X gov=0x%llX",
        helper_frame,(unsigned long long)(helper_ret ? helper_ret-g_base : 0),chr_offset,ph_offset,ph_count_offset,
        (unsigned long long)(gov_global ? gov_global-g_base : 0));
}

/* Writer thread only. Counters are cumulative; snapshots never clear a live slot. */
typedef struct { uintptr_t ret,outer; uint64_t n,effective; } cp_row;
static uint64_t fps_last[CP_SLOTS][CP_STATES],helper_last[CP_SLOTS][CP_STATES],ph_last[CP_SLOTS][CP_STATES],
    gov_last[CP_SLOTS][CP_STATES];
static cp_row rows[CP_SLOTS];
static uint64_t fps_effective_last[CP_SLOTS][CP_STATES],helper_effective_last[CP_SLOTS][CP_STATES];
static unsigned collect(cp_table *t,uint64_t last[CP_SLOTS][CP_STATES],unsigned s)
{
    unsigned n=0;
    for (unsigned i=0;i<CP_SLOTS;i++) {
        cp_slot *p=&t->slot[i]; uintptr_t key=__atomic_load_n(&p->ret,__ATOMIC_ACQUIRE);
        if (!key || key==UINTPTR_MAX) continue;
        uint64_t v=__atomic_load_n(&p->count[s],__ATOMIC_RELAXED),delta=v-last[i][s]; last[i][s]=v;
        if (!delta) continue;
        unsigned j;
        for (j=0;j<n;j++) if (rows[j].ret==key && rows[j].outer==p->outer) break;
        if (j==n) rows[n++]=(cp_row){key,p->outer,0,0};
        rows[j].n+=delta;
        if (t==&fps || t==&helper_calls) {
            uint64_t (*values)[CP_STATES]=t==&fps ? fps_effective : helper_effective;
            uint64_t (*previous)[CP_STATES]=t==&fps ? fps_effective_last : helper_effective_last;
            uint64_t value=__atomic_load_n(&values[i][s],__ATOMIC_RELAXED);
            rows[j].effective+=value-previous[i][s]; previous[i][s]=value;
        }
    }
    return n;
}
static int descending(const void *a,const void *b)
{
    const cp_row *x=a,*y=b;
    return x->n>y->n ? -1 : x->n<y->n;
}
static unsigned long long rva(uintptr_t p)
{
    return (unsigned long long)(p>=g_base && p-g_base<0x10000000 ? p-g_base : p);
}
void consumer_probe_second(double seconds)
{
    typedef struct { uintptr_t ret,outer; uint64_t count; unsigned slot; } guard_row;
    static guard_row guard_rows[CP_SLOTS];
    static uint64_t last[2][CP_SLOTS],last_unarmed;
    static double hot[2][CP_SLOTS];
    uint64_t unarmed=__atomic_load_n(&ticks[CP_OFF],__ATOMIC_RELAXED)+__atomic_load_n(&ticks[CP_UNARMED],__ATOMIC_RELAXED);
    int armed=state()==CP_ON && unarmed==last_unarmed; last_unarmed=unarmed;
    if (contexts[0].active && seconds>0) for (unsigned table=0;table<2;table++) {
      unsigned n=0;
      for (unsigned i=0;i<CP_SLOTS;i++) {
        cp_slot *p=table ? &helper_calls.slot[i] : &fps.slot[i]; uintptr_t key=__atomic_load_n(&p->ret,__ATOMIC_ACQUIRE);
        if (!key || key==UINTPTR_MAX) continue;
        uint64_t count=__atomic_load_n(&p->count[CP_ON],__ATOMIC_RELAXED),delta=count-last[table][i]; last[table][i]=count;
        unsigned j=0;
        while (j<n && (guard_rows[j].ret!=key || guard_rows[j].outer!=p->outer)) j++;
        if (j==n) guard_rows[n++]=(guard_row){key,p->outer,0,i};
        guard_rows[j].count+=delta;
      }
      for (unsigned j=0;j<n;j++) {
        guard_row *p=&guard_rows[j];
        int is_known=table ? cf_find(outer_sites,outer_count,p->outer)>=0 : cf_find(direct_sites,direct_count,p->ret)>=0;
        if (cf_guard(&hot[table][p->slot],p->count,seconds,armed,is_known)) {
            log_printf("UNKNOWN HOT CALLER ret=0x%llX outer=0x%llX gate=ON calls/s=%.2f",rva(p->ret),rva(p->outer),(double)p->count/seconds);
        }
      }
    }
    if (gov_global) cp_slot_hit(&gov_values,(uintptr_t)*(volatile uint32_t *)gov_global+1,0,state());
}
void consumer_probe_drain(void)
{
    uint64_t end=__atomic_load_n(&gov_write,__ATOMIC_ACQUIRE);
    if (end-gov_read>GOV_EVENTS) { gov_lost+=end-gov_read-GOV_EVENTS; gov_read=end-GOV_EVENTS; }
    while (gov_read<end) {
        gov_event *p=&gov_ring[gov_read&(GOV_EVENTS-1)],event;
        uint64_t stamp=__atomic_load_n(&p->stamp,__ATOMIC_ACQUIRE);
        if (stamp!=gov_read+1) break; /* a producer has not committed yet */
        event=*p; __atomic_thread_fence(__ATOMIC_ACQUIRE);
        if (__atomic_load_n(&p->stamp,__ATOMIC_ACQUIRE)!=stamp) { gov_lost++; gov_read++; continue; }
        cp_context *c=&contexts[event.id]; int kind=cp_anchors[event.id].kind;
        float speed; memcpy(&speed,&event.value,4);
        log_printf("P-Gov gate=%s %s site=0x%llX caller=0x%llX land=+0x%X old=0x%X new=0x%X speed=%.7g",
            states[event.state],kind==CG_SETTING ? "global-setting" : c->field==g_resolved.fps ? "fps-mode-change" : "speed_req",
            rva(c->site),rva(event.caller),kind==CG_SETTING ? 0 : c->field,event.old,event.value,(double)speed);
        gov_read++;
    }
}
void consumer_probe_summary(double seconds)
{
    static uint64_t last_ticks[CP_STATES],last_exposure[CP_STATES],last_steps[2][CP_STATES],
        last_calls[2][CP_STATES],last_ph_steps[CP_STATES],last_gov[3][CP_STATES];
    static x_stat last_inputs[2][CP_STATES];
    for (unsigned s=0;s<CP_STATES;s++) {
        uint64_t t=__atomic_load_n(&ticks[s],__ATOMIC_RELAXED),dt=t-last_ticks[s]; last_ticks[s]=t;
        uint64_t e=__atomic_load_n(&exposure[s],__ATOMIC_RELAXED),de=e-last_exposure[s]; last_exposure[s]=e;
        double secs=g_qpf ? (double)de/g_qpf : 0;
        if (!dt && !de && s!=state()) continue;
        if (secs<=0 && s==state()) secs=seconds;
        if (contexts[0].active) {
            unsigned n=collect(&fps,fps_last,s); uint64_t total=0;
            for (unsigned i=0;i<n;i++) total+=rows[i].n;
            qsort(rows,n,sizeof *rows,descending);
            log_printf("P-G %.1fs gate=%s exposure=%.3fs ticks=%llu calls=%llu sites=%u top=64 overflow=%llu",seconds,states[s],secs,
                (unsigned long long)dt,(unsigned long long)total,n,(unsigned long long)__atomic_load_n(&fps.overflow,__ATOMIC_RELAXED));
            for (unsigned i=0;i<n && i<64;i++) log_printf("P-G gate=%s rank=%u ret=0x%llX outer=0x%llX calls/s=%.3f calls/tick=%.4f calls=%llu effective=%.6f",
                states[s],i+1,rva(rows[i].ret),rva(rows[i].outer),secs>0 ? rows[i].n/secs : 0,dt ? (double)rows[i].n/dt : 0,(unsigned long long)rows[i].n,(double)rows[i].effective/rows[i].n);
            n=collect(&helper_calls,helper_last,s); qsort(rows,n,sizeof *rows,descending);
            for (unsigned i=0;i<n;i++) log_printf("P-G helper gate=%s ret=0x%llX outer=0x%llX frame=0x%X calls/s=%.3f calls/tick=%.4f calls=%llu effective=%.6f",
                states[s],rva(rows[i].ret),rva(rows[i].outer),helper_frame,secs>0 ? rows[i].n/secs : 0,dt ? (double)rows[i].n/dt : 0,(unsigned long long)rows[i].n,(double)rows[i].effective/rows[i].n);
        }
        if (x_ready) for (unsigned path=0;path<2;path++) {
            uint64_t a=__atomic_load_n(&steps[path][s],__ATOMIC_RELAXED),b=__atomic_load_n(&calls[path][s],__ATOMIC_RELAXED);
            uint64_t da=a-last_steps[path][s],db=b-last_calls[path][s]; last_steps[path][s]=a; last_calls[path][s]=b;
            x_stat *stat=&x_inputs[path][s],*last=&last_inputs[path][s]; double mean[2]; float value[2];
            for (unsigned i=0;i<2;i++) {
                int64_t sum=__atomic_load_n(&stat->sum[i],__ATOMIC_RELAXED);
                uint64_t valid=__atomic_load_n(&stat->valid[i],__ATOMIC_RELAXED),dn=valid-last->valid[i];
                mean[i]=dn ? (double)(sum-last->sum[i])/16777216.0/dn : 0;
                uint32_t bits=__atomic_load_n(&stat->last[i],__ATOMIC_RELAXED); memcpy(&value[i],&bits,4);
                last->sum[i]=sum; last->valid[i]=valid;
            }
            uint64_t count=__atomic_load_n(&stat->samples,__ATOMIC_RELAXED),samples=count-last->samples; last->samples=count;
            log_printf("P-X %.1fs gate=%s path=%s calls=%llu sum_n=%llu ticks/s=%.3f steps/accumulator/s=%.6f [rdx] mean/last=%.7g/%.7g [chr+0x%X] mean/last=%.7g/%.7g samples=%llu fixed=%d",
                seconds,states[s],path ? "override" : "X",(unsigned long long)db,(unsigned long long)da,secs>0 ? dt/secs : 0,
                cp_steps_per_second(da,db,dt,secs),mean[0],(double)value[0],chr_offset,mean[1],(double)value[1],(unsigned long long)samples,g_consumers_ready);
        }
        if (ph_ready) {
            unsigned n=collect(&ph_values,ph_last,s); uint64_t total=0; double configured=0;
            for (unsigned i=0;i<n;i++) {
                uint32_t bits=(uint32_t)(rows[i].ret-1); float step; memcpy(&step,&bits,4); total+=rows[i].n;
                configured+=(double)rows[i].outer*rows[i].n;
                log_printf("P-Ph %.1fs gate=%s scene+0x%X=%.9g bits=%08X configured_substeps=%llu count/s=%.3f calls=%llu",
                    seconds,states[s],ph_offset,(double)step,bits,(unsigned long long)rows[i].outer,secs>0 ? rows[i].n/secs : 0,(unsigned long long)rows[i].n);
            }
            uint64_t a=__atomic_load_n(&ph_steps[s],__ATOMIC_RELAXED),da=a-last_ph_steps[s]; last_ph_steps[s]=a;
            log_printf("P-Ph %.1fs gate=%s scene_calls=%llu configured_substeps/scene_tick=%.4f actual_body_substeps=%llu body_substeps/H1_tick=%.4f substep_probe=%s overflow=%llu",seconds,states[s],
                (unsigned long long)total,total ? configured/total : 0,(unsigned long long)da,dt ? (double)da/dt : 0,contexts[8].active ? "ACTIVE" : "PASSIVE",
                (unsigned long long)__atomic_load_n(&ph_values.overflow,__ATOMIC_RELAXED));
        }
        if (gov_global) {
            unsigned n=collect(&gov_values,gov_last,s);
            uint64_t changes[3];
            for (unsigned i=0;i<3;i++) {
                uint64_t count=__atomic_load_n(&gov_counts[i][s],__ATOMIC_RELAXED);
                changes[i]=count-last_gov[i][s]; last_gov[i][s]=count;
            }
            log_printf("P-Gov %.1fs gate=%s global=0x%llX current=%u sampled_values=%u setting_writes=%llu fps_mode_changes=%llu speed_req_writes=%llu event_lost=%llu",
                seconds,states[s],rva(gov_global),*(volatile uint32_t *)gov_global,n,
                (unsigned long long)changes[0],(unsigned long long)changes[1],(unsigned long long)changes[2],(unsigned long long)gov_lost);
            for (unsigned i=0;i<n;i++) log_printf("P-Gov gate=%s global_value=%llu samples=%llu",states[s],(unsigned long long)(rows[i].ret-1),(unsigned long long)rows[i].n);
        }
    }
}
