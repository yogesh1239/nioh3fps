# Nioh 3 variable-framerate mod: plan v8 (lockstep), a delta on plan v7.1

Status: v8.2: PASSED review r2 (r1 NO -> fixed; r2 YES with MINORs, folded in as section 6). Changes are marked [r1]/[r2].

## 0. Why v7.1 is not enough (C = confirmed live)
- C: below 120 fps the game does NOT slow down. The update thread is paced at exactly 120 Hz by its own QPC wait loop in frame fn `0x5acf80` (period = QPCfreq / [timing+0x30], wait loop `0x5ad292..0x5ad2b0` plus cold `0x19df57e`, yielding via `0x916ebc`).
- C: render presents at the GPU rate (74-94 fps). The one-slot mailbox `0x412748` drops stale packets, so each shown frame carries 1 or 2 fixed 8.33 ms steps.
- C, baseline at the shrine with a camera-only sweep: 1658 frames carried 1 tick and 670 carried 2; |game advance - present interval| mean 3.40 ms, p95 6.15 ms. That is the judder.
- C: in S1 the would-be r is about 1.0 (real dt ≈ 8.33 ms), so v7.1's P1/P2 alone change nothing.
- C, Wo Long 2: one counter family tracks the displayed fps (114, then 81). It does one update per rendered frame.

## 1. Architecture: lockstep gate + real-dt (three parts, all in the existing DLL)
**L1 Render heartbeat [r1: moved to the render packet take].** Primary signal: the render thread taking an update packet. That is the `r15 != 0` path at `0x1a7495f` in the render loop, after the mailbox lock is released (verified r1). It is 1:1 with consumed packets, works whatever the vendor (XeLL, Reflex or none), and gives the update thread the whole render CPU frame to publish the next packet.
- Implemented as a mid-function stub hook (same technique and unwind discipline as P1/P2), found by AOB with the stub's return derived from the match. It does `render_seq++` (interlocked) and `render_qpc = QPC`, with no waits and no calls besides QPC.
- `HeartbeatSource` options: `take` [default], `xell_entry`, `xell_exit`. The xell variants need a real C detour on xellSleep that preserves the return value. The S1 entry tail-jump thunk cannot run code after the call returns [r1].
- Diagnostics: log xellSleep entry and exit QPC. Count render packet-wait polls, which are Sleep(1) calls returning to `0x1a7495a` (resolved by AOB): count and ms per 10 s. That shows whether render ever starves for packets.

**L2 Update gate in H1** (the clock-entry hook `0x5ad5f8`, already installed; it runs on the update thread, the same TID as H2 in S1 data). It runs BEFORE r1's QPC is taken, so r1 includes the wait [cx]:
```
if gate_armed():
    deadline = now + min(2.5 * median(packet-take interval, last 16), 50 ms)
    while render_seq == last_seq and QPC() < deadline:
        help_jobs(H1.rdx, 0)        # = native 0x916ebc(ctx, 0): run/steal one job, else Sleep(0)  [r1]
    if render_seq == last_seq: timeouts++ (window counter)
    last_seq = render_seq            # consume latest, never bank missed frames [cx]
```
[r1] There is no kernel wait. The update thread is a job worker that is busy about 54% of the time inside the native pacer. Blocking it would starve jobs, so the gate does exactly what the native pacer does: help run jobs until the condition holds. `help_jobs` is resolved by AOB from the `call 0x916ebc` at `0x19df583` (rel32 target), with `rcx` = H1's `rdx` (job-worker context, verified r1).
`gate_armed()` requires all of: toggle ON; runtime invariants hold [r1] (over the last 2 s: H1 rate 115-125/s while the gate is unarmed, H1:H2 = 1:1, heartbeat on one constant TID); H2 ran during the previous tick [r1] (title/loading runs H1 without H2); offline only (see section 5); fps mode `+0x618 == 120`; not paused (`+0x58b`/`+0x58c` == 0); not a cutscene (existing scene check); heartbeat healthy (render_qpc < 100 ms old); not in timeout backoff. Bootstrap: the first armed tick does not wait.

