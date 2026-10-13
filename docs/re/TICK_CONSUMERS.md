# Tick consumers under lockstep (G1b(d) classification, 2026-10-13)

Static RE on `Nioh3.exe.unpacked.exe` (read-only; helper `tools/re/relib.py`). Hit rates come from the S1 probe histograms (`recordings/s1_run{1,2}_modlogs/nioh3fps_hist.csv`). run2 is ~28 min at the shrine; run1 includes title, loading and other areas. Answers PLAN_v8_lockstep section 3.

**Model.** Under S2L the update runs once per presented frame (~95/s). Let `r` = the P1 factor (real dt / (1/120), ~1.26 at 95 fps). P1 `0x5ad6a2` writes clock `+0x624 = +0x628 * r`, and P2 `0x39e526` scales `dt = 60/S.fps`. Anything derived from `+0x624`, GetTimeStep `0x46484` (`+0x624*60/fps`) or the P2 dt keeps real-time rate. Anything that uses `+0x614` (int 120) alone, through `60/fps`, `1/fps`, `fps*seconds` or a per-tick `++`, runs at `1/r` speed.

**Classes.**
- A: derives from P1/P2, safe.
- B: gameplay, runs slow, needs scaling.
- C: cosmetic, UI or audio, or not time-integrating.
- X: double-scaled, runs fast.

Confidence is about the arithmetic. System names are inferred, because the binary has no RTTI and few strings.

## Counts

| Group | A | B | C | X |
|-|-|-|-|-|
| F7b inline `+0x614` with hits (7 sites) | 0 | 2 | 5 | 0 |
| F7b zero-hit families (46 fns) | 0 | 0 | all (2 families) | 0 |
| F7c raw speed readers (6) | 2 | 0 | 3 | 1 |
| F9e (3) | 0 | 0 | 3 (2 dormant) | 0 |
| Integer frame timers F8 + 9th (9) | 0 | 1 | 8 | 0 |
| F7d / F7e / legacy tick (3) | 3 | 0 | 0 | 0 |
| GetFps callers with gameplay hits (22, not in the plan list) | 1 | 19 (6 of them B?) | 2 | 0 |
| GetFps callers, run1 only (11) | 0 | 9 | 2 | 0 |
| Other fixed-step readers (hunt, 10) | 3 | 1 (probable) | 6 | 0 |

Main conclusion: the plan's candidate list is mostly harmless. The real exposure is the **GetFps (`0x46594`) callers**: 19 hot gameplay sites with character countdowns, velocity, input hold timers and action timers. There are also two unexpected items: **X `0x42dde4`** (characters run fast) and **probable B physics `0x2de300`**.

## Recommended fixes (priority order)

1. **X: character sub-step double scale.** `0x42dc6c` computes `xmm6 = GetSpeed(0x42dde4 = +0x624) * [rdx]`. `[rdx]` is `0x44bb9f0`: it is written in `0x31a060` ← `0x3962e0` ← `0x39f328` ← `0x39e728` from the main-update xmm6, which is P2-scaled. The product carries `r²` per tick, so `r` per second. It feeds the 60 Hz sub-step accumulator `0x42dd98` (modf; integer n steps), at 1740 calls/s, so characters step about 1.26x fast at 95 fps. M5 (the legacy tick only) cannot see this.
   - Fix: detour leaf `0x42dde4` so it returns `+0x624 / r` (the pre-P1 effective speed).
   - AOB `48 8B 05 ?? ?? ?? ?? 48 85 C0 74 09 F3 0F 10 80 24 06 00 00 C3 0F 57 C0` (1 match).
   - Before patching, confirm live: log `[rdx]` at `0x42dcd4`, which should be ~0.5·r.
   - Confidence: med-high.
2. **GetFps allowlist hook (one hook covers every B GetFps row).**
   - Hook GetFps by its existing AOB `48 8B 05 ?? ?? ?? ?? 48 85 C0 74 07 8B 80 14 06 00 00`.
   - When the return address is on an allowlist, return the effective tick rate `120/r` instead of 120. Use per-site error diffusion so the int rounding averages out.
   - Each allowlist entry is resolved by the unique caller AOB below, as `ret = match + 5` unless stated otherwise. Also verify that the E8 rel32 resolves to GetFps.
   - This one substitution is correct for every pattern seen: per-tick `60/fps` or `1/fps` steps, `fps*seconds` counts, counters compared against `fps*k`, and `Δpos*fps` velocity.
   - Mode tables keyed on 30/60/120 (e.g. `0x5af6xx`, `0x947250`) must never be on the allowlist.
   - Thread-safe: r is a global float, read only.
