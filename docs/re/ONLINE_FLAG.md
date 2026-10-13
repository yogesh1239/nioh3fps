# Online / co-op session predicate (static RE, 2026-10-13)

Static RE on `Nioh3.exe.unpacked.exe` (RVAs). Release blocker from PLAN_v9 section 3: the lockstep gate must stay disarmed in any network session.

## Result

**Predicate: `g = [0x4709b50]`. The session is active if `g != 0 && dword [g+0x34] != 0`.**

| value of `[g+0x34]` | meaning | conf |
|-|-|-|
| 0 | no online play: solo, offline, title, NPC-only co-op | med-high |
| 1 | online play (`0x85930` "IsHost"-type getter, `== 1`) | med-high |
| 2 | online play (`0x85948`, `== 2`; also backs script `Online::IsTokoyoCoopMode`) | med |

The disarm rule is `!= 0`, not `== 1 || == 2`. Any value written in the future is treated as online.

`g == 0` (before the online subsystem is registered) counts as no session. The field is an aligned dword with plain stores, so a racy read from H1 is fine.

## Evidence

- **The game's own predicate.** The script function `Online::IsOnlinePlay` (name `0x3bf7680`, handler `0xa5e83c` / `0x23a68ac`) calls `0x85834`, and so does `Online::IsPlayerCoop` (`0x23e480c`). `0x85834` returns `g && ([g+0x34] == 1 || [g+0x34] == 2)`.
  - 116 direct callers in the engine use `0x85834`.
  - 154 callers use the `== 2` getter `0x85948` and 50 use the `== 1` getter `0x85930`.
  - `Online::CloseOnlineMode` tests `0x85834` before tearing down.
  - Conf: high that this is the game's "online play" flag.
- **Writers of `+0x34`, all through `[0x4709b50]`:**

| writer | value | context | conf |
|-|-|-|-|
| `0x1d785d0`, `0x1d9ffa4` | 1 | session establish: they call `0x1de3d58` (session reset/create), store the session descriptor, then call `0x1c22afc` | med-high |
| `0x1d788bc`, `0x1da0008` | 2 | join/connect path: they call `0x1de3e04` and set `+0x824 = 1` | med-high |
| `0x21de9b0`, `0x21dea28` | 0 | session teardown: called through thunks `0x21de990`/`0x21deabc`; they also reset per-player slots (`0x1ca8db0` ×2, `0x1ca8cc4` ×3) | med-high |

  - Caller `0x1e077cc` reaches the `=1` writer only when the session object `[[0x4759910]+0x530]` reports ready (`vfunc+0x48` true and `+0x974 == 1`).
- **Player-count scaling.** `0x85814` returns 0 offline, else `([g+0x34] == 2) + 1`. It feeds the multiplayer scaling table at `0x85790`: 1.0 when offline or when the player count `[[0x45c84f8]+0x2c4]` ≤ 1. This shows `+0x34` is the co-op mode the gameplay balancing keys on. Conf: med.
- **PvP/invasions.** `Online::IsOnlinePlayVersus` (`0x23e46a8`) returns a constant 0 in this build, so no versus or invasion mode exists. Visitors ("Marebito") and the Tokoyo co-op mode are co-op variants and set `+0x34`. Conf: med-high.
- **Semantics of 1 vs 2 are not settled.** Two readings fit: host/guest, or mission co-op versus Tokoyo co-op. Script `Online::IsTokoyoCoopMode` maps to `== 2`. The gate does not depend on which reading is right.

## Resolving it (update-proof)

Primary AOB, the whole `IsOnlinePlay` function (`0x85834`, 1 match):
```
48 83 EC 28 48 8B 05 ?? ?? ?? ?? 33 C9 48 85 C0 74 06 83 78 34 01 74 09 E8 ?? ?? ?? ?? 84 C0 74 02 B1 01 8A C1 48 83 C4 28 C3
```
- Global = `match + 4 + 7 + i32[match + 7]` (the `mov rax,[rip+disp]` at `+4`) = `0x4709b50`.
- Field offset = byte at `match + 0x14` (`83 78 34 01`) = `0x34`.
- Cross-check: the `E8` at `+0x18` targets `0x85948`.

Cross-checks; each must decode to the same global and offset:
- `0x85948`: `48 8B 05 ?? ?? ?? ?? 48 85 C0 74 08 83 78 34 02 0F 94 C0 C3` (1 match). Global at `+0`, offset at `+0xE`.
- `0x85930`: `48 8B 05 ?? ?? ?? ?? 48 85 C0 74 08 83 78 34 01 0F 94 C0 C3` (1 match).

Failure policy: if the primary AOB does not match exactly once, or the cross-checks disagree, the mod cannot prove single-player. It should stay passive (native 120 Hz), log `STATUS: PASSIVE online-flag unresolved`, and never arm.

## Read path and gate rule

```
g = *(uint64*)(base + 0x4709b50)        // resolved via AOB
online = g && *(volatile uint32*)(g + 0x34) != 0
```
- Read once per H1 tick, before arming. If `online` is true, take the native path for the whole tick and log `GATE online` on each transition.
- Hysteresis: re-arm only after `online` has been false for the same history window the gate already uses after an ON edge (about 2 s). This keeps a session that is still closing from flapping the gate.
- Expected single-player: `g` non-null after boot, `[g+0x34] = 0` at all times. This includes online-mode solo play with graves and ghosts, which uses no lockstep peer (inferred: no writer of `+0x34` sits outside the session-establish paths; conf med).
- Expected in a co-op, visitor or Tokoyo co-op session: 1 or 2 from session establish until teardown.
- Conservative side: the host may read 1 from session establish, before a guest arrives. That only disarms early, which is safe.

## Offline/online mode toggle

The connectivity or "online mode" state is **not** established as a separate readable flag (conf: low). The KTGL online layer (`COnlineModule`, `KTGL_ONLINE_E_DEVICE_*`, Steam via `SteamInternal_FindOrCreateUserInterface` with `SteamMatchMaking009`, `SteamNetworkingSockets012` and `SteamNetworkingMessages002`) has device up/down state, but this pass found no single field for it.

It is not needed. Connected-but-solo play has no lockstep peer and leaves `+0x34 = 0`, so the gate can stay armed. What must disarm the gate is the co-op mode flag above.

Related flags that must not be used:
- `[g+0x30]` (`0x85800`) is 1 while in a mission/field and 0 elsewhere. It is not an online flag.
- `Coop::IsCooperating` (`0x860fe0` → `0x286dec`/`0x861088` on `[0x4751848]`) is NPC/AI co-op, which runs offline.

## Live verification still needed

The game was not launched. A co-op session needs two accounts, so it is user-assisted.

1. Log `[g+0x34]` transitions with their RVA source:
   - solo offline: expect 0 throughout
   - solo with online on: expect 0
   - summon a visitor / join as visitor: expect 1 or 2 when the session is created, 0 after leaving
2. Confirm the gate shows `GATE online` and the session runs native 120/s ticks.
