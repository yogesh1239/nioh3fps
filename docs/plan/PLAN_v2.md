# Nioh 3 variable-framerate mod — plan v2

Goal: smooth gameplay uncapped at 80–120 fps (FreeSync) by giving Nioh 3 real elapsed time, as Wo Long 2 does.
Process: explore → plan → independent review until error-free → implement → review implementation until error-free.
v2 incorporates review round 1 (whole-plan, patch-site, threading, delivery, sites). Changes vs v1 are marked **[v2]**.

## 1. Findings (C = confirmed, L = likely). All addresses are RVAs in Nioh3.exe.unpacked.exe.

| # | Finding | Status |
|-|-|-|
| F1 | Clock object = `*(base+0x4759910)` (also reachable as `rcx` of the clock fn). `+0x614` int fps; `+0x618`/`+0x620` requested fps, set by `0x5af604` (returns {30,60,120}[idx]); `+0x614` ← `+0x618` at `0x5ad677`, *after* clock entry | C (3 reviewers) |
| F2 | Clock fn `0x5ad5f8` (.pdata 0x5ad5f8–0x5ad8ec). Single caller `0x5acf56` in task callback `0x5acee8` (vtable `0x40358e8`, single instance stored in `0x4bd15e8`). **[v2]** It runs on job-system workers (`KIDSTaskScheduler%d`), so the thread varies per frame. Once per graph run, no catch-up loop; non-overlap is not proven | C / L |
| F3 | Patch site P1 `0x5ad6a2`: `movss xmm2,[rdi+0x628]` (8 B) / `movss [rdi+0x624],xmm2` (8 B). No branch targets inside (whole-.text scan). rax and flags are dead; the cmp at 6b2 resets the flags. After it, xmm2 → pause zeroing (6c2/6d5) → `+0x62c` accumulator (702) and `+0x624` reloads (727, 795, 80e). The per-tick dt passed onward (`+0x624`/fps → param struct +0x44) is scaled | C (3 reviewers) |
| F4 | `+0x628` requested speed (only SetGameSpeed `0x27051e4` writes it); `+0x624` effective speed | C |
| F5 | `+0x62c` = frac + speed·60/fps. Its only integer reader is `0x5a535c` → `0x5a540c`: `if(int(acc)) call 0x5a6bb0`, so at most 1 legacy tick per frame. With scale ≤ 2 at speed 1 the increment is ≤ 1, so the long-run rate is exactly 60 Hz of real time | C |
| F6 | GetFps `0x46594` (129 callers), GetTimeStep `0x46484` = `+0x624`·60/fps (65 callers). Duplicate `0xe3a750` and variant `0x46398` have no direct callers | C |
| F7 | All 129 GetFps sites are unscaled. Provisional classes (sites_classified.csv): effects/render 17, audio 16, AI 12, input 3+, clock conversions 5, misc 5, unknown ~71. All GetTimeStep sites are scaled | C / L |
| F7b | **[v2]** 56 *inline* `+0x614` reads bypass GetFps (e.g. `0xe4952c`, `0xe501f6`, `0xe5ed0b`, `0xf0e17f`) | C |
| F7c | **[v2]** Raw game-speed readers: `0x42dde4` (one caller `0x42dcc8`), `0x434df4`, `0x10f7de3` (range compare), `0x26f4768`/`0x26f4780`, `0x2a0a44` (likely a script property). After the patch they see a speed that wobbles around 1.0 | C (existence) |
| F7d | **[v2]** The clock fn builds a 60/fps field at `0x5ad7b8`/`0x5ad825` ([rsp+0x48]), passed to `0x8c8e54`/`0x5ab644`. Consumer unknown | C |
| F7e | **[v2]** `0x5adc64` copies `+0x614..+0x638` into another clock object. `0x5ad964`/`0x5ad9ec` snapshot the clock before and after the tick (readers of the snapshot see finished values) | C / purpose L |
| F8 | Integer frame timers: `0x435864`, `0x437cd8`, `0x43837c`, `0x55dbf0`, `0x5af670`, `0x1975b90`, `0x2977014`, `0x2977144`. Likely input hold/repeat | C / L |
| F8b | `+0x634`/`+0x64b` countdown (n=10 from state transitions `0x565b81`/`0x566c05`): resets temporal/history state for N frames after a cut (`0x27051ac`). `+0x638`/`+0x64d` is a sibling. Cosmetic, fine per-frame | L |
| **F9** | **[v2] Main update `0x39e4e4` computes `dt = 60.0 / S.fps` at P2 `0x39e526` (S = per-frame context from rbx; S+0x10 int fps, S+0x14 speed, S+0x18 speed·60/fps). It ignores speed, and dt=0 when `0x46518()==5` (pause). This dt drives the CAMERA (→ task `0x8bee50` → `0x565ddc` → camera manager `0x567070` → mode update, vslot 1, arg 6) plus ~25 other systems (`[0x4745348]` manager, stage_hit `0x3202ac`, `0x319930`, `0x2e7b6c`, `0x2819a0`, `0x39f7f8`…)** | C (static, end-to-end) |
| F10 | Physics SubStepCount fixed per asset; no max-dt clamp found | C / L |
| F11 | Cutscenes fixed at 60 (user). Menus/startup run in 60 mode, gameplay in 120 | C |
| F12 | **[v2 corrected]** In 120 mode the step is fixed at speed/120 per frame, so game speed = fps/120. *Inferred* from the code + logged fps/speed; no uncapped or hit-stop run captured yet | C mechanism / L magnitude |
| F13 | No anti-cheat. Steam P2P co-op; lockstep unknown | L |
| F14 | Nioh3.exe imports DINPUT8 (only DirectInput8Create, static, not in KnownDLLs) and WINMM. System dinput8 has 6 exports @1–@6. OptiScaler has no dinput8; its `.asi` loader is off by default | C |
| F15 | SteamStub 3.1: `.text` is encrypted on disk and decrypted at runtime, *after* static-import DllMains. The global is BSS (zero at load) | C |
| F16 | **[v2]** Frame loop `0x577e50`: clock vfunc +0x30 → `0x4a529c` (xellSleep `0x4a538c` + Reflex markers) → task graph. Frame gen shows 2 presents per sim tick, so time must be measured at sim ticks, never presents | C / L |

