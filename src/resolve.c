/* All installer addresses/operands originate in unique .text matches. */
#include "common.h"
#include "resolve.h"
#include "anchors.h"
#include <string.h>
resolved_t g_resolved;
const char *resolve_name(int i) { return anchors[i].name; }
size_t resolve_span(int i) { aob_t a; return aob_parse(anchors[i].pattern,&a) ? 0 : (size_t)a.n; }
uint32_t resolve_hint(int i) { return anchors[i].hint; }
static int32_t d32(uintptr_t p) { int32_t v; memcpy(&v, (void *)p, 4); return v; }
static uintptr_t rip(uintptr_t p, int off, int end) { return p + end + d32(p + off); }
static int inside(uintptr_t p, uintptr_t b, size_t n, size_t len) { return p >= b && p-b <= n && len <= n-(p-b); }
int resolve_image(uint8_t *image, size_t size, resolved_t *r)
{
    uintptr_t b = (uintptr_t)image, ts = 0; size_t tn = 0;
    IMAGE_DOS_HEADER *dos = (void *)image;
    IMAGE_NT_HEADERS64 *nt; IMAGE_SECTION_HEADER *s;
    memset(r, 0, sizeof *r);
    if (size < sizeof *dos || dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew < 0 ||
        (size_t)dos->e_lfanew + sizeof *nt > size) return 0;
    nt = (void *)(image + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE || nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) return 0;
    s = IMAGE_FIRST_SECTION(nt);
    if (!inside((uintptr_t)s, b, size, nt->FileHeader.NumberOfSections * sizeof *s)) return 0;
    for (int i=0; i<nt->FileHeader.NumberOfSections; ++i)
        if (!memcmp(s[i].Name, ".text", 6)) { ts=b+s[i].VirtualAddress; tn=s[i].Misc.VirtualSize; }
    if (!ts || !inside(ts,b,size,tn)) return 0;
    for (int i=0; i<A_COUNT; ++i) {
        aob_t a; uintptr_t hit=0;
        if (aob_parse(anchors[i].pattern,&a)) continue;
        r->hits[i]=aob_scan((void *)ts,tn,&a,&hit,1);
        if (r->hits[i]==1) { r->match[i]=hit; r->hook[i]=hit+anchors[i].hookoff; }
    }
    if (r->match[A_GETFPS]) {
        r->clock_global=rip(r->match[A_GETFPS],3,7);
        r->actual_fps=(uint32_t)d32(r->match[A_GETFPS]+14);
    }
    if (r->match[A_P1]) {
        uintptr_t p=r->match[A_P1];
        r->fps=(uint32_t)d32(p+3); r->req=(uint32_t)d32(p+0x36);
        r->eff=(uint32_t)d32(p+0x3e); r->pause_a=(uint32_t)d32(p+0x44);
        r->pause_b=(uint32_t)d32(p+0x57);
    }
    if (r->match[A_P2]) { r->c60=rip(r->match[A_P2],10,14); r->s_fps=*(uint8_t *)(r->match[A_P2]+2); }
    if (r->match[A_S_FIELDS]) {
        uintptr_t p=r->match[A_S_FIELDS];
        /* S begins at rbp-0x38 in this stack builder: obtain member differences
           from the three encoded stores, rather than assuming S+14/S+18. */
        int fps_disp=(int8_t)*(uint8_t *)(p+0x1d);
        r->s_req=r->s_fps+(int8_t)*(uint8_t *)(p+0x12)-fps_disp;
        r->s_eff=r->s_fps+(int8_t)*(uint8_t *)(p+0x2b)-fps_disp;
    }
    if (r->match[A_SCENE]) r->scene_global=rip(r->match[A_SCENE],3,7);
    if (r->match[A_XELL]) r->xell_iat=rip(r->match[A_XELL],2,6);
    if (r->hook[A_SLEEP]) r->sleep_iat=rip(r->hook[A_SLEEP],2,6);
    if (r->match[A_HELP]) r->help_jobs=rip(r->hook[A_HELP],1,5);
    if (r->match[A_ONLINE] && r->match[A_ONLINE2] && r->match[A_ONLINE1]) {
        uintptr_t p=r->match[A_ONLINE], two=r->match[A_ONLINE2], one=r->match[A_ONLINE1];
        r->online_global=rip(p,7,11);
        r->online_offset=*(uint8_t *)(p+0x14);
        /* All three independent predicates must agree on decoded operands,
           including IsOnlinePlay's call to the ==2 getter. */
        r->online=inside(r->online_global,b,size,8) && r->online_offset>0 &&
            !(r->online_offset&3) && rip(two,3,7)==r->online_global &&
            rip(one,3,7)==r->online_global && *(uint8_t *)(two+0xe)==r->online_offset &&
            *(uint8_t *)(one+0xe)==r->online_offset && rip(p+0x18,1,5)==two;
    }
    r->core=r->online && r->hook[A_CLOCK] && r->hook[A_H2] && r->hook[A_P1] && r->hook[A_P2] && r->match[A_S_FIELDS] &&
        inside(r->clock_global,b,size,8) && inside(r->c60,b,size,4) && *(float *)r->c60==60.0f &&
        r->actual_fps>0 && r->fps>0 && r->req>0 && r->eff>0 && r->fps<0x10000 && r->req<0x10000 && r->eff<0x10000 &&
        r->s_fps>0 && r->s_req>0 && r->s_eff>0 && r->s_req<0x1000 && r->s_eff<0x1000;
    r->heartbeat=r->hook[A_TAKE]!=0;
    r->xell=inside(r->xell_iat,b,size,8);
    r->gate=r->core && r->heartbeat && r->hook[A_PUBLISH] && r->hook[A_FRAME] &&
        r->hook[A_SCENE] && inside(r->scene_global,b,size,8) && inside(r->help_jobs,ts,tn,1) &&
        r->pause_a>0 && r->pause_b>0 && r->pause_a<0x10000 && r->pause_b<0x10000;
    return r->core;
}
