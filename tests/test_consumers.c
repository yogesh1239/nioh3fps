/* Reuse the all-register/AVX/unaligned/unwind harness. No game code runs. */
#define main original_thunk_test_main
#define log_printf ignored_log_printf
#include "test_thunk.c"
#undef log_printf
#undef main
void log_printf(const char *fmt,...);
#include "../src/toggle_file.h"
#include "../src/consumer_probe.c"
#include <math.h>
#include <stdarg.h>
static unsigned unknown_logs;
void log_printf(const char *fmt,...)
{
    if (strstr(fmt,"UNKNOWN HOT CALLER")) unknown_logs++;
    va_list args; va_start(args,fmt); vprintf(fmt,args); va_end(args); puts("");
}
volatile LONG g_toggle=1,g_l3_tick;
static int test_tls;
int gate_tls_get(void) { return test_tls; }
uintptr_t g_clock_global;
void crash_add_range(uintptr_t p,size_t n,const char *s) { (void)p; (void)n; (void)s; }
static cp_table slots;
static int native_fps(void) { return 120; }
static float native_x(void) { return 1.75f; }
void cf_test_fps(void),cf_test_x(void);
__asm__(".text\n.globl cf_test_fps\n.seh_proc cf_test_fps\ncf_test_fps:\n.seh_endprologue\n"
        "call cf_fps_entry\njmp recorder\n.seh_endproc\n"
        ".globl cf_test_x\n.seh_proc cf_test_x\ncf_test_x:\n.seh_endprologue\n"
        "call cf_x_entry\njmp recorder\n.seh_endproc\n");