## 2. Approach **[v2: two patch points]**

One real-time factor per simulation frame:
`r = clampNaN(dtReal · fps, 0.5, 2.0)`, with `fps = [clock+0x618]` (this tick's value, read at clock entry), and `r = 1.0` in passthrough.

- **P1 (game time)** at `0x5ad6a2`: `[+0x624] = [+0x628] · r`. Covers all speed-scaled consumers, the legacy 60 Hz ticks and the per-tick dt in the param struct. Hit-stop/slow-mo preserved because we multiply; we never write `+0x628`.
- **P2 (camera + main update)** at `0x39e526`: `dt = (60.0 / S.fps) · r`. The camera and the ~25 systems fed by `0x39e4e4` advance by real time. Speed stays excluded there (original design), and the pause path that branches around P2 is untouched.
- Both read the same latched `r`. The ordering of clock tick → `0x39e4e4` within a frame is verified in S1; if P2 runs first, it uses the r latched for its own frame (frame counter check).

`dtReal` source and smoothing (decided from S1 data, configurable):
- **A:** raw QPC delta between successive clock entries;
- **B:** raw delta between successive `xellSleep` returns (less scheduling noise; reviewer suggestion);
- optional N-frame average **with error feedback**: keep `debt = Σ(real dt) − Σ(r/fps)` and fold a bounded part of it into the next r, so game time never drifts from real time (fixes the plain-average drift).

Passthrough (`r = 1`): mod disabled / SafeMode, `fps(+0x618) != 120`, first 2 ticks after any fps change (history cleared), real dt > 250 ms (history cleared), startup checks failed, overlap detected (F2).

Rejected alternative: writing a fake integer "real fps" into `+0x614`. It would fix every reader at once, but code keys tables on exactly 30/60/120 (`0x5af604`, `0x947250` mode flag) and integer `120/fps` timers would break. Too unpredictable.

## 3. Delivery **[v2: all review fixes]**

- `dinput8.dll` proxy. The `.def` maps `DirectInput8Create=Proxy_DI8Create @1`, `@2..@6` for the rest, with the Dll* exports `PRIVATE`. Each stub lazily `LoadLibraryW(GetSystemDirectoryW()+"\\dinput8.dll")` on first call (never in DllMain) and returns `E_FAIL`/`DIERR_*` on failure. No .def forwarders.
- DllMain: pin the module (`GET_MODULE_HANDLE_EX_FLAG_PIN`), `CreateThread(init)`, nothing else. No C++ static constructors, `thread_local` or `std::thread`. Built with `-static`; `objdump -p` must show only KERNEL32 + `api-ms-win-crt-*` (+ user32/dbghelp if used).
- Init thread readiness (poll 50 ms, timeout 120 s, otherwise PASSIVE):
  1. `.text` is committed and executable (VirtualQuery);
  2. the AOB for GetFps matches exactly once; derive `G` from its rip-relative operand (no hardcoded global RVA);
  3. all AOBs (clock entry, P1, P2, xellSleep site) match exactly once, and P1/P2 bytes equal the expected decrypted bytes;
  4. `*G != 0`.
- `MH_Initialize` in the init thread. **All hooks via MinHook**, which freezes threads and fixes their IPs, so it is safe while workers run the code:
  - clock entry `0x5ad5f8` (prologue `40 55 53 56 57` = 5 B, relocatable): latch `fps=[rcx+0x618]`, compute r, overlap counter (interlocked; >1 → log + passthrough for this tick), frame counter;
  - P1 `0x5ad6a2`: first instruction is 8 B, not rip-relative. Detour stub: `movss xmm2,[rdi+0x628]; mulss xmm2,[r]; movss [rdi+0x624],xmm2; jmp qword [abs 0x5ad6b2]` (FF25 + 8-byte address). The MinHook trampoline is unused (the detour fully replaces the 2 instructions);
  - P2 `0x39e526`: first instruction `movss xmm7,[rip+…]` is rip-relative (MinHook relocates it, but the stub doesn't use the trampoline). Detour stub: `movss xmm7,[c60]; cvtsi2ss xmm0,rax; divss xmm7,xmm0; mulss xmm7,[r]; jmp qword [abs 0x39e537]`. Uses only xmm7/xmm0, which the original also writes; eax was loaded at 0x39e520.
  - `xellSleep` (libxell export) or its call site `0x4a538c`: timestamp only (option B).
- Stub memory: one allocation of 2 adjacent pages near the exe. Page 1 = stubs (written, then flipped to RX); page 2 = RW data (`r`, `c60`, frame counter). All data access is rip-relative within the allocation, and all returns are absolute FF25. Register unwind info for the stubs with `RtlAddFunctionTable` (leaf functions, no stack use).
- `r` is a single aligned float written once per tick by the clock-entry hook before the original runs; readers see either the old or the new value, never a torn one.
- `nioh3fps.ini`: `Enabled`, `StartEnabled`, `SafeMode`, `ToggleKey=F10`, `DumpKey=Ctrl+F10`, `TimeSource=Clock|Xell`, `AvgFrames`, `ScaleMin/Max`, `LogLevel`. Exe never modified; uninstall = delete files.

## 4. Stages and gates

**S0 Prep.** Locate and back up the Nioh 3 saves (not under Documents\KoeiTecmo). Play offline until S5. **[v2]** Capture an n3mon run with F9 markers: uncapped ~80 fps segment, hit-stop, pause, cutscene exit. This backs F12 and shows whether `+0x628` ever changes (hit-stop) — needed before relying on either.

**S0.5 Static completion [v2].** Before writing probe code:
- list readers of `S+0x10` and every consumer of the `0x39e4e4` dt (the ~25 systems) → classify;
- classify the 56 inline `+0x614` reads (F7b), the raw speed readers (F7c), `0x46398`, the F7d field consumer, and the purpose of the `0x5adc64` copy (F7e);
- produce the function list the probe will count.

**S1 Probe build (no behaviour change; same DLL, P1/P2 detours run the original math).** Logs:
- per tick: clock-entry QPC, xellSleep QPC, thread ID, fps(+0x618/+0x614), speed req/eff, overlap counter, frame counter;
- ordering: clock tick vs `0x39e4e4` vs camera update `0x567070` per frame;
- per-second call-count histograms (lock-free) for GetFps/GetTimeStep return addresses and for the S0.5 function list (entry hooks).
F9-style markers. All through the section-6 logging.

**Gate G1 (camera) [v2]:** answered statically: the camera uses the unscaled main-update dt, so P2 is required. S1 must confirm at runtime that `0x567070` runs once per frame with the P2 dt.
**Gate G1b [v2]:** S1 shows no clock-fn overlap across a 30-min session, P2 ordering is understood, and every hot unscaled site from S0.5 + S1 is classified. S3 cannot start until G1b passes.

**S2 Per-site decisions → plan v3 → independent review.** For each hot unscaled site: (a) leave (UI, effects that are fine per-frame, F8b cut-reset); (b) scale by r (patch); (c) frame timers gating gameplay: convert by r or accept, with measured worst-case error at 80 fps. Raw speed readers (F7c): decide whether they need the *unscaled* requested speed (e.g. `==1.0` checks), and if so redirect them to `+0x628`.

**S3 Fix build.** P1 + P2 + S2 decisions; toggle default off.

**S4 Test matrix (offline; logging + markers).**
1. Locked 120: r ≡ 1.0, identical behaviour.
2. Uncapped 80–120: game-time accuracy (Σ r/fps vs real time) within ±0.5% over 10+ minutes; camera pan smooth by eye with F10 A/B.
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
| Some of the ~25 systems fed by the P2 dt must stay per-frame | S0.5 classification; P2 could be narrowed to the camera task only (scale dt inside `0x8bee50`'s task instead of at P2) |
| Unscaled GetFps/inline sites (AI, audio, input) drift at < 120 fps | S0.5/S1/S2 per-site decisions |
| Raw speed readers misfire on speed ≈ 1.0 ± ε | F7c redirect to `+0x628` where needed |
| Physics instability | clamp 2.0, hitch reset, test 9 |
| Clock overlaps across workers | interlocked counter → passthrough + log; G1b |
| Game update | exact-once AOBs, byte checks, G derived from AOB, otherwise PASSIVE |
| Co-op desync | S5, kill switch |
| Drift between game time and real time | error-feedback averaging, test 2 |
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

**Flight recorder:** ~30 s lock-free ring buffer of per-tick records (both timestamps, fps, speeds, r, flags, thread ID, toggle, frame counter). Dumped to `nioh3fps_trace_<time>.csv` on Ctrl+F10, on crash, and on any internal assertion (overlap, unexpected ordering). `LogLevel=Debug` enables the S1 call-site histograms in release builds.

**Crash capture:** chain `SetUnhandledExceptionFilter` (call the game's previous filter). A vectored handler acts only when the faulting IP is in our DLL or stub pages. On a crash: minidump (`MiniDumpWriteDump`) + flight-recorder trace + faulting module+offset in the log. Release builds are stripped; an unstripped twin + `.map` is archived per build ID.

**User kit:** `Collect-Nioh3FpsLogs.bat` (log, `.old`, ini, traces, dumps, folder listing → `Desktop\nioh3fps_report_<time>.zip`); documented ini with `SafeMode`; README (install, uninstall, keys, known issues, co-op status, how to report); MinHook BSD-2 licence.

**Compatibility policy:** unknown bytes → PASSIVE with a clear status, never a guess. Supporting a game patch = re-run the AOB scan script and bump the version.
