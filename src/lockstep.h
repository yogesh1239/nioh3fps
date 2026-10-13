#pragma once
#include "common.h"
#include "heartbeat.h"
#include "gate.h"
extern int g_l2_ready;
extern volatile LONG g_l3_tick, g_factor_epoch;
extern volatile LONG64 g_sleep_polls, g_sleep_qpc;
extern void *publish_tramp, *frame_tramp, *xell_tramp;
extern uintptr_t g_xell_return;
extern void (WINAPI *g_native_sleep)(DWORD);
uint64_t publish_detour(void *mailbox, void *a2, void *a3, void *a4);
uint64_t frame_detour(void *a1, void *a2, void *a3, void *a4);
uint64_t xell_detour(void *a1, void *a2, void *a3, void *a4);
void WINAPI sleep1_detour(DWORD ms);
int lockstep_h1(uint8_t *clk, void *worker, DWORD tid);
int lockstep_h2(DWORD tid); /* paired first H2 only; bypass paths stay unscaled */
void lockstep_init(void);
typedef struct {
    uint64_t calls, owned; /* cumulative; writer reports ten-second deltas */
    uintptr_t mailbox; /* last argument at entry, including unowned calls */
    DWORD tid[4];
    int tls[4]; /* latest entry TLS for each of the first four distinct TIDs */
} publish_diagnostics;
void lockstep_publish_snapshot(publish_diagnostics *out);
void heartbeat_drain_trace(void); /* writer only */
int sleep1_build(uint8_t *alloc, uintptr_t site, const uint8_t *ui,
                 RUNTIME_FUNCTION **tbl, char *err, size_t n);