static DWORD WINAPI stress_publication(void *unused)
{
    (void)unused;
    for (uint64_t j=1;j<=100000;j++) consumer_fix_publish((j&1) ? 1.25f : 1.5f,j,1);
    return 0;
}
void cf_test_mid1(void),cf_test_mid2(void),cf_test_mid3(void),cf_test_mid4(void);
#define FIX_MID_TEST(n) __asm__(".text\n.globl cf_test_mid" #n "\n.seh_proc cf_test_mid" #n "\ncf_test_mid" #n ":\n.seh_endprologue\ncall cf_mid_leaf\n.byte 0xe9\n.long recorder-.-4\n.space 15\n.long " #n "\n.seh_endproc\n");
FIX_MID_TEST(1)
FIX_MID_TEST(2)
FIX_MID_TEST(3)
FIX_MID_TEST(4)
static void fix_units(void)
{
    uint8_t code[48]; memset(code,0xcc,sizeof code); code[0]=0xe8;
    uintptr_t target=(uintptr_t)code+40; int32_t disp=(int32_t)(target-(uintptr_t)code-5); memcpy(code+1,&disp,4);
    code[5]=0x90; code[6]=0x91; const char *pattern="E8 ?? ?? ?? ?? 90 91";
    CHECK(caller_return((uintptr_t)code,sizeof code,pattern,target)==(uintptr_t)code+5,"caller AOB and rel32 resolved");
    CHECK(!caller_return((uintptr_t)code,sizeof code,pattern,target+1),"wrong GetFps target rejected");
    memcpy(code+16,code,7); CHECK(!caller_return((uintptr_t)code,sizeof code,pattern,target),"ambiguous caller rejected");
    code[0]=code[16]=0xcc; CHECK(!caller_return((uintptr_t)code,sizeof code,pattern,target),"missing caller rejected");
    float factors[]={1,1.26f,1.5f,2.73f,0.9f};
    for (unsigned j=0;j<sizeof factors/sizeof *factors;j++) {
        double sum=0; int same=1;
        for (uint64_t seq=0;seq<100000;seq++) {
            int value=cf_rate(factors[j],seq,11,CF_RATE); sum+=value;
            for (unsigned actor=0;actor<12;actor++) same &= value==cf_rate(factors[j],seq,11,CF_RATE);
        }
        CHECK(fabs(sum/100000-120.0/factors[j])<0.00002,"dither mean converges for r=%.7g",(double)factors[j]);
        CHECK(same,"all actors in tick share dither value");
    }
    CHECK(cf_rate(1.26f,UINT64_MAX,7,CF_RATE)>=95 && cf_rate(1.26f,UINT64_MAX,7,CF_RATE)<=96,"seq wrap bounded");
    consumer_fix_publish(1.26f,UINT64_C(0x100000001),1); cf_tick old=consumer_fix_snapshot();
    int before=cf_rate(old.r,old.seq,9,CF_RATE);
    consumer_fix_publish(1.5f,old.seq+1,1); cf_tick current=consumer_fix_snapshot();
    CHECK(old.r==1.26f && old.seq==UINT64_C(0x100000001) && current.r==1.5f && current.seq==old.seq+1,"atomic seq boundary retains old/new pairs");
    CHECK(cf_rate(old.r,old.seq,9,CF_RATE)==before && cf_rate(current.r,current.seq,9,CF_RATE)==80,"late job keeps snapshot result");
    consumer_fix_publish(1.25f,1,1);
    HANDLE thread=CreateThread(NULL,0,stress_publication,NULL,0,NULL); int coherent=1;
    while (WaitForSingleObject(thread,0)==WAIT_TIMEOUT) {
        cf_tick t=consumer_fix_snapshot(); coherent &= t.armed==1 && t.r==((t.seq&1) ? 1.25f : 1.5f);
    }
    CloseHandle(thread); CHECK(coherent,"concurrent publication never mixes r and seq");
    double ideal=0,actual=0; int shared=1;
    consumer_fix_publish(1,0,0);
    for (uint64_t seq=1;seq<=100000;seq++) {
        float r=(seq&1) ? 120.0f/95.5f : 1.25f;
        consumer_fix_publish(r,seq,1); cf_tick t=consumer_fix_snapshot();
        int value=cf_tick_rate(t,3,CF_RATE); ideal+=120.0/r; actual+=value;
        for (unsigned actor=0;actor<12;actor++) shared &= value==cf_tick_rate(t,3,CF_RATE);
    }
    CHECK(fabs(actual-ideal)<1,"published error diffusion converges with alternating r");
    CHECK(shared,"published phase gives all actors same value");
    CHECK(cf_rate(1.26f,1,0,CF_ROUND)==95 && cf_rate(1.26f,98765,0,CF_ROUND)==95,"round capacity independent of seq");
    CHECK(cf_rate(120.0f/95.6f,42,99,CF_ROUND)==96,"round nearest upper value");
    CHECK(cf_rate(1.26f,5,0,CF_FLOAT)==120 && cf_rate(1.26f,5,0,CF_DENY)==120,"float/native mode policy");
    CHECK(cf_x_speed(1.75f,0.5f,1)==0.5f,"X uses exact requested speed");
    CHECK(cf_x_speed(0,2,1)==0 && cf_x_speed(-0.0f,2,1)==0,"X paused effective zero");
    CHECK(cf_x_speed(1.75f,0.5f,0)==1.75f,"X unarmed native speed");
    cf_site sites[]={{0x100,CF_RATE,1,0},{0x200,CF_ROUND,0,1},{0x300,CF_DENY,0,2}};
    unsigned char placed[]={1,0,1};
    CHECK(cf_find(sites,3,0x100)==0 && cf_find(sites,3,0x300)==2 && cf_find(sites,3,0x150)==-1,"binary allowlist/denylist lookup");
    CHECK(cf_core_ok(sites,placed,3),"rare failure leaves core ready"); placed[0]=0;
    CHECK(!cf_core_ok(sites,placed,3),"one core failure disables whole tier");
    double hot=0; int logged=0;
    for (int i=0;i<4;i++) logged|=cf_guard(&hot,11,1,1,0);
    CHECK(!logged && cf_guard(&hot,11,1,1,0),"unknown >10/s logs at five seconds");
    hot=4; CHECK(!cf_guard(&hot,10,1,1,0) && hot==0,"exactly ten is not hot");
    hot=4; CHECK(!cf_guard(&hot,100,1,0,0) && hot==0,"OFF clears hot streak");
    hot=4; CHECK(!cf_guard(&hot,100,1,1,1) && hot==0,"known A/C excluded");
    hot=4; CHECK(!cf_guard(&hot,100,0,1,0) && hot==0,"zero exposure cannot advance guard");
    hot=0; logged=0; for (int i=0;i<9;i++) logged|=cf_guard(&hot,6,0.5,1,0);
    CHECK(!logged && cf_guard(&hot,6,0.5,1,0),"guard uses elapsed seconds");
}
static void compare_unwind(uint8_t *img,size_t size,unsigned id)
{
    const cp_context *c=&fix_contexts[id]; uint8_t flat[512]; uint32_t frame;
    CHECK(unwind(img,size,c->site,flat,&frame),"flatten actual host unwind for fix %u",id);
    IMAGE_NT_HEADERS64 *nt=(void *)(img+((IMAGE_DOS_HEADER *)img)->e_lfanew);
    IMAGE_DATA_DIRECTORY d=nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
    RUNTIME_FUNCTION *native=(void *)(img+d.VirtualAddress),*host=NULL;
    for (unsigned j=0;j<d.Size/sizeof *native;j++) if (g_base+native[j].BeginAddress<=c->site && c->site<g_base+native[j].EndAddress) host=&native[j];
    CHECK(host!=NULL,"host unwind entry found for fix %u",id); if (!host) return;
    uint8_t *allocation=VirtualAlloc(NULL,4096,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);
    uint64_t *stack=calloc(32768,sizeof *stack);
    memcpy(allocation+256,flat,stubs_unwind_len(flat)); RUNTIME_FUNCTION rf={0,1,256};
    for (unsigned j=0;j<32768;j++) stack[j]=0x123456780000ull+j*16;
    CONTEXT a={0},b={0}; a.Rsp=(uintptr_t)(stack+128); a.Rip=c->site; a.ContextFlags=CONTEXT_FULL;
    a.Rbp=a.Rsp+((flat[3]>>4)*16); b=a;
    PVOID handler=NULL; DWORD64 fa=0,fb=0;
    RtlVirtualUnwind(0,g_base,c->site,host,&a,&handler,&fa,NULL);
    b.Rip=(uintptr_t)allocation;
    RtlVirtualUnwind(0,(uintptr_t)allocation,b.Rip,&rf,&b,&handler,&fb,NULL);
    CHECK(a.Rip==b.Rip && a.Rsp==b.Rsp && fa==fb && a.Rbx==b.Rbx && a.Rbp==b.Rbp && a.Rsi==b.Rsi && a.Rdi==b.Rdi &&
          a.R12==b.R12 && a.R13==b.R13 && a.R14==b.R14 && a.R15==b.R15 && !memcmp(&a.Xmm6,&b.Xmm6,10*sizeof a.Xmm6),
          "flattened unwind matches native chained unwind for fix %u",id);
    free(stack); VirtualFree(allocation,0,MEM_RELEASE);
}
static void runtime_guard_tests(void)
{
    unsigned before=unknown_logs; tick_state=CP_ON;
    uintptr_t unknown=g_base+0x1111;
    int a=cp_slot_hit(&fps,unknown,0,CP_ON),b=-1;
    for (unsigned j=0;j<CP_SLOTS;j++) if (!fps.slot[j].ret) { b=(int)j; break; }
    CHECK(a>=0 && b>=0,"unknown caller duplicate-slot fixture");
    fps.slot[b].ret=unknown; fps.slot[b].outer=0;
    /* Saturating writers may claim duplicate slots: volume must be merged. */
    for (unsigned second=0;second<6;second++) {
        fps.slot[a].count[CP_ON]+=6; fps.slot[b].count[CP_ON]+=6;
        for (unsigned k=0;k<11;k++) cp_slot_hit(&helper_calls,helper_ret,g_base+0x2222,CP_ON);
        for (unsigned k=0;k<100;k++) cp_slot_hit(&fps,known[19],0,CP_ON);
        for (unsigned k=0;k<100;k++) cp_slot_hit(&fps,known[40],0,CP_ON);
        for (unsigned k=0;k<100;k++) cp_slot_hit(&helper_calls,helper_ret,g_base+0x3992de,CP_ON);
        consumer_probe_second(1);
        if (second<4) CHECK(unknown_logs==before,"unknown guard waits five armed seconds %u",second);
    }
    CHECK(unknown_logs==before+2,"unknown direct and helper outer log; known hot A stays silent");
    tick_state=CP_OFF; consumer_probe_second(1);
    tick_state=CP_ON;
    for (unsigned k=0;k<20;k++) cp_slot_hit(&fps,unknown,0,CP_ON);
    consumer_probe_second(1); CHECK(unknown_logs==before+2,"OFF resets runtime streak");
    tick_state=CP_UNARMED;
}
static void fix_thunks(void)
{
    uint32_t native_mode=120, saved_offset=g_resolved.actual_fps;
    uintptr_t clockptr=(uintptr_t)&native_mode, saved_global=g_clock_global;
    g_clock_global=(uintptr_t)&clockptr; g_resolved.actual_fps=0;
    void *entries[]={cf_test_mid1,cf_test_mid2,cf_test_mid3,cf_test_mid4};
    g_consumers_ready=1; g_l3_tick=1; consumer_fix_publish(1.25f,25,1);
    for (unsigned alignment=0;alignment<2;alignment++) for (unsigned i=0;i<4;i++) {
        g_misalign=(uint8_t)alignment; g_tf=0;
        unsigned target=i==0 ? 13 : i==3 ? 1 : 6;
        fill_in(0x12345678); float value=i==3 ? 4 : 0.5f; memcpy(g_in.x[target],&value,4);
        g_entry=entries[i]; run_harness();
        float expected=value*(i==3 ? 0.8f : 1.25f); memcpy(g_in.x[target],&expected,4);
        CHECK(!memcmp(&g_in,&g_out,sizeof g_in),"inline %u changes only target scalar; alignment=%u",i,alignment);
        CHECK(g_flags_in==g_flags_out,"fix helper preserves flags %u/%u",i,alignment);
        g_l3_tick=0;
        CHECK(transparent("OFF fix helper",entries[i],0x12345678),"OFF native registers exact %u/%u",i,alignment);
        g_l3_tick=1; test_tls=1;
        CHECK(transparent("reentrant fix helper",entries[i],0x12345678),"TLS reentrant native exact %u/%u",i,alignment); test_tls=0;
    }
    g_clock_global=saved_global; g_resolved.actual_fps=saved_offset;
    g_consumers_ready=0; g_l3_tick=0;
    cp_fps_tramp=(void *)native_fps; x_tramp=(void *)native_x;
    for (unsigned alignment=0;alignment<2;alignment++) {
        g_misalign=(uint8_t)alignment; fill_in(0x12345678); g_entry=cf_test_fps; run_harness();
        g_in.g[0]=120;
        CHECK(!memcmp(&g_in,&g_out,sizeof g_in),"GetFps preserving entry changes only returned integer %u",alignment);
        CHECK(g_flags_in==g_flags_out,"GetFps entry preserves flags %u",alignment);
        fill_in(0x12345678); g_entry=cf_test_x; run_harness(); float result=1.75f; memcpy(g_in.x[0],&result,4);
        CHECK(!memcmp(&g_in,&g_out,sizeof g_in),"X preserving entry changes only returned scalar %u",alignment);
        CHECK(g_flags_in==g_flags_out,"X entry preserves flags %u",alignment);
    }
}
void cp_test_mid(void);
__asm__(".text\n.globl cp_test_mid\n.seh_proc cp_test_mid\ncp_test_mid:\n.seh_endprologue\n"
        "call cp_mid_leaf\n.byte 0xe9\n.long recorder-.-4\n.space 15\n.long 2\n.seh_endproc\n");
