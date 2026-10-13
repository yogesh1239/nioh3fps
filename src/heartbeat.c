#include "common.h"
#include "heartbeat.h"
#include <string.h>
#include <stdio.h>
hb_rec g_hb_ring[HB_COUNT] __attribute__((aligned(64)));
volatile LONG64 g_hb_write, g_render_seq, g_render_qpc, g_render_conflict;
volatile LONG64 g_take_seq;
volatile uintptr_t g_expected_mailbox, g_previous_packet;
volatile uint32_t g_previous_frame;
volatile uintptr_t g_published_mailbox;
volatile LONG64 g_hb_relatches;
volatile LONG g_render_tid;
DWORD g_gate_tls=TLS_OUT_OF_INDEXES;
int g_heartbeat_source;
void *g_qpc_fn=(void *)QueryPerformanceCounter;
/* Native TLS, deliberately avoiding GCC's emutls allocator on game threads.
   Only the Windows x64 TEB layout is used here, never a game field offset. */
int gate_tls_get(void)
{
    uintptr_t teb; __asm__("movq %%gs:0x30,%0":"=r"(teb));
    return g_gate_tls<64 ? (int)*(uintptr_t *)(teb+0x1480+8*g_gate_tls) : 0;
}
void gate_tls_set(int v)
{
    uintptr_t teb; __asm__("movq %%gs:0x30,%0":"=r"(teb));
    if (g_gate_tls<64) *(uintptr_t *)(teb+0x1480+8*g_gate_tls)=(uintptr_t)v;
}
int heartbeat_build(uint8_t *a, uintptr_t site, const uint8_t *ui,
                    RUNTIME_FUNCTION **tbl, char *err, size_t n)
{
    size_t len; int32_t d; uintptr_t fn=(uintptr_t)heartbeat_leaf;
    /* Exactly one complete, non-RIP-relative instruction. Refuse another
       layout instead of copying partial instructions or changing stack bases. */
    if (memcmp((void *)site,"\x48\x8B\x4C\x24\x78",5)) {
        snprintf(err,n,"unsupported gameplay take instruction"); return -1;
    }
    if (stubs_transform_unwind(ui,a+0x100,&len,err,n)) return -1;
    /* Original instruction uses the original host rsp. The call gives the
       separately unwindable helper a normal return address. Conditional jumps
       avoid the unwinder's tail-jump epilogue heuristic without a scratch GPR. */
    memcpy(a,(void *)site,5);
    memcpy(a+5,"\xFF\x15",2); d=12; memcpy(a+7,&d,4); /* pointer at +23 */
    memcpy(a+11,"\x0F\x84",2); d=(int32_t)(site+5-(uintptr_t)(a+17)); memcpy(a+13,&d,4);
    memcpy(a+17,"\x0F\x85",2); d=(int32_t)(site+5-(uintptr_t)(a+23)); memcpy(a+19,&d,4);
    memcpy(a+23,&fn,8);
    *tbl=(void *)(a+0x200);
    (*tbl)[0]=(RUNTIME_FUNCTION){0,23,0x100};
    return 0;
}
