/* S2C-P: bounded, record-only counters. No game-thread logging or allocation. */
#pragma once
#include <stdint.h>
#include <stddef.h>
#define CP_SLOTS 2048
enum { CP_ON, CP_OFF, CP_UNARMED, CP_STATES };
typedef struct { uintptr_t ret, outer; uint64_t count[CP_STATES]; } cp_slot;
typedef struct { cp_slot slot[CP_SLOTS]; uint64_t overflow; } cp_table;
double cp_steps_per_second(uint64_t steps, uint64_t calls, uint64_t ticks, double seconds);
int cp_slot_hit(cp_table *table, uintptr_t ret, uintptr_t outer, unsigned state);
int cp_helper_decode(const uint8_t *code, size_t size, uintptr_t at,
                     uintptr_t getfps, uintptr_t *ret, unsigned *frame);
uintptr_t cp_outer_return(const void *rsp, uintptr_t helper_ret, unsigned frame);
void consumer_probe_install(uint8_t *image, size_t size);
void consumer_probe_tick(int64_t now);
void consumer_probe_second(double seconds);
void consumer_probe_drain(void); /* writer only, every 50 ms */
void consumer_probe_summary(double seconds);
