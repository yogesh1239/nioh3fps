/* Real mapped-image anchors, corruption, relocation and decoded-field controls. */
#include "../src/common.h"
#include "../src/resolve.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static int checks,fail;
#define CHECK(x) do { checks++; if (!(x)) { fail++; printf("FAIL line %d: %s\n",__LINE__,#x); } } while(0)
int main(int argc,char **argv)
{
    if (argc<2) return 2;
    FILE *f=fopen(argv[1],"rb"); if (!f) return 2;
    fseek(f,0,SEEK_END); size_t n=(size_t)ftell(f); rewind(f);
    uint8_t *file=malloc(n); if (fread(file,1,n,f)!=n) return 2; fclose(f);
    IMAGE_NT_HEADERS64 *nt=(void *)(file+((IMAGE_DOS_HEADER *)file)->e_lfanew);
    size_t size=nt->OptionalHeader.SizeOfImage;
    uint8_t *img=calloc(1,size+0x10000);
    memcpy(img,file,nt->OptionalHeader.SizeOfHeaders);
    IMAGE_SECTION_HEADER *sec=IMAGE_FIRST_SECTION(nt);
    for (int j=0;j<nt->FileHeader.NumberOfSections;j++) memcpy(img+sec[j].VirtualAddress,file+sec[j].PointerToRawData,sec[j].SizeOfRawData);
    uintptr_t b=(uintptr_t)img; resolved_t r;
    static const uint32_t hints[A_COUNT]={0x46594,0x5ad5f8,0x5ad670,0x39e4e4,0x39e520,0x46484,0x567070,
        0x20dda0c,0x5a6bb0,0x4a538c,0x3d20eb,0x19df57e,0x123aa20,0x412748,0x5acf80,0x5ac725,0x1a7494b,
        0x85834,0x85948,0x85930};
    CHECK(resolve_image(img,size,&r)); CHECK(r.gate && r.heartbeat && r.xell);
    for (int j=0;j<A_COUNT;j++) { CHECK(r.hits[j]==1 && r.match[j]==b+hints[j]); printf("%s unique at 0x%X\n",resolve_name(j),hints[j]); }
    CHECK(r.hook[A_P1]==b+0x5ad6a2 && r.hook[A_P2]==b+0x39e526);
    CHECK(r.hook[A_TAKE]==b+0x3d20eb && r.hook[A_HELP]==b+0x19df583);
    CHECK(r.hook[A_SLEEP]==b+0x1a74954);
    CHECK(r.help_jobs==b+0x916ebc && r.clock_global==b+0x4759910);
    CHECK(r.scene_global==b+0x45ce800 && r.c60==b+0x3bd4080);
    CHECK(r.online && r.online_global==b+0x4709b50 && r.online_offset==0x34);
    CHECK(r.xell_iat==b+0x38e2cf0 && r.sleep_iat==b+0x38e2338);
    CHECK(r.fps==0x618 && r.actual_fps==0x614 && r.req==0x628 && r.eff==0x624);
    CHECK(r.pause_a==0x58c && r.pause_b==0x58b && r.s_fps==0x10 && r.s_req==0x14 && r.s_eff==0x18);
    for (int j=0;j<A_COUNT;j++) {
        uint8_t old=img[hints[j]]; img[hints[j]]^=0xff;
        resolved_t neg; resolve_image(img,size,&neg);
        CHECK(neg.hits[j]==0 && !neg.hook[j]);
        if (j==A_TAKE) CHECK(!neg.heartbeat && !neg.gate && neg.core);
        if (j==A_HELP || j==A_SCENE || j==A_PUBLISH || j==A_FRAME) CHECK(!neg.gate && neg.core);
        if (j==A_XELL) CHECK(!neg.xell && neg.gate);
        if (j==A_SLEEP) CHECK(!neg.sleep_iat && neg.heartbeat && neg.gate);
        if (j==A_P1 || j==A_P2 || j==A_H2 || j==A_CLOCK || j==A_GETFPS || j==A_S_FIELDS) CHECK(!neg.core && !neg.gate);
        if (j==A_ONLINE || j==A_ONLINE2 || j==A_ONLINE1) CHECK(!neg.online && !neg.core && !neg.gate);
        img[hints[j]]=old;
    }
    /* A unique opcode match alone must not arm with an invalid rel32 target. */
    int32_t old; memcpy(&old,img+0x19df584,4);
    memset(img+0x19df584,0x7f,4); resolve_image(img,size,&r);
    CHECK(r.hits[A_HELP]==1 && !r.gate && r.core); memcpy(img+0x19df584,&old,4);
    /* All online failures are core-tier, even with unique opcode matches. */
    const uint32_t online_disp[]={0x8583b,0x8594b,0x85933,0x8584d};
    for (unsigned j=0;j<sizeof online_disp/sizeof *online_disp;j++) {
        uint32_t at=online_disp[j]; int32_t value;
        memcpy(&old,img+at,4); value=old+4; memcpy(img+at,&value,4);
        CHECK(!resolve_image(img,size,&r) && !r.online && !r.core && !r.gate);
        CHECK(r.hits[A_ONLINE]==1 && r.hits[A_ONLINE2]==1 && r.hits[A_ONLINE1]==1);
        memcpy(img+at,&old,4);
    }
    const uint32_t online_off[]={0x85848,0x85956,0x8593e};
    for (unsigned j=0;j<sizeof online_off/sizeof *online_off;j++) {
        uint8_t value=img[online_off[j]]; img[online_off[j]]=0x38;
        CHECK(!resolve_image(img,size,&r) && !r.online && !r.core && !r.gate);
        img[online_off[j]]=value;
    }
    /* An additional primary predicate in .text is ambiguous and must stay passive. */
    size_t online_len=resolve_span(A_ONLINE); uint8_t duplicate[64];
    CHECK(online_len<=sizeof duplicate);
    memcpy(duplicate,img+0x1000,online_len); memcpy(img+0x1000,img+0x85834,online_len);
    CHECK(!resolve_image(img,size,&r) && r.hits[A_ONLINE]==2 && !r.online && !r.core && !r.gate);
    memcpy(img+0x1000,duplicate,online_len);
    /* Shift every section equally; installer must accept the new addresses. */
    memmove(img+0x11000,img+0x1000,size-0x1000); memset(img+0x1000,0,0x10000);
    nt=(void *)(img+((IMAGE_DOS_HEADER *)img)->e_lfanew); sec=IMAGE_FIRST_SECTION(nt);
    for (int j=0;j<nt->FileHeader.NumberOfSections;j++) sec[j].VirtualAddress+=0x10000;
    nt->OptionalHeader.SizeOfImage+=0x10000;
    CHECK(resolve_image(img,size+0x10000,&r) && r.gate && r.xell);
    for (int j=0;j<A_COUNT;j++) CHECK(r.hits[j]==1 && r.match[j]==b+hints[j]+0x10000);
    CHECK(r.help_jobs==b+0x926ebc && r.clock_global==b+0x4769910 && r.c60==b+0x3be4080);
    CHECK(r.online && r.online_global==b+0x4719b50 && r.online_offset==0x34);
    /* Field movement is decoded, not compared with historical field offsets. */
    uint32_t moved=0x728; memcpy((void *)(r.hook[A_P1]+4),&moved,4);
    CHECK(resolve_image(img,size+0x10000,&r) && r.req==moved);
    printf("%d checks, %d failures\nTEST_RESOLVE: %s\n",checks,fail,fail ? "FAIL" : "PASS");
    free(file); free(img); return fail ? 1 : 0;
}