Backoff: 3 timeouts within 1 s disarms the gate for 2 s, resets the r1/r2 history and logs `GATE BACKOFF`. Repeated backoff (5 in 60 s) disarms it for the session, with a STATUS line. This breaks any render→update dependency cycle [cx: render can wait on update at `0x1a7494b`, `0x1a74872`/`0x92552c`].

Rules: never wait while holding game locks (H1 entry holds none: it is the clock fn entry, outside the mailbox lock); never wait on a thread other than the update thread (re-capture the update TID from each H1→H2 pair. A TID change is logged and disarms the gate for 1 s [r1: the task can move between workers]). `0x5acf80` is also reachable without H1 via vtable `0x40ef458`. That path is not gated and is logged if seen [r1]. The game's own 120 Hz limiter stays, so above 120 nothing changes.

**L3 Real-dt.** Enable the v7.1 factor (TimingMode Raw default, clamps unchanged) at P1 `0x5ad6a2` and P2 `0x39e526`. With lockstep at ~90 Hz, r ≈ 1.33. Without it the game would run at 75% speed.

**Native alternative (NOT chosen) [cx].** Re-arming `Game+0x200 |= 8` each tick selects the game's lossless publish-and-wait path (`0x412943` → `0x19a525e`). Rejected: that wait has no deadline and spins under the mailbox lock-release loop, so it cannot fail passive. Keep it as a fallback experiment only if L2 shows phase problems.

**Rate-only alternative (rejected) [cx].** Setting `[timing+0x30]` to the render rate does not lock phase, and the value is overwritten at `0x5ad179`.

## 2. Update resilience (user requirement: must survive game patches)
- Every hook address, the xellSleep return-address filter and every referenced global/offset is resolved from a UNIQUE AOB match anywhere in .text, decoding rip-relative operands from the match. No fixed-RVA expectation anywhere (v7.1/S1 checked AOBs at their old RVAs, which fails safe but does not survive relocation [cx]).
- AOB anchors [r1]: `+0x618` and P1 come from the `0x5ad6a2` AOB, and pause bytes `+0x58b`/`+0x58c` from the clock fn's pause test (the `cmp byte [reg+0x58b]` and `[reg+0x58c]` encodings inside `0x5ad5f8`). The scene check comes from the existing `0x123aa20` AOB plus the scene-manager global decoded from its rip-relative load. The heartbeat, the Sleep(1) return and help_jobs come from render-loop and pacer AOBs. A unique match is necessary but not sufficient: arming also requires the runtime invariants in L2 [r1].
- Other field offsets are taken from the AOB-matched instruction encodings (the `0x5ad6a2` AOB already encodes +0x628/+0x624). If extraction fails: passive.
- Each feature fails independently. No heartbeat AOB means L2 is off (L3 alone is then pointless, so it is off too). No P1/P2 means everything is passive. One STATUS line per feature.
- The probe CSV machinery stays diagnostics-only (loaded only if the file is present). Release builds ship without the CSV.

## 3. Consumers that count ticks (carried over from v7.1, now MORE important)
With lockstep, ticks/s drops from 120 to the render rate. Integer per-tick timers and `60/fps` computations not routed through P1/P2 (F7b inline +0x614 readers, F7c raw speed readers, F9e `0x5acb36`/`0x271590d`/`0x271827a`, the 9 integer frame timers) would run 25% slow at 90 fps. Required before any release build (not for the S2L experiment): the G1b(d) classification of every site with hits > 0 from the S1 probe CSV, then either per-site scaling or proof that it is cosmetic. The S2L experiment measures the size of this (see M5).

## 4. Stage S2L: experiment build (Codex implements; independent review before install)
Config: `Lockstep=1`, `StartEnabled=1` (testing only), F10 toggles L2 and L3 together (A/B). New log lines: per-10 s `GATE` summary (armed %, waits, mean/max wait ms, timeouts, backoffs, ticks per present histogram) and per-tick trace records for gate wait begin/end.