static DWORD WINAPI stress_slots(void *arg)
{
    for (int i=0;i<10000;i++) cp_slot_hit(&slots,0x1234,0x4567,(unsigned)(uintptr_t)arg);
    return 0;
}
static void unit_tests(void)
{
    CHECK(cp_steps_per_second(600,1200,1200,10)==60,"120 Hz normalisation");
    CHECK(cp_steps_per_second(12000,24000,1200,10)==60,"actor count cancels");
    CHECK(fabs(cp_steps_per_second(600,950,950,10)-60)<1e-10,"95 Hz normalisation");
    CHECK(fabs(cp_steps_per_second(758,950,950,10)-75.8)<1e-10,"X double scaling visible");
    CHECK(cp_steps_per_second(475,950,950,10)==47.5,"override B visible");
    CHECK(cp_steps_per_second(0,0,950,10)==0,"zero calls");
    CHECK(cp_steps_per_second(600,950,950,0)==0,"zero seconds");
    CHECK(cp_steps_per_second(600,950,950,-1)==0,"negative seconds");
    CHECK(cp_steps_per_second(600,950,0,10)==0,"zero ticks");
    int i=cp_slot_hit(&slots,123,456,CP_ON);
    CHECK(i>=0 && slots.slot[i].count[CP_ON]==1,"claim slot");
    CHECK(cp_slot_hit(&slots,123,456,CP_ON)==i && slots.slot[i].count[CP_ON]==2,"same key counted");
    CHECK(cp_slot_hit(&slots,123,456,CP_OFF)==i && slots.slot[i].count[CP_OFF]==1,"gate split");
    CHECK(cp_slot_hit(&slots,123,457,CP_ON)!=i,"outer key split");
    CHECK(cp_slot_hit(&slots,0,0,CP_ON)==-1,"zero sentinel rejected");
    CHECK(cp_slot_hit(&slots,123,456,CP_STATES)==-1,"invalid state rejected");
    HANDLE threads[9];
    for (unsigned j=0;j<9;j++) threads[j]=CreateThread(NULL,0,stress_slots,(void *)(uintptr_t)(j%3),0,NULL);
    WaitForMultipleObjects(9,threads,TRUE,INFINITE);
    uint64_t sum[3]={0};
    for (unsigned j=0;j<CP_SLOTS;j++) if (slots.slot[j].ret==0x1234 && slots.slot[j].outer==0x4567)
        for (unsigned s=0;s<3;s++) sum[s]+=slots.slot[j].count[s];
    for (unsigned j=0;j<9;j++) CloseHandle(threads[j]);
    CHECK(sum[0]==30000 && sum[1]==30000 && sum[2]==30000,"multi-thread counts exact");
    memset(&slots,0,sizeof slots); uint64_t counted=0;
    for (unsigned j=1;j<CP_SLOTS*2;j++) cp_slot_hit(&slots,0x10000+16*j,0,CP_ON);
    for (unsigned j=0;j<CP_SLOTS;j++) counted+=slots.slot[j].count[CP_ON];
    CHECK(counted+slots.overflow==CP_SLOTS*2-1 && slots.overflow>0,"bounded overflow accounts for all calls");
    uint8_t code[]={0x48,0x83,0xec,0x28,0xe8,0,0,0,0};
    uintptr_t ret=0; unsigned frame=0; int32_t d=0x1000-0x2009; memcpy(code+5,&d,4);
    CHECK(cp_helper_decode(code,9,0x2000,0x1000,&ret,&frame) && ret==0x2009 && frame==0x28,"decode rel32/frame");
    uintptr_t stack[32]={0}; stack[0]=ret; stack[(frame+8)/8]=0x7777;
    CHECK(cp_outer_return(stack,ret,frame)==0x7777,"outer at rsp+decoded frame+8");
    stack[0]++;
    CHECK(cp_outer_return(stack,ret,frame)==0,"other first-level ret never decoded");
    CHECK(cp_outer_return(stack,0,frame)==0,"passive helper");
    code[3]=0x38;
    CHECK(cp_helper_decode(code,9,0x2000,0x1000,&ret,&frame) && frame==0x38,"frame movement decoded");
    stack[0]=ret; stack[(frame+8)/8]=0x8888;
    CHECK(cp_outer_return(stack,ret,frame)==0x8888,"synthetic moved frame");
    CHECK(!cp_helper_decode(code,8,0x2000,0x1000,&ret,&frame),"short helper rejected");
    CHECK(!cp_helper_decode(code,9,0x2000,0x1001,&ret,&frame),"wrong target rejected");
    code[3]=0xf8;
    CHECK(!cp_helper_decode(code,9,0x2000,0x1000,&ret,&frame),"negative allocation rejected");
    toggle_file_edges edges={0}; int toggle=1;
    CHECK(toggle_file_edge(&edges,0)==-1 && toggle==1,"initial absence preserves StartEnabled");
    int edge=toggle_file_edge(&edges,1); if (edge>=0) toggle=edge;
    CHECK(toggle==1 && edge==1,"appearance ON"); toggle=!toggle;
    CHECK(toggle_file_edge(&edges,1)==-1 && toggle==0,"steady presence preserves F10 OFF");
    edge=toggle_file_edge(&edges,0); if (edge>=0) toggle=edge;
    CHECK(edge==0 && toggle==0,"disappearance OFF"); toggle=!toggle;
    CHECK(toggle_file_edge(&edges,0)==-1 && toggle==1,"steady absence preserves F10 ON");
    for (unsigned j=0;j<10;j++) CHECK(toggle_file_edge(&edges,!(j&1))==(int)!(j&1),"ten file transitions %u",j);
}
static void thunk_tests(void)
{
    cp_fps_tramp=cp_ph_tramp=cp_phstep_tramp=(void *)recorder;
    g_avx=IsProcessorFeaturePresent(PF_AVX_INSTRUCTIONS_AVAILABLE);
    for (unsigned alignment=0;alignment<2;alignment++) {
        g_misalign=(uint8_t)alignment; g_tf=0;
        void *entries[]={cp_fps_entry,cp_ph_entry,cp_phstep_entry};
        for (unsigned j=0;j<3;j++) {
            CHECK(transparent("consumer",entries[j],0x12345678),"registers/stack unchanged %u/%u",alignment,j);
            CHECK(g_flags_in==g_flags_out,"RFLAGS preserved %u/%u",alignment,j);
            range_of(entries[j],&g_lo,&g_hi); g_steps=g_unwind_bad=0;
            g_tf=1; transparent("consumer unwind",entries[j],0x12345678); g_tf=0;
            CHECK(!g_unwind_bad && g_steps>0,"unwind every instruction %u/%u: %s",alignment,j,g_bad_msg);
        }
        CHECK(transparent("consumer mid helper",cp_test_mid,0x12345678),"mid helper preserves registers/stack %u",alignment);
        CHECK(g_flags_in==g_flags_out,"mid helper preserves branch flags %u",alignment);
    }
}
static void image_tests(const char *path)
{
    FILE *f=fopen(path,"rb"); CHECK(f!=NULL,"open static executable"); if (!f) return;
    fseek(f,0,SEEK_END); size_t n=(size_t)ftell(f); rewind(f); uint8_t *file=malloc(n);
    CHECK(fread(file,1,n,f)==n,"read executable"); fclose(f);
    IMAGE_NT_HEADERS64 *nt=(void *)(file+((IMAGE_DOS_HEADER *)file)->e_lfanew);
    size_t size=nt->OptionalHeader.SizeOfImage;
    uint8_t *img=VirtualAlloc(NULL,size,MEM_COMMIT|MEM_RESERVE,PAGE_EXECUTE_READWRITE);
    CHECK(img!=NULL,"allocate image copy"); if (!img) { free(file); return; }
    memcpy(img,file,nt->OptionalHeader.SizeOfHeaders); IMAGE_SECTION_HEADER *sec=IMAGE_FIRST_SECTION(nt);
    for (unsigned j=0;j<nt->FileHeader.NumberOfSections;j++) memcpy(img+sec[j].VirtualAddress,file+sec[j].PointerToRawData,sec[j].SizeOfRawData);
    CHECK(resolve_image(img,size,&g_resolved),"resolve S2L dependencies"); g_base=(uintptr_t)img; g_clock_global=g_resolved.clock_global;
    CHECK(MH_Initialize()==MH_OK,"MinHook static placement only");
    consumer_probe_install(img,size); /* Hooks stay disabled; no game code runs. */
    CHECK(consumer_fix_install(img,size),"all S2C-F core hooks placed (disabled)");
    CHECK(g_consumers_ready,"consumer core readiness");
    for (unsigned j=0;j<CF_PATCHES;j++) CHECK(fix_contexts[j].active,"place fix %s",cf_patches[j].name);
    for (unsigned j=1;j<CF_PATCHES;j++) compare_unwind(img,size,j);
    CHECK(direct_count==CP_KNOWN && outer_count==CF_OUTERS,"complete sorted site tables");
    for (unsigned j=0;j<CF_OUTERS;j++) CHECK(outer_sites[j].ret!=0,"outer caller uniquely resolved %u",j);
    for (unsigned j=0;j<sizeof cf_directs/sizeof *cf_directs;j++) {
        const cf_direct *d=&cf_directs[j]; int index=cf_find(direct_sites,direct_count,known[d->index]);
        CHECK(index>=0 && direct_sites[index].policy==d->policy && direct_sites[index].core==d->core,"every B/B? row resolved %u",d->index);
    }
    uint8_t clock[0x630]={0}; uintptr_t clockptr=(uintptr_t)clock,saved_global=g_clock_global;
    g_clock_global=(uintptr_t)&clockptr; *(int32_t *)(clock+g_resolved.actual_fps)=120;
    g_l3_tick=1; consumer_fix_publish(1.25f,100,1); cf_tick tick=consumer_fix_snapshot();
    CHECK(known[40]==g_base+0x2978676 && effective_fps(known[40],0,120,tick)==120,
          "input-hold caller AOB resolved and GetFps remains native");
    CHECK(effective_fps(known[37],0,120,tick)==120 && effective_fps(known[38],0,120,tick)==120,"mode denylist stays exactly 120");
    CHECK(effective_fps(known[2],0,120,tick)==96,"capacity rounded");
    CHECK(effective_fps(helper_ret,g_base+0x42ea6b,120,tick)==96 && effective_fps(helper_ret,g_base+0x434e5a,120,tick)==96,"B helper outer callers scaled");
    CHECK(effective_fps(helper_ret,g_base+0x43b822,120,tick)==120 && effective_fps(helper_ret,g_base+0x43babd,120,tick)==120,"C helper constructors native");
    static const uint32_t b_outers[]={0x42ea6b,0x434e5a,0x43bf01,0x43bf1a,0x7accd9,0x5ea556,0x689caa};
    static const uint32_t ac_outers[]={0x43b822,0x43babd,0x398e1f,0x3992de,0x5bc62d,0x5e969e,
        0x9fd67d,0x9fd824,0x2042093,0x23ad9a3,0x23ad9bd,0x23adaa5,0x23adabf};
    CHECK(CF_OUTERS==20,"all static helper outers classified");
    for (unsigned j=0;j<sizeof b_outers/sizeof *b_outers;j++) {
        int index=cf_find(outer_sites,outer_count,g_base+b_outers[j]);
        CHECK(index>=0 && outer_sites[index].policy==CF_RATE && outer_sites[index].core==(j<2),
              "B outer %x has dither and volume-based tier",b_outers[j]);
        CHECK(effective_fps(helper_ret,g_base+b_outers[j],120,tick)==96,"B outer %x scaled",b_outers[j]);
    }
    for (unsigned j=0;j<sizeof ac_outers/sizeof *ac_outers;j++) {
        int index=cf_find(outer_sites,outer_count,g_base+ac_outers[j]);
        CHECK(index>=0 && outer_sites[index].policy==CF_KNOWN && !outer_sites[index].core,
              "A/C outer %x known rare",ac_outers[j]);
        CHECK(effective_fps(helper_ret,g_base+ac_outers[j],120,tick)==120,"A/C outer %x native",ac_outers[j]);
    }
    /* The new rare callers must reject a unique AOB with a wrong E8 target. */
    uintptr_t ts=0; size_t tn=0;
    for (unsigned j=0;j<nt->FileHeader.NumberOfSections;j++) if (!memcmp(sec[j].Name,".text",6)) {
        ts=g_base+sec[j].VirtualAddress; tn=sec[j].Misc.VirtualSize;
    }
    CHECK(ts && tn,"fixture text bounds");
    for (unsigned j=0;j<2;j++) {
        uint8_t *disp_at=img+b_outers[5+j]-4; int32_t saved,wrong;
        memcpy(&saved,disp_at,4); wrong=saved+1; memcpy(disp_at,&wrong,4);
        CHECK(!caller_return(ts,tn,cf_outers[7+j].pattern,contexts[1].match),"new rare outer wrong helper target rejected %u",j);
        memcpy(disp_at,&saved,4);
    }
    CHECK(effective_fps(helper_ret,g_base+0xabcdef,120,tick)==120,"unknown helper outer pass-through");
    CHECK(effective_fps(known[15],0,120,tick)==120 && fabs(4*consumer_mid_factor(4)-3.2f)<1e-6,"float threshold avoids int quantisation");
    CHECK(effective_fps(known[35],0,120,tick)==120,"override path remains native");
    CHECK(effective_fps(known[1],0,60,tick)==60,"non-120 native values retained");
    *(int32_t *)(clock+g_resolved.actual_fps)=60;
    for (unsigned j=1;j<CF_PATCHES;j++) CHECK(consumer_mid_factor(j)==1.0f,"non-120 mid fix is neutral %u",j);
    *(int32_t *)(clock+g_resolved.actual_fps)=120;
    g_l3_tick=0; CHECK(effective_fps(known[1],0,120,tick)==120 && consumer_mid_factor(1)==0,"OFF bypasses stale armed snapshot");
    void *saved_fps=cp_fps_tramp; cp_fps_tramp=(void *)native_fps; uintptr_t fps_stack[]={known[1]};
    CHECK(consumer_fps_value(fps_stack)==120,"P-G measures native OFF return");
    g_l3_tick=1; CHECK(consumer_fps_value(fps_stack)==96,"P-G measures effective ON return"); cp_fps_tramp=saved_fps;
    void *saved_x=x_tramp; x_tramp=(void *)native_x;
    g_clock_global=(uintptr_t)&clockptr; *(float *)(clock+g_resolved.eff)=1.25f; *(float *)(clock+g_resolved.req)=0.375f;
    CHECK(consumer_x_value()==0.375f,"X exact pre-P1 value from decoded fields");
    *(float *)(clock+g_resolved.eff)=0; CHECK(consumer_x_value()==0,"X preserves pause");
    g_l3_tick=0; CHECK(consumer_x_value()==1.75f,"X OFF calls native leaf"); x_tramp=saved_x; g_clock_global=saved_global;
    for (unsigned j=0;j<CP_ANCHORS;j++) {
        CHECK(contexts[j].match!=0,"unique anchor %s",cp_anchors[j].name);
        if (cp_anchors[j].off>=0) CHECK(contexts[j].active,"place %s",cp_anchors[j].name);
        printf("  %s: %s\n",cp_anchors[j].name,cp_anchors[j].off<0 ? "RESOLVED" : contexts[j].active ? "PLACED" : "PASSIVE");
    }
    CHECK(helper_frame==0x28 && helper_ret==g_base+0x42f105,"real helper decode");
    CHECK(chr_offset==0x8e0 && ph_offset==0x648 && ph_count_offset==0x664,"real field decode");
    CHECK(ph_ret==g_base+0x197cf3c && gov_global==g_base+0x45baf58,"physics caller/global decode");
    for (unsigned j=0;j<CP_KNOWN;j++) CHECK(known[j]!=0,"known caller %u",j);
    runtime_guard_tests();
    MH_Uninitialize(); free(file); VirtualFree(img,0,MEM_RELEASE);
}
static void record_only_tests(void)
{
    uint8_t chr[0x900]={0},self[64]={0},x[96]={0};
    uint64_t saved[16]={0},*r=saved+1; uintptr_t stack[]={0x1111,0x2222};
    float producer=0.631f,extra=0.125f,speed=1.0f,next=0.5f;
    uintptr_t clock=(uintptr_t)chr;
    memcpy(chr+0x8e0,&extra,4); memcpy(self,&clock,8);
    r[11]=(uintptr_t)&producer; r[6]=clock; r[8]=(uintptr_t)self;
    x_ready=1; chr_offset=0x8e0; tick_state=CP_ON;
    chr[0]=1; saved[0]=0; r[14]=3;
    consumer_probe_hit(2,r,x,stack); consumer_probe_hit(3,r,x,stack);
    CHECK(calls[0][CP_ON]==1 && steps[0][CP_ON]==3,"X path counts actual accumulator result");
    chr[0]=0; saved[0]=0x40; r[14]=2;
    consumer_probe_hit(2,r,x,stack); consumer_probe_hit(3,r,x,stack);
    CHECK(calls[1][CP_ON]==1 && steps[1][CP_ON]==2,"override path split by saved branch flags");
    CHECK(producer==0.631f && *(float *)(chr+0x8e0)==extra && *(uintptr_t *)self==clock,"P-X game operands unchanged");
    CHECK(fabs((double)x_inputs[0][CP_ON].sum[0]/16777216.0-producer)<1e-7 &&
          x_inputs[0][CP_ON].sum[1]==2097152,"P-X fixed-point samples retain dt and additive term");
    x_stat unusual={0}; x_sample(&unusual,0,0xbf000000); x_sample(&unusual,1,0x7fc00000);
    CHECK(unusual.sum[0]==-8388608 && unusual.valid[0]==1 && unusual.valid[1]==0 && unusual.last[1]==0x7fc00000,
          "negative samples and non-finite raw values are recorded without FP arithmetic");
    x_ready=0; g_clock_global=(uintptr_t)&clock; g_resolved.req=0x628; g_resolved.fps=0x618;
    cp_context *c=&contexts[19]; c->field=0x628; c->base_reg=1; c->src_reg=1; c->xmm=1; c->caller_offset=0;
    memcpy(chr+0x628,&speed,4); memcpy(x+16,&next,4); r[13]=clock;
    uint64_t before=gov_write; consumer_probe_hit(19,r,x,stack);
    gov_event event=gov_ring[before&(GOV_EVENTS-1)];
    CHECK(gov_write==before+1 && event.caller==0x2222 && event.state==CP_ON && event.value==0x3f000000,"speed_req recorded with caller and exact float bits");
    CHECK(*(float *)(chr+0x628)==speed,"speed_req store is left to original game code");
    uint32_t mode=120; memcpy(chr+0x618,&mode,4);
    c=&contexts[15]; c->field=0x618; c->base_reg=0; c->src_reg=1; c->xmm=0; c->caller_offset=0;
    r[14]=clock; r[13]=60; before=gov_write; consumer_probe_hit(15,r,x,stack);
    event=gov_ring[before&(GOV_EVENTS-1)];
    CHECK(event.old==120 && event.value==60 && gov_write==before+1,"fps-mode transition recorded");
    CHECK(*(uint32_t *)(chr+0x618)==120,"fps-mode remains unchanged by probe");
    r[13]=120; before=gov_write; consumer_probe_hit(15,r,x,stack);
    CHECK(gov_write==before,"unchanged mode produces no transition");
    tick_state=CP_UNARMED;
}
int main(int argc,char **argv)
{
    if (argc!=2) return 2;
    unit_tests();
    fix_units();
    printf("consumer units complete (%d checks)\n",g_checks); fflush(stdout);
    PVOID handler=AddVectoredExceptionHandler(1,veh); thunk_tests(); RemoveVectoredExceptionHandler(handler);
    printf("consumer thunks complete\n"); fflush(stdout);
    fix_thunks();
    record_only_tests();
    image_tests(argv[1]);
    printf("%d checks, %d failures\nTEST_CONSUMERS: %s\n",g_checks,g_fail,g_fail ? "FAIL" : "PASS");
    return g_fail ? 1 : 0;
}
