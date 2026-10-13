#pragma once
#include "stubs.h"
#define HB_COUNT 4096
typedef struct {
    volatile uint64_t stamp;
    int64_t qpc;
    uint64_t packet, mailbox, seq;
    uint32_t tid, flags; /* bit 0 = other pair, bit 1 = repeat, bit 2 = null */
    uint32_t frame_no;
    uint8_t pad[12];
} hb_rec;
_Static_assert(sizeof(hb_rec)==64,"heartbeat cache line");
_Static_assert(__builtin_offsetof(hb_rec,frame_no)==48,"assembly frame offset");
extern hb_rec g_hb_ring[HB_COUNT];
extern volatile LONG64 g_hb_write, g_render_seq, g_render_qpc, g_render_conflict;
extern volatile LONG64 g_take_seq; /* physical selected packet takes, including repeats */
extern volatile uintptr_t g_expected_mailbox, g_previous_packet;
extern volatile uint32_t g_previous_frame;
extern volatile uintptr_t g_published_mailbox; /* advisory latest H1-owned publish */
extern volatile LONG64 g_hb_relatches;
extern volatile LONG g_render_tid;
extern DWORD g_gate_tls;
extern int g_heartbeat_source;
extern void *g_qpc_fn;
void heartbeat_leaf(void);
int heartbeat_build(uint8_t *alloc, uintptr_t site, const uint8_t *host_ui,
                    RUNTIME_FUNCTION **table, char *err, size_t n);
int gate_tls_get(void);
void gate_tls_set(int v);
