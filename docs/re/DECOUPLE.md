# Nioh 3 update/render decoupling (2026-10-13)

## Live facts (measured)
- At ~74-94 rendered fps the update side runs a steady 120/s: clock fn 0x5ad5f8 (H1), main update 0x39e4e4 (H2), camera 0x567070.
- Rendered frames (xellSleep, render counters) run at the displayed rate. In the camera-sweep baseline: 1658 shown frames carried 1 tick and 670 carried 2; present interval median 10.66 ms; |game advance - real interval| mean 3.40 ms, p95 6.15 ms.
- Wo Long 2 at ~81 fps: one counter family at the displayed rate (lockstep is inferred).

## Static RE (subagent; RVAs)
- Render frame 0x4a529c (via GfxApp+0x110, 0x577e50, main loop 0x1a74130). It increments 0x45c3f48/0x45baa94, calls xellSleep at 0x4a538c and records the present at 0x7d9b6c.
- Update side: Game+8 update 0x5ac664, clock 0x5ad5f8. The 120/s counters are written at 0x5a6cbc (0x44b94a4), 0x287a41 (0x45c90c8) and 0x5aa7b0 (0x45c4140).
- Hand-off is a one-slot mailbox, publisher 0x412748 (verified).
  - If the previous packet is unconsumed (+0x138) and vfunc+0x38 returns false, the old packet is dropped and replaced. That drop is why some ticks never reach the screen.
  - If vfunc+0x38 returns true, the publisher spins with Sleep(0) at 0x19a525e until render takes the packet.
- Consumer: 0x1a74760, 0x3d1f20.
- 120 Hz pacer: NOT FOUND statically. 0x5af604 picks the rate from the {30,60,120} table (inferred).

## Candidate fixes (unranked until the pacer is found)
1. Force the mailbox to wait instead of drop (0x412748 / vfunc+0x38), plus a real-dt step in the clock. Needs a timeout fallback to drop.
2. Gate the next update on the packet being consumed (0x8fe3bc / 0x31a63c).
3. Pace the update from xellSleep with an event.
Risks: deadlock in loading/menu/null-packet paths; timers/AI that assume 120 ticks/s; network tick.

## Runtime: update-thread stack sample (tools/stacksample.py, 500 Hz, 10 s, at the shrine with a camera sweep)
- Update thread (tid of H1/H2): 38.5% of samples in Sleep(0) via 0x916ebc (job-system help/yield), called from 0x19df583, a cold chunk of the frame function 0x5acf80.
- Another ~15% spin in 0x41050 (job steal) from the same wait. The camera runs on a worker thread; XELL runs on the render thread.

## THE 120 Hz PACER (verified disasm, frame function 0x5acf80)
- r15 = timing struct ([rbp+0x60]+0x28). [r15+0x30] = target fps (min 1); rbx = QPC frequency ([[rip+0x400d548]+0x18]).
- period = freq / target_fps -> [rsp+0x30]; r12 = [r15] = previous frame QPC.
- Loop 0x5ad292..0x5ad2b0 + cold 0x19df57e: now = QPC (0x205530). While now - prev < period: call 0x916ebc (help/yield/Sleep(0)), then re-read QPC.
- Then [r15] = now; [r15+0x24] = (now - prev) / freq as float (measured real dt; the clock step ignores it); [r15+0x18] = [obj+0x44] or 1/fps.
- The update thread never blocks on render. The mailbox 0x412748 drops unconsumed packets, so update stays at target_fps (120) and render shows whatever is latest.

## Gameplay packet take (RE 2026-10-13, after the S2L run showed 0x1a7495f never fires in gameplay)
- 0x1a7495f belongs to the inline sync take (0x1a74760..0x1a748d6). It is used only when [app+0xd8] (0x8b4af4) test-and-clears bit 0 of app+0x1ee5c4, a one-shot sync/loading request (inferred).
- GAMEPLAY TAKE: 0x3d1f20, called only from 0x1a749cb in the loop 0x1a74130. It takes +0x138 into +0x140 under lock 0x10010000. Chain: 0x3d1f20 → 0x112dd8 (0x3d2115) → [app+0x110] = 0x577e50 → render frame 0x4a529c → xellSleep.
  - Render runs ONLY when a new packet arrived (sil = 1). Otherwise [app+0x28] (0x3d2180) returns without rendering.
- Recommended heartbeat: 0x3d20eb (`mov rcx,[rsp+0x78]`, 5 B), after the take and unlock and before the render call. AOB `48 8B 4C 24 78 45 8B CD 4C 8B 44 24 70 48 85 C9 48 8B D3 4C 0F 45 C1`. Registers: r12 = new packet, rbx = mailbox, rdi = app, r13d = packet frame number. On the frame-mismatch path (0x3d2158), r12 can be 0.
- Exact take instant: 0x3d2147 (AOB `49 8B 04 24 49 8B CC FF 50 18 33 D2 48 85 F6`), inside the lock, so record-only there.
- Fallbacks before xellSleep:
  - 0x577e50 entry. AOB `48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 48 83 EC 20 48 8B E9 41 8B F8 48 8B 89 70`. rcx = app, rdx = mailbox, r8d = frame number, r9 = packet.
  - 0x4a529c entry. AOB `48 89 5C 24 18 55 56 57 41 54 41 55 41 56 41 57 B8 90 3D 00 00 E8`. It is also slot +0x110 of vtable 0x40d4b88.
- The render thread's 47% sleep is a GPU fence (frame-latency) wait: waiter 0x7d9e1c on [dev+0x6d8] ID3D12Fence, called via 0x3d2b90 / 0x3d2650. It is not a packet wait.

## Gameplay heartbeat verification (2026-10-13, static unpacked exe only)
- The supplied AOB matches `.text` exactly once, at 0x3d20eb. A new +0x138 packet takes 0x3d2147 → 0x3d2168 (sil=1) → clear/transfer → unlock at 0x3d2003..0x3d2021 → 0x3d206d → 0x3d20eb → 0x3d2115. There is no backedge through the heartbeat: exactly one visit per new-packet render invocation, before 0x112dd8 → app+0x110 (0x577e50) → 0x4a529c.
- With no new packet and an equal retained frame, sil=0; gameplay app vtable 0x3aaf600 has +0x28=0x3d2180 (`return true`), so 0x3d204b/0x3d2050 returns without reaching the heartbeat/render. Exception: retained-frame mismatch at 0x3d1fdb → 0x3d2158 sets sil=1 with r12=0 and does reach it (hb_null). Other app types whose +0x28 returns false, or callers with a mismatching non-null r15 output, can also continue without a new packet; gameplay caller 0x1a749cb passes r9=0. Thus this site is not proof of a new packet unless r12 is non-null.

## S2L live result (build dab1b2d6, 2026-10-13, shrine, camera-only sweep)
- Publish 0x412748 runs on the RENDER thread (same TID as the 0x3d20eb heartbeat) with the same mailbox as rbx there, so it is never "owned" by the H1 task. Take mode now latches rate-based on (mailbox, tid).
- Gate armed 100%: every GATE_END consumed exactly seq+1. H1 starts 0.5-2 us after its HB. Gate wait is about 7.1 ms mean.
- 40 s sweep: M1 100%, M2 lagged 0.060/0.314 ms (floor 0.171/0.524), M3 94.51 vs 93.65 fps gate-idle, M5 game/real 0.99996.
- 5 min sweep: 0 timeouts, 0 backoffs, armed 100% in all 30 windows (M4).
