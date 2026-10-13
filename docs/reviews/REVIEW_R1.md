# Review round 1: issues raised against plan v1

Whole-plan reviewer:
1. MAJOR: the S1 census misses 56 inline +0x614 reads.
2. MAJOR: raw game-speed readers (0x42dde4, 0x434df4, 0x10f7de3, 0x26f4768/80, 0x2a0a44) will see a jittering speed.
3. MAJOR: the third timestep variant 0x46398 and the duplicate 0xe3a750 have no direct callers; 0x5adc64 copies the clock into another object.
4. MAJOR: threading is not established; need an overlap counter, and the patch must be installed safely.
5. MINOR: the entry hook must read +0x618, not +0x614.
6. MINOR: the return must be an absolute FF25 jump.
7. MINOR: init timing vs SteamStub; derive G from the AOB; load dinput8 lazily.
8. MINOR: F11 and hit-stop are not backed by the live log; capture an uncapped run and a hit-stop run.
9. MINOR: test gaps (F10 during hit-stop/pause, photo mode, save/reload, 10-min drift, alt-tab while paused); add gate G1b.

Patch-site: MAJOR: read +0x618. MINOR: clear the dt history on fps change; the 60/fps field at 0x5ad7b8/825; stub encoding, NaN-safe clamp, RtlAddFunctionTable.
Threading: MAJOR: the clock runs on KIDSTaskScheduler workers, so use MinHook for P1 instead of a hand overwrite, plus an interlocked overlap counter. MINOR: +0x618; a 5-frame average drifts, so add error correction; optional xellSleep timestamp; frame gen means measure at sim ticks.
Delivery: MAJOR: put scale on an RW page adjacent to the stub page; absolute return jmp. MINOR: readiness poll 50ms/120s with byte check + VirtualQuery; lazy-load dinput8 stubs; .def naming @1–@6, PRIVATE; static CRT and objdump check; MH_Initialize in the init thread.
Sites: G1 = YES. The camera uses the unscaled dt = 60/S.fps from main update 0x39e4e4 (at 0x39e526), which also feeds ~25 other systems. The +0x634 countdown is a cosmetic history reset.