Acceptance, measured by me in-game with camera-only input (tools/gamectl.py sweep), using the S1 trace analysis (ticks between XELL events; game advance = sum of P1 effective dt in the frame):
- M1: ≥ 98% of shown frames carry exactly 1 tick (baseline 71%).
- M2 [r1: lag-aware, floor-relative]: dt always lags by one frame, so compare game advance in frame n with the heartbeat interval of frame n-1, using the same signal point. The floor is the S1-style |ΔT[n] - ΔT[n-1]| of the heartbeat intervals in the same run (S1: 0.54 ms mean, 1.49 ms p95). Pass: mean ≤ floor_mean + 0.2 ms and p95 ≤ floor_p95 + 0.5 ms (baseline 3.40 / 6.15).
- M3: present rate within 3% of the toggle-OFF rate over the same sweep (the gate must not cost fps).
- M4: 0 timeouts and 0 backoffs over a 5-minute steady sweep; no hang when loading from the title (I can drive menus before loading in).
- M5 [r1]: Raw mode now also logs time_lost/time_gained from clamps and hitches. Game-time rate = real time ±0.5% over 5 min after subtracting the logged clamp losses (the legacy 60 Hz tick count `0x5a6bb0` must run 60.0/s ±0.3), and a count of probe-CSV sites whose rate scales with tick rate.
- M6: F10 A/B: both states stable and switchable 10 times without hitches > 50 ms.
User-only checks (needs buttons, which I must not press after loading in): pause menu, cutscene, fast travel/loading, combat hit-stop, alt-tab. A short checklist is given to the user after M1-M6 pass.

## 5. New risks
| Risk | Mitigation |
|-|-|
| Render waits on update → gate stalls | bounded deadline, backoff, watchdog; M4 |
| Input latency changes | lockstep removes dropped/stale updates; measure the present-to-H1 gap; F10 A/B by feel |
| xellSleep not called (XeLL off, other upscaler path) | heartbeat-healthy check → L2 off → passive; STATUS line |
| Frame generation (FSR/XeSS FG DLLs are loaded) | heartbeat is the render packet take [r2], so generated frames don't count |
| Tick-counting consumers slow down | section 3; M5 quantifies; release blocked until classified |
| Co-op / P2P at a variable tick rate [r1] | S2L is tested offline only. Release is blocked until an online/session flag is found and the gate disarms on it |
| Render starved of packets (serialization) [r1] | take-point heartbeat; the Sleep(1) poll counter; M3 |
| H1 not always on the update thread | TID check; gate only on the captured update TID |

## 6. Review r2 (verdict PASS, MINORs folded in) [r2]
1. The heartbeat is NOT strictly 1:1 with new packets. It also fires on the replay-previous-packet path (`0x1a748a6`, r15 = rsi). The stub records the packet ptr (r15) and a repeat flag (same ptr as last). Repeats still count as heartbeats, which keeps the gate live, but they are counted separately.
2. The render loop `0x1a74130` is generic, with 2 callers (`0x27050d1`, `0x32471bc`). Latch the mailbox pointer (r13 at the stub) together with the render TID on the first heartbeat after the update-side publish (the mailbox the H1 task publishes into), and count heartbeats only for that pair. Others are counted as `hb_other`.
3. Re-entrancy: a thread-local `in_gate` flag. Any hook (H1/H2/P1/P2/heartbeat) entered on a thread with in_gate set passes straight through, without touching pairing, TID or factor state.
4. Trace records: HB (seq, qpc, packet ptr, repeat flag, mailbox ptr), GATE_BEGIN/GATE_END (seq seen, waited µs, timeout flag). The GATE summary adds heartbeat:XELL and repeats:heartbeat ratios.
5. The legacy tick counter `0x5a6bb0` and the camera counters are hard-wired built-in probes in S2L, independent of the probe CSV.
6. "Offline only" is a TEST-PROCEDURE rule until an online flag is found, not a runtime check.
7. Section 5 frame-generation row: the heartbeat is the packet take, so generated frames don't count.
8. Sleep(1) poll counting: patch only the 6-byte `call [rip+Sleep]` at `0x1a74954` (AOB-located) to call a counting thunk that then calls the original import. No global Sleep hook.
