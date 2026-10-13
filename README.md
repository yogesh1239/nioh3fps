# nioh3fps

**Smooth gameplay in Nioh 3 at any frame rate below 120 fps.**

Nioh 3 runs its game logic at a fixed 120 updates per second. If your PC renders fewer than 120 frames per second, some frames get one update and others get two. You see this as small, uneven hitches, mostly when you turn the camera. It happens even at a steady 90 fps.

nioh3fps fixes this. It makes the game run exactly one update per frame you see, sized to the time that actually passed. The game keeps its normal speed, and the camera pans smoothly. At 120 fps and above, nothing changes.

---

## Before and after

Measured while panning the camera at a shrine, at about 94 fps:

| | Without the mod | With the mod |
|-|-|-|
| Frames with exactly one update | 72% | **100%** |
| Average motion error per frame | 3.4 ms | **0.05 ms** |
| Worst 5% of frames | 6.3 ms | **0.33 ms** |
| Frame rate | 93.7 fps | 94.4 fps |
| Game speed vs real time | 1.0000 | 0.99999 |

The frame rate stays the same. What changes is that every frame now moves the right amount.

---

## Installing

1. Download **`nioh3fps-<version>.zip`** from the [Releases](../../releases) page.
2. Open your Nioh 3 folder. In Steam, right-click **Nioh 3 → Manage → Browse local files**.
3. Copy **`dinput8.dll`** and **`nioh3fps.ini`** into that folder, next to `Nioh3.exe`.
4. Start the game.

To check that it's running, open `nioh3fps.log` in the same folder and look for the line `STATUS: ACTIVE`.

**To uninstall,** delete `dinput8.dll` and `nioh3fps.ini`. The mod never changes any game files.

---

## Controls

| Key | What it does |
|-|-|
| **F10** | Turns the fix on or off, so you can compare |
| **F9** | Writes a marker into the log. Press it when you notice a problem |
| **Ctrl+F10** | Saves the recent timing data as a CSV file, for bug reports |

You can change these keys, along with every other setting, in `nioh3fps.ini`. Each setting has a short comment explaining it.

---

## Is it safe?

- **Online and co-op:** the fix switches itself off as soon as the game reports an online or co-op session. It comes back on 2 seconds after you return to single player. While it is off, the game runs exactly as it would without the mod.
- **Game updates:** the mod looks for the code it needs by pattern, not by fixed address. If a patch changes something it relies on, the mod steps aside and the game runs normally. The log tells you what was missing.
- **Built-in fail-safe:** the mod never waits on the renderer for more than 50 ms. If that keeps happening, for example while you change graphics settings, it switches off for a moment and then recovers by itself.
- **Menus, pauses, cutscenes and loading screens** are left alone. The fix only runs during normal gameplay.

---

## Things to know

**Some systems may run at a slightly different pace.**
A few game systems count updates instead of measuring time. Once updates follow your frame rate, those systems need adjusting. The mod already handles every one found in the code: character movement and timers, held-button timers, turning speed, and the time-of-day clock. Simple counters that never look at the frame rate can't be found that way. If something feels faster or slower with the fix on, such as stamina or Ki recovery, buffs or hit-stop, compare it with F10 and please open an issue.

**Frame generation works, but some jitter may remain.**
Generated frames aren't counted, so the game updates once per real frame. That is the correct behaviour. With XeSS Frame Generation (through OptiScaler), the mod still removes most of the stutter: the worst-5% motion error drops from 6.8 ms to 1.5 ms. Without frame generation it drops to 0.6 ms, so motion is smoother still. DLSS Frame Generation hasn't been tested yet. **For the smoothest result, play with frame generation off.**

**Co-op hasn't been tested live yet.**
The online switch-off uses the game's own online flag, which was found by reading the code. It hasn't been confirmed in a real co-op session.

**Physics** already follows real time, so below 120 fps it takes slightly larger steps. You shouldn't notice this.

---

## Reporting a bug

1. If you can, press **F9** at the moment you notice the problem.
2. Run **`tools/Collect-Nioh3FpsLogs.bat`**. It zips up the log and timing files.
3. Open an issue, attach the zip, and describe what you saw and when.

---

## Building from source

You'll need MinGW-w64 (`gcc` and `mingw32-make`) and Python 3.

```
mingw32-make            # debug build   -> build/
mingw32-make release    # release build -> build/release/
mingw32-make test       # unit tests
```

Some tests check the patch patterns against the game executable. Point `EXE=` at your copy of `Nioh3.exe`. The tests only read the file and never run the game. The analysis helpers in `tools/` need `pip install pefile capstone numpy`.

---

## How it works

<details>
<summary>Technical overview</summary>

- **Render heartbeat:** a hook at the point where the render thread picks up a finished game update records each pickup.
- **Update gate:** before each update starts, the update thread waits until the renderer has picked up the previous one. The wait is bounded, and the thread helps with the game's job queue while it waits. This keeps updates and frames in a strict one-to-one rhythm.
- **Real-time scaling:** the game clock and camera step are scaled by *real elapsed time ÷ 1/120 s*, so the game speed stays exact.
- **Consumer fixes:** game code that assumes a fixed 120 updates per second gets the actual rate instead. Character sub-steps that would otherwise be scaled twice get the normal step.

Design documents, reverse-engineering notes and measurements are in [`docs/`](docs). The latest plans are `docs/PLAN_v8_lockstep.md` and `docs/PLAN_v9_consumers.md`. The notes are in `docs/re/`.

</details>

---

## Credits and licence

- nioh3fps is released under the MIT licence. See [LICENSE](LICENSE).
- It uses [MinHook](https://github.com/TsudaKageyu/minhook) (BSD 2-Clause, in `third_party/minhook`).
- This is an unofficial fan project. It is not affiliated with or endorsed by Koei Tecmo or Team NINJA. Use it at your own risk.
