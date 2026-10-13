# Nioh 3 variable-framerate mod: plan v9 (tick consumers), a delta on plan v8.2

Status: v9.2. Review r2 YES for S2C-P; r2 items are in section 5.
Previous: v9.1. Review r1 NO (2 BLOCKER, 5 MAJOR) → fixed here and marked [r1]. S2C-P is unblocked; S2C-F also needs the r1 MAJORs, which are folded in.

## 0. Where we are
- S2L lockstep passed M1-M5 live (docs/re/DECOUPLE.md, "S2L live result"). M6 (F10 A/B) is pending user input.
- The update now runs at the render rate (~95/s) with r = 120/fps ≈ 1.26 applied at P1/P2. docs/re/TICK_CONSUMERS.md (static) found:
  - X `0x42dde4`: character sub-steps carry r twice, so characters run ~1.26x fast.
  - 19 hot B GetFps callers (+9 seen only outside the shrine) and 2 inline `+0x614` B sites: these run at 1/r speed.
  - 1 input-hold B site.
  - Probable B physics `0x2de300`.
  - Governor risk `0x5af670`.
- Not findable statically: per-tick `++` counters that never read fps.

## 1. Stage S2C-P: probe build (live facts before any fix)
Built-in probes with fixed outputs, each resolved by a unique AOB, record-only, no behaviour change. Each logs a per-10 s summary with gate ON vs OFF:
- P-X [r1]: at `0x42dcd4`, log `[rdx]` and `[chr+0x8e0]` (the producer `0x404824` adds them; the 0.5·r prediction assumes `+0x8e0` = 0). Split by call path at `0x42dcd8`: X path versus the override path (`[r9]&0xf == 0`, where xmm6 = `0x927bac` = `[x+0x40]` or 60/GetFps, ret `0x927bce`). For each path, normalise as steps per accumulator per second: Σn / calls × ticks/s. Correct is 60/s per accumulator at any fps. X shows 60·r on the X path; the override path is expected to show 60/r (B via GetFps). This is the decisive test.
- P-G: GetFps return-address histogram (top 64 by count/s), so the allowlist can be compared against live callers in gameplay, title and loading. [r1] For the 1/fps helper `0x42f0fc` (ret `0x42f105`, 20 outer callers), also log the second-level return address. Calls per tick per site are logged too (several actors per tick).
- P-Ph: `scene+0x648` values seen at `0x2de300` entry (distinct values and count/s) and the substep count per tick.
- P-Gov: global `0x45baf58` value, and any `+0x618` fps-mode change with the caller return address. [r1] Find the setting/code that writes `0x45baf58` (the governor sees ~10.5 ms frames under lockstep, while native always sees 8.33 ms, so it could drop the mode and oscillate). Also log speed_req writes, to confirm that hit-stop/slow-mo only touch `+0x628`, not `+0x618`.
- P-Ph [r1]: verify `0x2d4285`: whether the substep count derives from the step or from per-asset constant `+0x664`.
- Camera-only test at the shrine, 2 min ON / 2 min OFF, toggling without F10 through a new debug-only `ToggleFile` option [r1]:
  - A dedicated low-priority thread polls `nioh3fps.toggle` in the game folder once a second. It never runs on H1, so disk/AV stalls cannot cause hitches.
  - Edge-triggered: a file appearing sets ON, the file disappearing sets OFF. F10 still flips the state, and the next file edge overrides it.
  - The new state is applied by H1 at the tick boundary.
  - Compiled out of release builds.
  - It also covers M6 (10 toggles).

## 2. Stage S2C-F: fixes (after S2C-P confirms)
All fixes are gated by the same `g_l3_tick` state as P1/P2: active only on armed lockstep ticks, otherwise pass-through. r is read from the per-tick published factor (one float, written by H1 before the native frame runs).
1. X fix: detour leaf `0x42dde4` (single caller `0x42dcc8`) to return `+0x628` (pre-P1 speed, exact) on armed ticks, or 0 if `+0x624` is 0 [r1]. Do not patch the producer `0x44bb9f0`: it also feeds A readers. AOB in TICK_CONSUMERS fix 1. Accept: P-X sub-steps/s = 60 ±1% ON and OFF.
2. GetFps allowlist: detour GetFps. If the return address is in the allowlist (a sorted array, binary search), return the effective rate, otherwise 120. [r1]
   - Rate-type sites (steps, velocity, per-tick countdowns): one dithered value per (site, tick), so every actor in a tick gets the same value.
   - Threshold/capacity sites (e.g. `0x12b619`, a deque capacity): plain round(120/r).
   - The `0x42f0fc` 1/fps helper is classified per outer caller: hook the B outer callers' call sites, or key the allowlist on the second-level return address.
   - An allowlist (not a denylist) is the default, because a denylist would feed ~95 to the `==120`-keyed mode tables `0x5af687`/`0x5af6a4`.
   - Each entry is resolved by its unique caller AOB, and the `E8 rel32` must target GetFps.
   - Mode-table callers (30/60/120 keyed) are on an explicit denylist, which is asserted never to be allowlisted.
   - Allowlist sites from run1 only (outside the shrine) are included; their live confirmation comes from P-G in other areas later.