3. **Inline `+0x614` B sites.** These bypass GetFps, so each needs its own mid-function stub (same technique as P1/P2): `0xe7050d` and `0x117c051`.
4. **Input hold `0x2978640`.** Stub at `0x29786a6`.
5. **Physics `0x2de300` (probe first).** A runtime probe must confirm that `scene+0x648` is a constant per-asset step before this is patched.

## Rows

### F7b inline `+0x614` readers (sites with hits)

| site | fn | hits/s | class | evidence | fix | conf |
|-|-|-|-|-|-|-|
| 0xe704b4 | 0xe6fda0 | 120 | B (minor) | `[mgr 0x45b9e58+0x1970] += 1/fps` until > 0.5 s; read at 0x2b3faf to gate a flag (~0.13 s late at 95 fps) | stub 0xe7050d: `xmm13 *= r`. AOB `44 0F 28 E1 F3 45 0F 58 E5 F3 44 0F 11 A7 70 19 00 00 48 8B 3D ?? ?? ?? ??` (1) | med |
| 0x117c020 | 0x117a6d0 | bursty (run1) | B (low exposure) | animation-event look-ahead window `60/fps*rate[+0xc8]` → 0x9a3c14/0x9a3c54; too narrow a window can skip keys | stub 0x117c051: `xmm6 *= r`. AOB `F3 0F 10 35 ?? ?? ?? ?? 0F 57 C0 8B C0 0F 28 CF 41 C6 87 4C 01 00 00 01 49 8B CD F3 48 0F 2A C0 F3 0F 5E F0 0F 28 D6 F3 0F 59 97 C8 00 00 00` (1), hook = match+0x24 | med |
| 0xe5ed17 | 0xe5e9d0 | 120 | C | `fps*[r8+0x16c] > 30`: seconds-to-frames for a threshold; fps stays 120 | - | high |
| 0xe5ff50 | 0xe5f410 | 120 | C | `2/fps` one-shot blend time in a request struct → 0x8433a4 | - | high |
| 0x10e0537 | 0x10e03d0 | bursty | C | `int(cnt + 60/fps)` truncates to 0 at 120, so it never advances either way | - | high |
| 0xe49538, 0xf826e1 | 0xe48f30, 0xf82520 | ~1 | C | same `2/fps` one-shot family | - | high |

**Zero-hit F7b families** (0 hits in ~30 min).
- `2/fps` one-shot param family, C: 0xc64340, 0xe34b80, 0xe4f7f0, 0xeb5290, twins 0xf0e020/0xf0fec0/0xf18bc0/0xf1eae0/0xf23140, 0xf19070, 0xf1b450, 0xf1c8a0, 0xf20510, 0xf2a3b0, 0xf7d060, 0xf7fc50, 0xf85d30, 0x1152030..0x115ee50, 0x11785d0, 0x1240de0..0x129ee80.
- Round-robin refresh scheduler 0xe66520/0xe66e20, C: item i runs when `i mod 3·fps` (or `10·fps`) equals tick counter 0x45c84a0. The 3 s/10 s refresh stretches to ~3.8/12.6 s.

### F7c raw speed readers

| site | fn | hits/s | class | evidence | conf |
|-|-|-|-|-|-|
| 0x42dde4 | leaf (caller 0x42dc6c) | 1740 | **X** | see fix 1 | med-high |
| 0x434df4 | caller 0x434da0 | 120 | A | `[obj+0x3d0] += speed/60` per tick; r keeps the per-second rate | high |
| 0x10f7de3 | 0x10f7dd0 | 95 | A | idle timer `0x45c9168 += +0x624` while input ≈ 0 | high |
| 0x2a0a44 | field-getter jump table | 0 | C | reflection/debug getter (would return speed·r) | med |
| 0x26f4768/0x26f4780 | leaf | 0 | C | read clock `+0x58/+0x5c` (int), not speed | high |

### F9e

