# Nioh 3 variable-framerate mod — plan v7

Goal: smooth gameplay uncapped at 80–120 fps (FreeSync) by giving Nioh 3 real elapsed time, as Wo Long 2 does.
Process: explore → plan → independent review until error-free → implement → review implementation until error-free.
v2 incorporated review round 1, v3 round 2, v4 round 3, v5 round 4, v6 round 5, v7 round 6. Changes are marked **[v2]**…**[v7]**. Previous versions: PLAN_v2.md – PLAN_v6.md; review notes: REVIEW_R1.md – REVIEW_R6.md.

## 1. Findings (C = confirmed, L = likely). All addresses are RVAs in Nioh3.exe.unpacked.exe.

| # | Finding | Status |
|-|-|-|
| F1 | Clock object = `*(base+0x4759910)` (also reachable as `rcx` of the clock fn). `+0x614` int fps; `+0x618`/`+0x620` requested fps, set by `0x5af604` (returns {30,60,120}[idx]); `+0x614` ← `+0x618` at `0x5ad677`, *after* clock entry | C (3 reviewers) |
| F2 | Clock fn `0x5ad5f8` (.pdata 0x5ad5f8–0x5ad8ec). Single caller `0x5acf56` in task callback `0x5acee8` (vtable `0x40358e8`, single instance stored in `0x4bd15e8`). **[v2]** It runs on job-system workers (`KIDSTaskScheduler%d`), so the thread varies per frame. Once per graph run, no catch-up loop; non-overlap is not proven | C / L |
| F3 | Patch site P1 `0x5ad6a2`: `movss xmm2,[rdi+0x628]` (8 B) / `movss [rdi+0x624],xmm2` (8 B). No branch targets inside (whole-.text scan). rax and flags are dead; the cmp at 6b2 resets the flags. After it, xmm2 → pause zeroing (6c2/6d5) → `+0x62c` accumulator (702) and `+0x624` reloads (727, 795, 80e). The per-tick dt passed onward (`+0x624`/fps → param struct +0x44) is scaled | C (3 reviewers) |
| F4 | `+0x628` requested speed (only SetGameSpeed `0x27051e4` writes it); `+0x624` effective speed | C |
| F5 | `+0x62c` = frac + speed·60/fps. Its only integer reader is `0x5a535c` → `0x5a540c`: `if(int(acc)) call 0x5a6bb0`, so at most 1 legacy tick per frame. **[v3 qualified]** At speed ≤ 1 with r ≤ 2 the increment is ≤ 1, so the long-run rate is 60 Hz of real time *while r is unclamped*; below 60 fps (r clamped) it is lower, as in vanilla | C |
| F6 | GetFps `0x46594` (129 callers), GetTimeStep `0x46484` = `+0x624`·60/fps (65 callers). Duplicate `0xe3a750` and variant `0x46398` have no direct callers | C |
| F7 | All 129 GetFps sites are unscaled. Provisional classes (sites_classified.csv): effects/render 17, audio 16, AI 12, input 3+, clock conversions 5, misc 5, unknown ~71. All GetTimeStep sites are scaled | C / L |
| F7b | **[v2]** 56 *inline* `+0x614` reads bypass GetFps (e.g. `0xe4952c`, `0xe501f6`, `0xe5ed0b`, `0xf0e17f`) | C |
| F7c | **[v2]** Raw game-speed readers: `0x42dde4` (one caller `0x42dcc8`), `0x434df4`, `0x10f7de3` (range compare), `0x26f4768`/`0x26f4780`, `0x2a0a44` (likely a script property). After the patch they see a speed that wobbles around 1.0 | C (existence) |
| F7d | **[v2]** The clock fn builds a 60/fps field at `0x5ad7b8`/`0x5ad825` ([rsp+0x48]), passed to `0x8c8e54`/`0x5ab644`. Consumer unknown | C |
| F7e | **[v2]** `0x5adc64` copies `+0x614..+0x638` into another clock object. `0x5ad964`/`0x5ad9ec` snapshot the clock before and after the tick (readers of the snapshot see finished values) | C / purpose L |
| F8 | Integer frame timers: `0x435864`, `0x437cd8`, `0x43837c`, `0x55dbf0`, `0x5af670`, `0x1975b90`, `0x2977014`, `0x2977144`. Likely input hold/repeat | C / L |
| F8b | `+0x634`/`+0x64b` countdown (n=10 from state transitions `0x565b81`/`0x566c05`): resets temporal/history state for N frames after a cut (`0x27051ac`). `+0x638`/`+0x64d` is a sibling. Cosmetic, fine per-frame | L |
| **F9** | **[v2] Main update `0x39e4e4` computes `dt = 60.0 / S.fps` at P2 `0x39e526` (S = per-frame context from rbx; S+0x10 int fps, S+0x14 speed, S+0x18 speed·60/fps). It ignores speed, and dt=0 when `0x46518()==5` (pause). This dt drives the CAMERA (→ task `0x8bee50` → `0x565ddc` → camera manager `0x567070` → mode update, vslot 1, arg 6) plus ~25 other systems (`[0x4745348]` manager, stage_hit `0x3202ac`, `0x319930`, `0x2e7b6c`, `0x2819a0`, `0x39f7f8`…)** | C (static, end-to-end) |
| F9b | **[v3]** S (rbx at P2) is a stack struct built in `0x5ac664` (clock class vtable `0x3b1f308` slot 1, at `0x5ac725–0x5ac74c`) from `clk+0x614`/`+0x624`, *not* the clock snapshot. Chain: `0x5ac664 → 0x5ac9ec → 0x564f40 → 0x564f60 (vslot+0x30) → 0x565198 → 0x5651d0 → 0x39e4e4`, once per `0x5ac664` call. Who calls `0x5ac664` (virtual), and its order vs tick task `0x5acee8`, is unknown | C / unknown |
| F9c | **[v3]** P2 site: `0x39e4e4` spans `0x39e4e4–0x39e867`. Nothing branches into `0x39e521–0x39e536`; the movss at `0x39e526` is 8 B (`F3 0F 10 3D`) and MinHook 1.3.3 relocates its rip-relative operand. xmm7 = dt; xmm6 = dt only when `0x46518()==0`, otherwise 0. dt → `0x39f34c(S, xmm6, xmm7)` → job `0x8bee50` (+0x48 = xmm6, +0x4c = xmm7) → `0x565ddc` → `0x567070` (xmm6 → `0x842548`, `0x9a7450`, mode vslot1 arg6; xmm7 → `0x568e90`). Only dt>0 checks and a frame accumulator at `0x5656d3` found 2 calls deep; no int conversion | C |
| F9d | **[v3]** A second camera path: `0x567070` is also called from `0x20dda0c` ← `0x20e20d0` (vtable `0x402d868`), with dt from obj+0x1a8/+0x1ac (copied by render fn `0x415080` via `0x8ddc44`/`0x8ddc88`; origin unknown) | C / origin unknown |
| F9e | **[v3]** Other own `60/S.fps` computations not covered by P2: `0x5acb36` (→ `0xa4609c`, gated by flag +0x2f7), `0x271590d`, `0x271827a` (frame accumulators compared against 10 and 3) | C |
| F10 | Physics SubStepCount fixed per asset; no max-dt clamp found | C / L |
| F11 | Cutscenes fixed at 60 (user). Menus/startup run in 60 mode, gameplay in 120 | C |
| F12 | **[v2 corrected]** In 120 mode the step is fixed at speed/120 per frame, so game speed = fps/120. *Inferred* from the code + logged fps/speed; no uncapped or hit-stop run captured yet | C mechanism / L magnitude |
| F13 | No anti-cheat. Steam P2P co-op; lockstep unknown | L |
| F14 | Nioh3.exe imports DINPUT8 (only DirectInput8Create, static, not in KnownDLLs) and WINMM. System dinput8 has 6 exports @1–@6. OptiScaler has no dinput8; its `.asi` loader is off by default | C |
| F15 | SteamStub 3.1: `.text` is encrypted on disk and decrypted at runtime, *after* static-import DllMains. The global is BSS (zero at load) | C |
| F16 | **[v2]** Frame loop `0x577e50`: clock vfunc +0x30 → `0x4a529c` (xellSleep `0x4a538c` + Reflex markers) → task graph. Frame gen shows 2 presents per sim tick, so time must be measured at sim ticks, never presents | C / L |