3. Inline `+0x614` stubs at `0xe7050d` (`xmm13 *= r`) and `0x117c051` (`xmm6 *= r`), plus input hold `0x29786a6` (`xmm6 *= r`). Same stub technique and unwind discipline as P1/P2.
4. Physics `0x2de300`: only if P-Ph shows a constant per-asset step and `0x2d4285` allows a raised substep count [r1]. Otherwise defer, and add a ragdoll/gadget visual check: deferred physics runs at 1/r.
5. Governor: if P-Gov shows `0x45baf58 != 0` or a mode change during gameplay, disarm the gate (already happens through the `+0x618 == 120` check) and log it. No patch.

**Update resilience (tiered) [r1].**
- Core tier, all-or-nothing: the X leaf, the GetFps hook, the inline stubs, and the allowlist entries that together carry ≥ 99% of the live B hit volume (from P-G). If any core item fails to resolve uniquely, L2/L3 stay passive for the session. The game then runs native 120 Hz (old judder, correct speeds), with a STATUS line naming the site.
- Rare tier: each entry is skipped individually with a STATUS line.
- Runtime guard: count GetFps calls per second from return addresses that are in neither the allowlist nor the known C/denylist set. If unknown callers are above 10/s for 5 s while armed, log `UNKNOWN HOT CALLER ret=…`. A game patch can add callers no AOB knows about.

## 3. Acceptance (S2C-F, camera-only, me)
- P-X: steps per accumulator per second within 1% of 60 on both paths, with the gate ON and OFF.
- Governor [r1]: 0 fps-mode changes and 0 gate arm/disarm transitions over the 5-minute sweep.
- Hit-stop [r1]: measured from a frame capture (ON vs OFF duration), not by feel. This is a user-assisted check, since it needs combat.
- Online [r1, release BLOCKER carried from v8]: find a session/online flag, and make the gate disarm on it before any release.
- P-G: every hot (>10/s) caller seen live in gameplay is either allowlisted, denylisted or classified C in TICK_CONSUMERS. An unknown hot caller blocks release.
- The M1-M5 regression stays within S2L numbers.
- ON/OFF A/B 10× through ToggleFile, no hitch > 50 ms (M6).
- User checklist (buttons): walk/run speed vs OFF over a fixed distance, dodge/i-frame feel, Ki/stamina regen time to full (stopwatch, ON vs OFF), a timed buff, pause, cutscene, fast travel, alt-tab, combat hit-stop. This covers the per-tick `++` counters that static RE cannot find.

## 4. Risks
| Risk | Mitigation |
|-|-|
| X analysis wrong; fix would slow characters | P-X measures it first; fix only on confirmation |
| Unknown per-tick counters run slow | user stopwatch checks; F10 A/B; release notes |
| GetFps int rounding jitter | per-(site, tick) dithering; float stub for `int(2fps/60)`-type sites |
| Allowlist caller shared by a mode table | denylist assert; per-site AOB checks the call target |
| Game patch moves or changes a site | tiered resilience plus the unknown-hot-caller guard |
| Physics stability with a larger step | verify 0x2d4285; defer with a visual check |
| Governor oscillation under lockstep [r1] | P-Gov; transition count in acceptance |
| Co-op at a variable tick [r1] | online flag disarms the gate; release blocker |

## 5. Review r2 (YES for S2C-P) [r2]
1. MAJOR for S2C-F: the X override path needs its own fix. Ret `0x927bce` (60/GetFps) becomes a conditional allowlist entry, decided by the P-X override-path result. The `[x+0x40]` branch is classified from probe data (stub it if it is B). The override path's expected value is not assumed to be 60/r.
2. Second-level return for `0x42f0fc`: read it at GetFps entry from `[rsp + helper_frame + 8]`, only when the first-level ret is `0x42f105`. helper_frame (0x28) is decoded from the helper's `sub rsp,imm8` in its AOB, not a constant.
3. The dither tick id is the H1-published seq, stored next to r (one 16-byte pair, published by H1), so jobs that spill over a tick boundary don't mix values. Sites of the `int(2fps/60)` type (e.g. `0xefafc`) get a float stub instead of round() (round() still leaves about 5% quantisation error).
4. The unknown-hot-caller guard's known set includes the hot A/C callers: `0x27ec52`, `0x68894a`, `0x231cb1`, `0x8b28b8`, `0x3cdb69`, `0x435935`, `0x55dc51` (each AOB-resolved; seed the set from P-G data).
5. Governor: if P-Gov finds the setting that makes `0x45baf58` non-zero, name it in the release notes, because the mod is effectively off for those users.