| site | fn | hits/s | class | evidence | optional fix | conf |
|-|-|-|-|-|-|-|
| 0x5acb36 | 0x5ac9ec | 120 (branch only while +0x2f7) | C | stage-load/respawn transition updater; `60/S.fps` → job vfunc+0x30; ~1.26x longer transition | stub after divss: `xmm1 *= r`. AOB `F3 0F 10 0D ?? ?? ?? ?? F3 48 0F 2A C0 F3 0F 5E C8 E8 ?? ?? ?? ??` (1), hook = match+0xD | med-low |
| 0x271590d | 0x27158d8 (vt 0x3b644c0 #21) | 0 | C (dormant) | `+0x1710` in 60ths; flag `+0x174d` while < 10 (~167 ms) | AOB `F3 0F 10 0D ?? ?? ?? ?? F3 48 0F 2A C0 F3 0F 5E C8 83 7F 2C 01` (1) | med |
| 0x271827a | 0x2718260 (#35) | 0 | C (dormant) | `+0x1714` in 60ths; > 3 → vfunc+0xa8 state advance | AOB `F3 0F 10 0D ?? ?? ?? ?? F3 48 0F 2A C0 F3 0F 5E C8 F3 0F 58 89 14 17 00 00` (1) | med |

### Integer frame timers (F8 plus the 9th)

A scan of every GetFps caller finds exactly 8 integer `120/fps`/`60/fps` divisions, so the "9th" timer is `0x2978640`. The input-module timers are assumed to tick once per update tick; this follows from their 120/fps units, but their thread was not proven.

| fn | hits/s | class | evidence | fix | conf |
|-|-|-|-|-|-|
| 0x2978640 (ret 0x2978676) | 120 in play | **B** | input action map: held action `[entry+4] += 60/fps` per tick (hold, charge and long-press thresholds) | stub 0x29786a6 (7-byte rip-rel mov, relocatable): `xmm6 *= r`. AOB `F3 0F 10 35 ?? ?? ?? ?? F3 0F 5E F7 48 8B 05 ?? ?? ?? ?? F3 44 0F 10 0D ?? ?? ?? ??` (1 match at 0x297869a), hook = match+0xC | med |
| 0x435864 | ~20 | C | mouse-idle counter up to 1000 (GetCursorPos) | - | high |
| 0x55dbf0 | ~9 | C | cursor auto-hide after 3 s | - | high |
| 0x437cd8 | rare | C | input-device refresh timer (1.5 s) | - | med |
| 0x43837c | ~2 | C | rumble motor durations | - | med |
| 0x2977144 | - | C | rumble pattern sequencer | - | med |
| 0x2977014 | - | C | per-port haptic hold-off | - | med-low |
| 0x1975b90 | rare | C | paused-only `int(60/fps)` = 0; inert | - | high |
| 0x5af670 (from 0x5ad8c3) | 120 | C + **risk** | frame-time governor: 10-sample interval average × fps/1000 vs table 0x3be2a20; level 4 → 0x5af4e4 drops the cap 120 → 60 → 30. Under lockstep it would read ~10.5 ms and could step the fps mode down, which disarms the gate. Live only when global `0x45baf58 != 0` | check `0x45baf58` live; if non-zero, feed it 8.33 ms or disarm | med |

### F7d, F7e, legacy tick

| site | class | evidence | conf |
|-|-|-|-|
| F7d field 0x5ad7b8/0x5ad825 → 0x8c8e54/0x5ab644 | A | `+0x624*60/+0x614`, with `+0x624` P1-scaled | high |
| F7e 0x5ad9ec/0x5adc64 copies | A | whole-clock snapshot: copied `+0x624` is scaled (1 tick lag); copied `+0x614` is still 120, so readers of the copy classify like main-clock readers | high |
| legacy tick 0x5a6bb0 | A | driven by `+0x62c`, built from P1-scaled xmm2 (0x5ad702) | high |

### GetFps callers (not in the plan list; fix = allowlist entry, fix 2)

| ret | fn | hits/s | class | evidence | caller AOB (at E8) | conf |
|-|-|-|-|-|-|-|
| 0x42f105 | 0x42f0fc (1/fps helper, 20 callers) | 1455 | mixed B/C (see S2C-F below) | per-tick dt for the chr 2 s timer `+0x698a0` (0x434e55), fades, chr sub-objects `+0x9c`, ms timers, and 0x7acc6c (twin of the `+0x1970` timer) | `E8 ?? ?? ?? ?? 85 C0 74 18 F3 0F 10 05 ?? ?? ?? ??` | high |
| 0x42d264 | 0x42d218 | 1200 | B | countdown `-= rate/fps` | `E8 ?? ?? ?? ?? F3 41 0F 10 09 0F 57 C0 44 8B C0` | high |
| 0x12b619 | 0x12b440 | 840 | B | duration → ticks `fps*n/60`, counted per tick | `E8 ?? ?? ?? ?? 0F 57 C0 44 8B C0 66 41 0F 6E C9` | med |
| 0x20ff99 | 0x20ff70 (chr vt 0x38fdf88+0x120) | 550 | B | chr countdown `+0x4cc -= 60/fps` → lerp 0x865de4 | `E8 ?? ?? ?? ?? F3 0F 10 3D ?? ?? ?? ?? 48 8D 8E 98 04 00 00` | high |
| 0xb119a | 0xb0b70 (actor mgr) | 550 | B | chr velocity `(pos-prev)*fps` → 0x911f9c, overestimated by r | `E8 ?? ?? ?? ?? 44 21 64 24 4C 48 8D 93 10 01 00 00` | high |
| 0x231da0 | 0x231d94 | 480 | B? | vfunc+0x20 with dt = 60/fps; system unknown | `E8 ?? ?? ?? ?? F3 0F 10 0D ?? ?? ?? ?? 0F 57 C0 44 8B C0 49 8B C9 49 8B 01` | low |
| 0x901bb3 | 0x901ba4 | 241 | B? | int `table[i]*60/fps` frame↔tick conversion | `E8 ?? ?? ?? ?? F3 0F 10 0D ?? ?? ?? ?? 0F 57 C0 43 8B 8C 8A A4 00 00 00` | low |
| 0x433a85 | 0x433a4c | 120 | B | input: 0x51 button hold timers `+= 60/fps` | `44 0F 29 54 24 20 E8 ?? ?? ?? ?? 44 8B C0 0F 57 FF` (ret = match+0xB) | med-high |
| 0x90817c | 0x908154 | 120 | B? | dt = 60/fps → 0xe6fae0 → worker job (same subsystem as 0xe6fda0) | `E8 ?? ?? ?? ?? F3 0F 10 0D ?? ?? ?? ?? 0F 57 C0 44 8B C0 F3 49 0F 2A C0 F3 0F 5E C8 E8 ?? ?? ?? ??` | med |
| 0x2b2f59 | 0x2b2f28 | 120 | B | tick counter `[+0x78] > fps*param` | `E8 ?? ?? ?? ?? 44 8B C0 0F 57 C9 F3 49 0F 2A C8 F3 0F 59 C8` | med |
| 0x127200 | 0x1271ec | 120 | B? | chr counters `+0xb4a/+0xb4b >= fps/2` | `E8 ?? ?? ?? ?? D1 E8 44 3B C8 7D 07 32 C0 48 83 C4 28` | low |
| 0x5c0420 | 0x5bf3b4 | 120 | B? | elapsed `[+0x5f8] += 60/fps` | `E8 ?? ?? ?? ?? F3 0F 10 0D ?? ?? ?? ?? 0F 57 C0 44 8B C0 F3 49 0F 2A C0 F3 0F 5E C8 F3 0F 58 8F F8 05 00 00` | med |
| 0x396713 | 0x3966c8 (actor mgr, inside P2 consumer 0x39f370) | 120 | B | 4 countdowns `+0x98..+0xa4 -= 60/fps` | `E8 ?? ?? ?? ?? F3 0F 10 15 ?? ?? ?? ?? 0F 57 C9 F3 0F 10 83 A0 00 00 00` | high |
| 0x5fb365 | 0x5fb32c | 120 | B | input stick-history window `param*fps/60` ticks | `E8 ?? ?? ?? ?? BA 99 2D 00 00 48 8B CF 8B D8 E8 ?? ?? ?? ??` | med |
| 0x4362fd | 0x43619c | 120 | B | input hold/repeat counter vs `fps/x*param` | `E8 ?? ?? ?? ?? 48 8B 5C 24 68 44 8B C0 0F 57 C9 F3 49 0F 2A C8 F3 0F 5E CB F3 0F 59 C8 0F 2F D1 76 1A 80 7F 01 00 0F 94 C0` | med |
| 0xefafc | 0xef644 (chr vt +0x1b4) | 120 | B | chr `[+0xfdc]++` vs `param*int(2fps/60)` | `E8 ?? ?? ?? ?? 48 8B 0D ?? ?? ?? ?? 0F 57 C9 44 8B C0` | med |
| 0x5fb817 | 0x5fb66c | 3 | B | per-tick distance threshold `speed*60/fps` | `E8 ?? ?? ?? ?? 44 8B C0 0F 57 C9 0F 28 D6 F3 49 0F 2A C8` | med |
| 0x4afe3d | 0x4afdf8 | 1 | B | 2 s countdown `round(2*fps)` → `+0x52f60` | `E8 ?? ?? ?? ?? 0F 57 C0 44 8B C0 F3 49 0F 2A C0 F3 0F 58 C0 E8 ?? ?? ?? ?? F3 0F 2C C0 89 83 60 2F 05 00` | med |
| 0x8f236b | 0x8f223c | 0.5 | B | same pattern → `+0x52f50` | same AOB, ending `89 87 50 2F 05 00` | med |
| 0x27ec52 | 0x27ec38 | 2640 | A | `[chr+0x8e8]/(60/fps)`; `+0x8e8` is written (0xb049e) from the P2 consumer 0x396e38's dt struct | - | med-high |
| 0x68894a | 0x6886ac | 550 | C | audio smoothing | - | med |
| 0x231cb1 | 0x231c84 | 120 | C (now) | writes F9d `obj+0x1ac = 60/fps`; becomes B only if 0x20dda0c goes live (0 S1 hits) | - | med |

**Run1 only** (not seen at the shrine). The full AOBs come from `mkaob`; regenerate them when building the allowlist.
- B, player-character action timers (`+=/-= x/fps`): 0x27d5fc, 0x27d057, 0x27463b, 0x27487d, 0x26fed7, 0x270da1.
- B?: 0xfaabe (frame threshold); 0x7d4bd2/0x7d4c02 (animation seconds → ticks).
- C (effects): 0x8b28b8, 0x3cdb69.
- GetFps callers with 0 hits in both runs are unclassified. If any shows up later, it must be classified before it goes on the allowlist.

### Other fixed-step readers (hunt)

| site | class | evidence | fix | conf |
|-|-|-|-|-|
| 0x2de300 physics scene step (CPhysScene ← 0x586178) | **B (probable)** | substep dt = `scene+0x648` (TimeStep) / `scene+0x664` (SubStepCount), once per tick. The property table names both as per-asset; the only writer is the setter 0x586580, which has no static refs, so the value is likely constant. Rigid bodies, ragdolls and gadgets would run at 1/r | probe `scene+0x648` per tick first. If constant: hook the entry, AOB `40 53 48 83 EC 20 0F 57 C0 48 8B D9 0F 2E 81 48 06 00 00 74 72 C6 81 C1 06 00 00 00` (1); save `[rcx+0x648]`, multiply by r, call the original, restore; take the offset from the AOB; clamp so the substep stays ≤ 1/60 (0x2d4285 compares against 1/60) | low-med |
| pacer `[r15+0x18]` (0x5ad30b) | A | = clock param `+0x44` = `+0x624/fps` (P1-scaled); the 1/fps fallback only runs when paramobj is null. Readers 0x5ab994 (total time), 0x4ec43c | - | high |
| pacer `[r15+0x24]` | C | measured real dt; stored only | - | med |
| packet `+0x1e6` (0x5ad8b6/0x4127dc) | C | `+0x614/+0x61c` ticks per published frame = 1 | - | high |
| 0x1fc122c streaming | C | 3-tick countdown (~6 ms extra latency) | - | med |
| 0x1fbfcac, clock `+0x618/+0x61c/+0x620` getters | C | mode selection / no callers | - | med |
| S struct (0x5ac725) consumers: 0x2237840, 0x2229f18, 0x5ea4f8, 0x3b2a04, 0x564250, 0x56437c, 0x564720 | A | use `S+0x14/S+0x10` or `S+0x18`, built from the persistent P1-scaled `+0x624`; none uses `S+0x10` alone | - | med-high |
| 0x841eb3 | C | `+0x1610 += 1/60` wrap 3600 (shader time) | - | med |
| GetTimeStep accumulators 0x2823fa, 0x282305, 0x2d6a40, 0x2d6ad5 | A | `+0x624*60/fps` | - | high |

Constant scan (rip-relative f32 loads):
- 1/120: 31 loads, 15 fns. The sampled ones are Taylor-series terms (maths, not time).
- 1/60: 112 loads, 99 fns, mostly unclassified. The sample found only known uses.
- 120.0: 60 loads, 40 fns.
- Residual risk: per-tick `++` counters that never read fps cannot be found statically. M5-style rate probes are the backstop.

## Verification before release
1. Live-log `[rdx]` at `0x42dcd4` (it should be ~0.5·r), and `scene+0x648` at `0x2de300`.
2. Read global `0x45baf58` (governor armed?).
3. Extend the S2L probe so the allowlisted GetFps callers and stubs report effective-rate counters (events per real second should match 120-fps baseline ±1%).
4. User checks: charge/hold attacks, dodge timing, ragdolls and gadgets, buff and i-frame durations at ~90 fps, with F10 A/B.

## S2C-P static verification

**P-Ph answer: the substep count is the per-asset constant `scene+0x664`, not step-derived.**
The asset loader `0x5863a4` copies asset `+0x38` into `scene+0x664` at `0x586422`.
The scene's body loop tests this field at `0x197cf21`, calls the body substep
`0x2fed9dc` at `0x197cf37`, increments its index, and compares that index with
the same field at `0x197cf45`. It does not compute a new count from `scene+0x648`.
`0x2d4285` loads `1/60` and compares it with a constraint-local `[rdi+0x7c]`;
this is not the scene substep-count calculation. Increasing the step would not
automatically raise the configured count. S2C-P records both the configured count
and actual body-substep calls per H1 tick; it applies no physics change.

P-Gov's global `0x45baf58` has a settings-dispatch write at `0x12a407b`
(`mov [rip+disp32],edi`). The probe decodes and checks that destination against
the governor's read operand. The human-readable setting name is not established
by this dispatch alone. Clock mode writes are distinguished from speed-request
writes by their decoded `+0x618`/`+0x628` destinations and actual clock-object
identity; caller returns come from decoded host unwind stack allocations.

## S2C-P live results (build 74576ff9, 2026-10-13, shrine, camera-only; gate ON 07:42-07:44, OFF 07:44-07:46)
- X confirmed. X-path steps per accumulator per second: OFF 60.0, ON 76.3-76.8 (= 60·r at ~94 fps). `[rdx]` OFF 0.5, ON 0.635-0.639 (= 0.5·r). `[chr+0x8e0]` = 0 on the X path. Fix 1 is required.
- Override path: ~150-170 steps/acc/s both ON and OFF (no 1/r drop), and `[rdx]` scales up with r. The `[x+0x40]` branch is real-time correct. Ret `0x927bce` never appears in P-G. No fix is needed for the override path.
- Physics: `scene+0x648` = 1/120 OFF, and ON it equals the real frame dt (0.0085-0.0121). 1 configured substep. The step already follows real time, so it is class A and needs no fix. The ON `scene_calls` figure is a probe artefact: there are only 16 distinct-value slots, and the overflow counter absorbs ~940/10 s.
- Governor: `0x45baf58` = 0 throughout. 0 fps-mode changes, 0 speed_req writes. Inactive.
- P-G, gate ON at the shrine: 27 live sites. Every caller above 1/s is classified except the helper outer callers `0x42ea6b` (10/tick) and `0x434e5a` (1/tick), which need classifying. The mode tables `0x5af687`/`0x5af6a4` are on the denylist. `0x435935` is in the known set.
- M6 via ToggleFile: 20 toggles, 0 backoffs, min real fps 80. The gate re-arms about 2 s after each ON edge (the history requirement).


## S2C-F classification and implementation (2026-10-13)

The helper is mixed B/C; its first-level return `0x42f105` alone is never
allowlisted. The second-level return is decoded using the helper's AOB-decoded
stack allocation. All specifically named helper callers and every outer observed
in the reviewed shrine sweep are classified below. The other 13 static helper
callers remain unclassified and pass through; a newly hot outer triggers the
unknown-caller guard even though the first-level helper is known.

| helper outer return | class | disassembly evidence | policy/tier |
|-|-|-|-|
| `0x42ea6b` | **B** | `0x42ea66` calls the 1/fps helper; `0x42ea77`/`0x42ea86` move dt to xmm1. `0x42f0d0` and `0x42f060` store it into character sub-object `+0x9c`, without P1/P2 scaling. | dither/core |
| `0x434e5a` | **B** | `addss xmm0,[rbx+0x698a0]`, compare with 2.0, store: the character's two-second per-tick timer. | dither/core |
| `0x43b822` | **C** | Constructor `0x43b810` doubles 1/fps and stores a one-shot default into `+4`; it does not integrate elapsed time. Same 2/fps request-parameter family as F7b C. | native/rare known |
| `0x43babd` | **C** | Constructor doubles 1/fps and stores a one-shot default into `+0xc`, clearing the other request fields. | native/rare known |
| `0x43bf01` | **B** | Multiplies a rate by 1/fps and stores per-update movement into `+0x28`. | dither/rare |
| `0x43bf1a` | **B** | Multiplies a rate from `0x992374` by 1/fps and stores per-update movement into `+0x2c`. | dither/rare |
| `0x7accd9` | **B** | Adds 1/fps to elapsed `+0x1970`; twin of the confirmed inline elapsed-time consumer. | dither/rare |

Direct rate sites use per-tick fixed-point error diffusion with a site phase.
The immutable 16-byte `{r, phase/armed, seq}` pair is published atomically by H1 after
factor calculation and before the native frame. Calls at the same site that capture
the same published pair get the same integer. Calls after the next publish get the
next tick's value; at a steady rate, boundary calls are at most ±1 apart.
H1 advances the fractional phase once per tick, including when r changes; the
phase occupies the padding alongside r. Cumulative quantisation error relative
to the fixed-point rates is bounded by one count. Capacity
and threshold sites use nearest rounding: `0x12b619`, `0x901bb3`, `0x2b2f59`,
`0x127200`, `0x5fb365`, `0x4362fd`, `0x4afe3d`, `0x8f236b`, `0xfaabe`,
`0x7d4bd2`, `0x7d4c02`. All remaining direct B/B? rows use dithering except
`0xefafc`, which retains native GetFps and scales the float threshold at
`0xefb3e`: native `int(2*120/60)` is exactly 4, so xmm1 becomes `4/r` before
multiplication by the duration parameter. This avoids integer quantisation.

### Core volume and resilience

P-G source: build 74576ff9, the last fully ON shrine window at 07:43:59.546.
C helper constructors, A/C direct calls, and mode-table calls are excluded from
B hit volume. The core direct caller rates are:

| core GetFps return | calls/s |
|-|-|
| `0x42d264` | 943.246 |
| `0x12b619` | 660.273 |
| `0x20ff99` | 319.383 |
| `0xb119a` | 319.383 |
| `0x231da0` | 377.299 |
| `0x901bb3` | 189.249 |
| `0x433a85` | 94.325 |
| `0x90817c` | 94.325 |
| `0x2b2f59` | 94.325 |
| `0x127200` | 94.325 |
| `0x5c0420` | 94.325 |
| `0x396713` | 94.325 |
| `0x5fb365` | 94.325 |
| `0x4362fd` | 94.325 |
| `0xefafc` | 94.325 |
| helper outer `0x42ea6b` | 943.246 |
| helper outer `0x434e5a` | 94.325 |

Core B volume is 4695.329/s of 4705.531/s total (99.783%).
The remaining 10.202/s consists of rare direct calls and the two
`0x43bfxx` helper calls. All run1-only B/B? entries are present as rare entries,
including `0x27463b`, which also appears at 0.300/s in this live window.

Core additionally requires X `0x42dde4`, GetFps and the helper frame decode,
the three inline stubs `0xe7050d`/`0x117c051`/`0x29786a6`, and the float stub
`0xefb3e`. Any core resolution, call-target validation, unwind or hook-placement
failure disables L2/L3 for the session. Every rare caller resolves independently
and emits its own STATUS line. Unique E8 caller AOBs must target GetFps (or the
helper for second-level entries); sorted tables use binary search. Mode-table
returns `0x5af687`/`0x5af6a4` are explicitly denied, with an assert against any
allowlist collision. The known set includes all seven reviewed A/C hot callers.

The preserving helpers retain GPRs, untouched XMM/YMM lanes, RFLAGS and MXCSR;
body unwind records flatten chained host metadata. Armed state and native TLS
re-entry checks match P1/P2. Unarmed ticks take native leaves and replay native
inline instructions. P-G reports the mean actual integer return as `effective`;
P-X retains the path-specific accumulator measurements and adds `fixed=1` when
the complete consumer core is ready. Expected ON X steps/accumulator/s is 60;
post-fix live acceptance is still pending (the game was not launched).

Override `0x927bce`, physics and the governor receive no fix. The existing probes
remain diagnostic. The unknown-hot-caller guard checks both direct and helper
outer callers, resets on any OFF/unarmed exposure, and logs only after more than
10/s persists for five armed seconds.

## S2C-F live results (build 90e480ff, 2026-10-13, shrine, camera-only; ON 08:27:31-08:29:35, OFF 08:29:38-08:31:38)
- X fixed: X-path steps/acc/s ON 59.95-60.06 (pre-fix 76.8); OFF 59.94-60.07.
- GetFps effective values while armed: allowlisted sites 93.7-94.0 (= 120/r); mode tables 0x5af687 and KNOWN 0x27ec52/0x68894a stay 120. Helper ret 0x42f105 is 96.4 (a mix of fixed outers and unclassified outers left at native 120).
- Lockstep regression with the fixes in: M1 100%, M2 lagged 0.051/0.334 ms (limits 0.407/1.101), M3 94.4 fps, M4 0 timeouts/backoffs, M5 0.999994. No hitches in either state. No UNKNOWN HOT CALLER, RELATCH or PASSIVE lines.

## 1/fps helper `0x42f0fc`: remaining outer callers (static, 2026-10-13)

All 20 static callers are now classified (13 new rows below). Under lockstep the
helper returns native `1/120` unless the outer caller is allowlisted. The
earlier seven are unchanged: `0x42ea6b`, `0x434e5a`, `0x43bf01`, `0x43bf1a` and
`0x7accd9` are B; `0x43b822` and `0x43babd` are C.

Totals for all 20: **A 2, B 7, C 11**. The two new B entries are `0x5ea556` and `0x689caa`.

| outer ret (call) | fn | class | evidence | conf |
|-|-|-|-|-|
| `0x398e1f` (`0x398e1a`) | 0x398a54 (← 0x397cf8) | C | Light/colour fade. `[e+0x28] += (1/fps)/0.2` is clamped to 1. `(1-p)·[obj+0xf0..f8]` → `+0x110..0x118` (RGB), with flags `+0x10b`/`+0x12b`. Fade is 0.2 s → ~0.25 s at r = 1.26 | med-high |
| `0x3992de` (`0x3992d9`) | 0x398a54 | C | Same fade loop, second list (`[e+0x18]`). The only remaining outer seen live: 40-190/s, gate unarmed | med-high |
| `0x5bc62d` (`0x5bc628`) | 0x5bc48c (← 0x5be154 ← 0x403775) | A | `dt = (1/fps) · xmm9`. `xmm9` is the character speed `[r14]`/`[r14+4]` from 0x403388, the same struct the producer 0x404824 sums with `[chr+0x8e0]`, P2-scaled (P-X `[rdx]` = 0.5·r). One r per tick gives real-time rate → timeline integrator 0x20388ac (`+0x90/+0x94/+0xa0` vs durations) | med |
| `0x5e969e` (`0x5e9699`) | 0x5e8fbc | C | One-shot. Inside the pending-flag block `[rdi+0x1c]` (cleared at 0x5e96f4), `10/fps` is stored as a blend parameter at `+0x98` with flag bits `+0x94` by 0x5e9e90. Same family as the `2/fps` request parameters | high |
| `0x5ea556` (`0x5ea551`) | 0x5ea4f8 | **B** | Time-of-day clock. Day fraction `[t+8] += max((1/fps)/2880, g_min) · opt_mult` per tick (1 real s = 30 game s); seconds `[t] mod 86400`. Runs only when `0x85800() == 1` (in mission) and not paused (`0x46518`). Day/night advances at 1/r. Its S-struct use (A, existing row) is separate | med-high |
| `0x689caa` (`0x689ca5`) | 0x6895b8 (vt 0x38fe6f8, character family) | **B** | Turn step. `(1/fps)·[c+0xdc4]` (turn rate) → 0x1abd1a0, which returns `clamp(Δyaw, ±max)·step·4` → 0x5f8ce0 `yaw[+0xa4] += Δ`. Per-tick turning runs at 1/r | med |
| `0x9fd67d` (`0x9fd678`) | 0x9fd634 (← 0x211e3f) | C | One-shot on/off of an effect/sound. `int((1/fps)·[o+0x814]·1000)` converts frames to ms for the fade (`+0x764`, type-7 request 0x6a854c). Native 120 is the authored conversion | med-high |
| `0x9fd824` (`0x9fd81f`) | 0x9fd634 | C | Same, the off branch | med-high |
| `0x2042093` (`0x204208e`) | 0x2041f64 (← 0x35dba7, 0x35ef67) | A | `(1/fps) · 0x927bac(chr+0x3b0)` → 0x205d7b4 → 0x32a9430. 0x927bac returns `[x+0x40]`, which P-X showed is r-scaled (override path real-time correct), so the product is real-time. Only the null-`[x+8]` fallback `60/GetFps` (ret 0x927bce, never live) would make it B | med |
| `0x23ad9a3`, `0x23ad9bd` | 0x23ad924 = script `Sound::PlayMissionBGM` | C | One-shot frames → ms (fade in/out) for a BGM request | high |
| `0x23adaa5`, `0x23adabf` | 0x23ada18 = script `Sound::PlayMissionBGMAndState` | C | Same | high |

New B allowlist entries for the second-level return (use the dither policy, rare tier). Neither is in the shrine P-G data. The `E8` at the match start must target `0x42f0fc`; ret = match + 5.

| outer ret | AOB (1 match each) |
|-|-|
| `0x5ea556` | `E8 ?? ?? ?? ?? F3 0F 59 05 ?? ?? ?? ?? E8 ?? ?? ?? ?? 0F 28 D8 E8 ?? ?? ?? ?? 84 C0 74 09` |
| `0x689caa` | `E8 ?? ?? ?? ?? 49 8B 88 A0 03 00 00 F3 0F 59 87 C4 0D 00 00 48 85 C9 74 1E F3 0F 10 9E A4 00 00 00` |

The A/C outers (`0x398e1f`, `0x3992de`, `0x5bc62d`, `0x5e969e`, `0x9fd67d`,
`0x9fd824`, `0x2042093`, `0x23ad9a3`, `0x23ad9bd`, `0x23adaa5`, `0x23adabf`)
can join the unknown-hot-caller guard's known set:
- `0x5bc62d`: `E8 ?? ?? ?? ?? F3 41 0F 59 C1 4C 8D 45 D7` (1)
- `0x2042093`: `E8 ?? ?? ?? ?? F3 0F 59 D0 48 8B CB` (1)
- `0x3992de` (the only one seen live): its unarmed 40-190/s rate would otherwise trip the guard only if it persisted while armed.