## 2. Approach **[v3: two independently timed patch points]**

Each patch point gets its own real-time factor, measured from the period between its own once-per-frame calls. No ordering between them is assumed (F9b).

- **P1 (game time)** at `0x5ad6a2`: `[+0x624] = [+0x628] · r1`. `r1` is computed in the clock-entry hook (`0x5ad5f8`) from the QPC delta between clock entries, with `fps = [rcx+0x618]` (this tick's value). Covers speed-scaled consumers, legacy 60 Hz ticks and the per-tick param dt. Hit-stop/slow-mo preserved (multiply only; `+0x628` never written).
- **P2 (camera + main update)** at `0x39e526`: `dt = (60.0 / eax) · r2` **only if eax == 120**, otherwise the original math. `r2` is computed in an entry hook on `0x39e4e4` (prologue `mov rax,rsp; mov [rax+8],rbx` = 7 B, no rip-relative; verified r3/r4) from the QPC delta between its own entries, with `fps = [rdx+0x10]` (S.fps). Speed stays excluded (original design). The pause path that branches around P2 is untouched.
- If S1 proves a fixed order (P1 always before P2 in the same frame), the S2 plan revision may switch P2 to reuse `r1`; until then they are independent.

**Factor computation [v7] (identical for r1/r2, separate state per point):**

State: `qpc_prev` (0 = none), `prev_fps`, `prev_enabled`, `hist[N]` ring, `debt` (seconds owed to the game), shared `r` (r1 or r2).
Config, validated at ini parse (defaults in brackets): `TimingMode` [Raw], `ScaleMin ∈ [0.5, 1.0]` [0.5], `ScaleMax ∈ [1.0, 2.0]` [2.0] (F5 needs r ≤ 2), `AvgFrames ∈ [1, 32]` [8], `SmoothK ∈ (0, 1]` [0.2]. Invalid → default + log line.
```
if overlap_counter_after_increment > 1:                     # another worker is inside this hook
    log_overlap(); skip to call_original                    # change NOTHING: P1/P2 use the current r
# (this block is the factor update only; the detour always calls the original/trampoline, then decrements the counter)
now   = QPC()
first = (qpc_prev == 0)
dt    = first ? 0.0 : (double)(now - qpc_prev) / (double)qpf     # floating-point division
qpc_prev = now                                              # always updated (non-overlapping calls)
reset = first | enabled != prev_enabled | fps != prev_fps | fps != 120 | (!first && dt <= 0)
hitch = !reset && ScaleMax > 1.0 && dt >= ScaleMax / fps    # any frame at/above the max step (incl. > 0.25 s); with ScaleMax = 1 it is a plain clamp
prev_fps = fps; prev_enabled = enabled
if !enabled or SafeMode or reset:
    hist.clear(); debt = 0; r = 1.0                          # passthrough this tick
elif hitch:
    hist.clear(); debt = 0; r = ScaleMax                     # max step; history restarts, nothing repaid
elif Mode == Raw:                                            # default
    r = clampNaN(dt * fps)
else:                                                        # Mode == Smooth (optional)
    hist.push(dt)                                            # dt < ScaleMax/fps here, so no clamping needed
    target = mean(hist)
    owed   = debt + dt
    given  = target + SmoothK * (owed - target)
    given  = clamp(given, owed - 1/fps, owed + 1/fps)        # [v7] true drift bounded to ±1 frame
    r      = clampNaN(given * fps)
    debt   = owed - r/fps                                    # exact; only beyond ScaleMin/Max can it exceed 1 frame
    if |debt| > 1/fps: dropped += |debt| - 1/fps; debt = clamp(debt, -1/fps, +1/fps)   # logged, not silent
clampNaN(x) = isnan(x) ? 1.0 : min(max(x, ScaleMin), ScaleMax)
```
- **Raw is the default.** Smooth is optional, only enabled if S1/S4 show visible noise in raw dt. In Raw, time lost to clamps or hitches is intentionally *not* repaid (no burst afterwards). In Smooth, drift is bounded to ±1 frame and up to one frame of a clamp is repaid. Anything beyond is counted as `dropped` in the 10 s summary. The simulations in `review3/`–`review5/` are re-run against this exact pseudo-code in the next plan review and in the implementation review.
- Any frame long enough to hit the cap is a hitch, so a sustained dip below 60 fps (at ScaleMax 2.0) runs at r = ScaleMax with an empty history, and recovery is immediate (fixes r5 MAJOR1).
- If H2 is skipped for some frames (early-outs in `0x5651d0`), its next dt spans them. That is correct, because those frames had no main update.
- `r` starts at 1.0 and the data page is initialised to 1.0 before any hook is enabled.
- **SafeMode** = all hooks installed and all logging active, but r forced to 1.0. It answers "is it the mod's timing?" without uninstalling.
- The toggle and config are sampled **only at hook entry** (each point latches its own `enabled`; a toggle forces one passthrough tick at each point). The toggle key is read via `GetAsyncKeyState` at clock entry, edge-detected, only when the game window is foreground.
- Passthrough, hitch and overlap counts are logged in the 10 s summary.

**Camera-only fallback** (if S2 finds the P2 dt must stay per-frame for some of the ~25 systems): drop P2 and hook the entry of `0x39f34c` (single caller `0x39e707`, first 7 B `48 83 EC 28 48 8B D1`, no rip-relative), scaling xmm1 and xmm2 by r2 before calling the trampoline. Never hook `0x567070` itself (second path F9d, double scaling).

Rejected alternative: writing a fake integer "real fps" into `+0x614`. Tables key on exactly 30/60/120 (`0x5af604`, `0x947250`) and integer `120/fps` timers would break.

## 3. Delivery **[v2–v5: all review fixes]**

- `dinput8.dll` proxy. The `.def` maps `DirectInput8Create=Proxy_DI8Create @1`, `@2..@6` for the rest, with the Dll* exports `PRIVATE`. Each stub lazily `LoadLibraryW(GetSystemDirectoryW()+"\dinput8.dll")` on first call (never in DllMain) and returns `E_FAIL`/`DIERR_*` on failure. No .def forwarders.
- DllMain: pin the module (`GET_MODULE_HANDLE_EX_FLAG_PIN`), `CreateThread(init)`, nothing else. No C++ static constructors, `thread_local` or `std::thread`. Built with `-static`; `objdump -p` must show only KERNEL32 + `api-ms-win-crt-*` (+ user32/dbghelp if used).
- Init thread readiness (poll 50 ms, timeout 120 s, otherwise PASSIVE):
  1. `.text` is committed and executable (VirtualQuery);
  2. the GetFps AOB matches exactly once; derive `G` from its rip-relative operand;
  3. all AOBs (clock entry, P1, `0x39e4e4` entry, P2, optional xellSleep site) match exactly once, **at their expected RVA** (return addresses are base+RVA, so a unique match elsewhere means an unknown build → PASSIVE), and the hooked bytes equal the expected decrypted bytes;
  4. `*G != 0`.
- `MH_Initialize` in the init thread. **All hooks via MinHook** (freezes threads, fixes IPs). All return addresses are computed at runtime as `base + RVA`:
  - **H1** clock entry `0x5ad5f8` (prologue `40 55 53 56 57`, 5 B). The detour has the same signature and forwards all 4 args (rcx, rdx, r8, r9; r9 is used at `0x5ad628`) and the return value to the trampoline. It computes r1 (section 2), interlocked overlap counter, incremented on entry and decremented only after the trampoline returns (>1 → that call changes no state at all and logs; P1 keeps using the current r1), frame counter, toggle sampling;
  - **P1** `0x5ad6a2` (8 B, not rip-relative). Detour stub: `movss xmm2,[rdi+0x628]; mulss xmm2,[r1]; movss [rdi+0x624],xmm2; mov rax, imm64(base+0x5ad6b2); jmp rax` (rax is dead, r1/r3). Trampoline unused;
  - **H2** `0x39e4e4` entry (prologue `mov rax,rsp; mov [rax+8],rbx`, 7 B, no rip-relative; verified r3). The detour forwards rcx, rdx and the rax return value, and computes r2 with `fps = [rdx+0x10]`. Overlap counter;
  - **P2** `0x39e526` (8 B rip-relative movss; MinHook still builds a trampoline, which is unused, as for P1). Detour stub: `movss xmm7,[c60]; cvtsi2ss xmm0,rax; divss xmm7,xmm0; cmp eax,120; jne skip; mulss xmm7,[r2]; skip: mov rax, imm64(base+0x39e537); jmp rax` (rax is dead after: 0x39e537 is a call). Uses only xmm7/xmm0/flags (flags are dead: next is a call). eax was loaded at `0x39e520`;
  - optional `xellSleep` timestamp (S1 data only).
- Stub memory: one allocation of 2 adjacent pages near the exe. Page 1 = stubs (written, then RX); page 2 = RW data (`r1`, `r2`, `c60`, counters), each value on its own 64-byte line. All data access is rip-relative within the allocation. Returns use `mov rax,imm64` (`48 B8` + 8 B) then `jmp rax` encoded exactly as `FF E0` (never `48 FF E0`, which the unwinder reads as an epilogue), not `FF25`, because the unwinder treats `jmp [rip]` as an epilogue.
- **Unwind [v5, verified r5 by simulated unwind at every stub offset]:** each stub runs inside its host's frame (P1 in `0x5ad5f8`, P2 in `0x39e4e4`; neither host is chained). For each stub, copy the host's UNWIND_INFO into page 1 (`4 + 2*((CountOfCodes+1)&~1)` bytes, CountOfCodes unchanged, copy at a 4-byte-aligned address) with **Flags = 0** (drops the host's EHANDLER/UHANDLER, whose RVA would resolve against our allocation), **SizeOfProlog = 0**, and **the CodeOffset byte set to 0 in the first slot of every unwind code** (never in extra slots, which hold sizes and offsets), so every code applies at any stub offset. FrameRegister unchanged (0). Host facts (r4): version 1, not chained, prologs 33/37 B with code offsets 2–19 and 12–37; P1/P2 sit after the prologs. Build a sorted RUNTIME_FUNCTION table on page 1 (BeginAddress/EndAddress/UnwindData relative to the allocation base) and call `RtlAddFunctionTable(tbl, n, allocBase)` before enabling hooks. The implementation review verifies with `RtlVirtualUnwind` from each stub instruction that the walk reaches the host's caller. **Accepted gap:** the MinHook relay and the H1/H2 trampolines have no unwind info; a stack walk caught exactly there may be truncated. This affects only diagnostics, never execution.
- `r1`/`r2` are aligned floats, each written once per tick by its own hook before its stub runs.
- `nioh3fps.ini`: `Enabled`, `StartEnabled`, `SafeMode`, `ToggleKey=F10`, `DumpKey=Ctrl+F10`, `TimingMode=Raw|Smooth`, `AvgFrames`, `SmoothK`, `ScaleMin/Max`, `LogLevel` (ranges and SafeMode defined in section 2). Exe never modified; uninstall = delete files.

## 4. Stages and gates

**S0 Prep.** Locate and back up the Nioh 3 saves (not under Documents\KoeiTecmo). Play offline until S5. **[v2]** Capture an n3mon run with F9 markers: uncapped ~80 fps segment, hit-stop, pause, cutscene exit. This backs F12 and shows whether `+0x628` ever changes (hit-stop) — needed before relying on either.

**S0.5 Probe target list [v3: narrowed].** Deliverables only:
1. `probe_targets.csv`: the functions the probe entry-counts. These are the containing functions of the 56 inline `+0x614` reads (F7b), raw speed readers (F7c), the F7d consumer candidates (`0x8c8e54`, `0x5ab644`), `0x46398`, `0x5adc64`, the ~25 consumers called from `0x39e4e4` (F9), `0x20dda0c` (F9d), the F9e sites and the legacy tick `0x5a6bb0` (for test S4.2). Each needs a hookable prologue (≥ 5 B, relocatable) checked by script; otherwise the nearest hookable caller is used.
2. `sites.csv` schema: `site_rva, containing_fn, caller_chain, class{per-frame-ok|scale|timer|speed-exact|unknown}, evidence, hits_per_s`. Prefilled from `rev-sites/sites_classified.csv` with class `unknown` where not proven.
Exit: both files exist and every target has a hook decision. Classification proper happens in S2, on S1 data.

**S1 Probe build (no behaviour change; r1 = r2 = 1.0 forced, all hooks installed).** Logs:
- per tick at H1/H2: QPC, thread ID, frame counter, fps (`+0x618`/`+0x614`/S.fps), speed req/eff, overlap counters, optional xellSleep QPC;
- ordering: per frame, the sequence and QPC of H1, H2, the camera update `0x567070` and `0x20dda0c` (F9d);
- per-second entry-count histograms for GetFps/GetTimeStep return addresses and all `probe_targets.csv` functions (per-thread counters, merged by the writer thread);
- the would-be r1/r2 (computed but not applied), to validate the factor math offline.
F9 markers via the section-6 logging.

**Gate G1 (camera) [v2]:** answered statically: the camera uses the unscaled main-update dt, so P2 is required. S1 must confirm at runtime that `0x567070` runs once per frame with the P2 dt.
**Gate G1b [v3]:** (a) zero clock/H2 overlaps across a 30-min session; (b) H1/H2 ordering documented, and H2 fires exactly once per H1 tick (a second H2 call per frame would halve r2: `0x5ac664` is virtual and there is a second clock copy F7e); (c) `0x20dda0c` call rate known, so it's clear whether the second camera path is live in gameplay; (d) every site in `sites.csv` with hits > 0 classified, none `unknown`. S3 cannot start until G1b passes.

**S2 Per-site decisions → next plan revision → independent review.** For each hot unscaled site: (a) leave (UI, effects that are fine per-frame, F8b cut-reset); (b) scale by r (patch); (c) frame timers gating gameplay: convert by r or accept, with measured worst-case error at 80 fps. Raw speed readers (F7c): decide whether they need the *unscaled* requested speed (e.g. `==1.0` checks), and if so redirect them to `+0x628`.

**S3 Fix build.** P1 + P2 + S2 decisions; toggle default off.

**S4 Test matrix (offline; logging + markers).**
1. Locked 120: r ≡ 1.0, identical behaviour.
2. Uncapped 80–120: game-time accuracy measured on the **game's own clock**: the legacy tick rate (`0x5a6bb0` calls per second, counted by an entry hook, added to probe targets) must be 60 ± 0.3 over 10+ minutes, excluding windows with clamps/resets (reported separately as "time intentionally dropped"). Cross-check with the `+0x62c` progression in n3mon; camera pan smooth by eye with F10 A/B.
3. Dodge / parry / Ki Pulse timing at 80 vs 120.
4. Hit-stop, slow-motion effects, Living Weapon equivalent; F10 toggled during hit-stop and pause.
5. Audio pitch/speed at 80 fps (16 audio sites, F7c readers).
6. Pause, loading, fast travel, death/respawn, alt-tab and alt-tab while paused, minimise.
7. Cutscenes (60 → passthrough) and cutscene exit (60→120 transition, no speed burst).
8. Photo mode.
9. Cloth/hair/ragdoll at 80 fps and across a hitch.
10. FSR4 / OptiScaler frame gen on and off.
11. Save + reload with the mod on; confirm the save is unaffected.

**S5 Co-op.** Only after S4: both modded, then mixed. On desync → auto-disable in co-op (detection to be researched).

**S6 Implementation review loop** until clean → release build.

## 5. Risks

| Risk | Mitigation |
|-|-|
| Some of the ~25 systems fed by the P2 dt must stay per-frame | S1/S2 classification; camera-only fallback at `0x39f34c` entry (section 2) |
| Second camera path `0x20dda0c` (F9d) stays per-frame | S1 counts it (G1b c). **If live**, fallback candidates, in order: (1) scale the dt values where render fn `0x415080` copies them into obj+0x1a8/+0x1ac (`0x8ddc44`/`0x8ddc88`), after S1 identifies their source; (2) hook the call from `0x20dda0c` to `0x567070` and scale the dt arguments for that call only (never the `0x567070` entry, which would double-scale the P2 path). The chosen fallback is specified and reviewed in the S2 plan revision before S3 |
| P1/P2 factors differ slightly (independent timing) | both measure the same frame period; S1 compares would-be r1 vs r2 |
| Unscaled GetFps/inline sites (AI, audio, input) and F9e own-60/fps sites drift at < 120 fps | S0.5 targets → S1 counts → S2 decisions |
| Raw speed readers misfire on speed ≈ 1.0 ± ε | F7c redirect to `+0x628` where needed |
| Physics instability | clamp 2.0, hitch reset, test 9 |
| Clock overlaps across workers | interlocked counter → passthrough + log; G1b |
| Game update | exact-once AOBs, byte checks, G derived from AOB, otherwise PASSIVE |
| Co-op desync | S5, kill switch |
| Drift between game time and real time | Raw mode is exact by construction; Smooth mode drift bounded to ±1 frame by clamping `given` to owed ± 1/fps (verified r6 sim6c); test 2 measures the game's own legacy tick rate |
| Saves | S0 backup, test 11 |

## 6. Distribution and remote troubleshooting

Goal: any user can send one zip that lets us diagnose their problem without a back-and-forth.

**Always-on log `nioh3fps.log`** (INFO, small)
- Header: mod version + build ID; game exe version, PE timestamp, SizeOfImage, CRC of the decrypted patch regions; Windows build; CPU; ini found?; effective config.
- Environment: the game folder's DLLs with sizes (OptiScaler, ReShade, Special K, other proxies). Paths sanitised (`%USERPROFILE%`).
- Each startup check: readiness wait time, AOB name → hits → RVA, expected-vs-actual bytes on failure, each MinHook result as a status string.
- One status line: `STATUS: ACTIVE` / `PASSIVE (reason)` / `DISABLED BY CONFIG`. A game update shows up as `PASSIVE (unsupported game build <timestamp>)`.
- Every 10 s: real fps avg/min/max, r avg/min/max, clamp hits, hitch resets, fps-mode changes, overlap count, game-time accuracy.
- Toggle events, mode changes, passthrough reasons.
- 5 MB rotation with one `.old`; written by a worker thread, never the game thread.

**Flight recorder [v4]:** ~30 s ring buffer in a file-backed mapping `nioh3fps_trace.bin`, preallocated at about 1 MB (≈ 240 records/s × 30 s × 128 B, rounded up), with a header of build ID, record size and write index. Dirty pages of a mapped file survive process crashes and kills (not BSOD or power loss). At startup the previous file is renamed to `nioh3fps_trace.prev.bin` first, so the previous session's crash trace is kept. Writers reserve a slot with an interlocked increment, write the fields, then write a sequence number last (seqlock) plus a CRC32, so the converter drops torn or partial records. If the game folder isn't writable, the file goes to `%LOCALAPPDATA%\nioh3fps\`. Each record holds the per-tick fields: both timestamps, fps, speeds, r1/r2, debt, passthrough reason, thread ID, toggle, frame counter. The collect script converts both files to CSV. The writer thread also dumps a CSV snapshot on Ctrl+F10 or an internal assertion (overlap, unexpected ordering). `LogLevel=Debug` enables the S1 call-site histograms in release builds.

**Crash capture [v3: no global filter]:** the game already installs its own UnhandledExceptionFilter and writes its own minidumps, so we install **no UEF**. One vectored exception handler acts only when the faulting IP is inside our DLL, stub pages or MinHook trampolines. It signals a pre-created dump thread (`MiniDumpWriteDump` + log line with module+offset), waits for it with a 10 s timeout, then returns `EXCEPTION_CONTINUE_SEARCH`, so the game's own handling still runs. A crash in game code is not handled by us at all. Release builds are stripped; an unstripped twin + `.map` is archived per build ID.

**Threading rule for diagnostics [v3]:** game/worker threads only write to preallocated lock-free buffers and set flags. All file I/O (log, trace dumps on Ctrl+F10 or assertions, rotation) happens on the single writer thread.

**User kit:** `Collect-Nioh3FpsLogs.bat` (log, `.old`, ini, traces, dumps, folder listing → `Desktop\nioh3fps_report_<time>.zip`); documented ini with `SafeMode`; README (install, uninstall, keys, known issues, co-op status, how to report); MinHook BSD-2 licence.

**Compatibility policy:** unknown bytes → PASSIVE with a clear status, never a guess. Supporting a game patch = re-run the AOB scan script and bump the version.
