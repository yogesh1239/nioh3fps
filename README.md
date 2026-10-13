# nioh3fps: smooth variable frame rate for Nioh 3

Nioh 3 runs its game simulation at a fixed 120 updates per second. When your PC renders fewer than 120 frames per second, each shown frame carries either one or two updates. The result is uneven motion, most visible as stutter when you pan the camera, even at a steady 90 fps.

nioh3fps is a small `dinput8.dll` mod that fixes this. It runs exactly one game update per rendered frame and scales each update by the real elapsed time, so the game keeps real-time speed. Above 120 fps nothing changes.

## Results

Measured at a shrine with a camera-only pan at about 94 fps:

| | Without mod | With mod |
|-|-|-|
| Frames carrying exactly one update | 72% | 100% |
| Motion error per frame, mean / 95th percentile | 3.4 / 6.3 ms | 0.05 / 0.33 ms |
| Frame rate | 93.7 fps | 94.4 fps |
| Game time vs real time | 1.0000 | 0.99999 |

## Install

1. Download `nioh3fps-<version>.zip` from [Releases](../../releases).
2. Extract `dinput8.dll` and `nioh3fps.ini` into the Nioh 3 folder, next to `Nioh3.exe`. In Steam: right-click Nioh 3 → Manage → Browse local files.
3. Start the game. `nioh3fps.log` appears in the same folder. A line `STATUS: ACTIVE` means the mod loaded.

**Uninstall:** delete `dinput8.dll` and `nioh3fps.ini`. The mod never modifies any game file on disk.

## Use

- **F10** turns the fix on and off while playing, so you can compare.
- **F9** writes a marker into the log. Press it the moment you notice a problem.
- **Ctrl+F10** saves the recent timing trace as a CSV file (useful for bug reports).

The keys and all other settings are in `nioh3fps.ini`, which has a comment for each one.

## Safety

- **Online play:** the fix switches itself off whenever the game reports an online or co-op session, and switches back on 2 s after the session ends. The game then runs exactly as unmodified.
- **Game updates:** every patch location is found by byte pattern, not by fixed address. If a game update changes something the fix depends on, the mod stays passive (the game runs unmodified) and the log names the missing piece.
- **Fail-safe:** the update thread never waits more than 50 ms for the renderer. Repeated timeouts switch the fix off for a while and log it.
- **Pause, menus, cutscenes, loading:** the fix is active only in normal gameplay.

## Known limitations

- **Systems the mod adjusts:** some game systems count updates instead of measuring time, which matters once updates follow the frame rate. The mod adjusts every one found in the code: character movement, character timers, input hold timers, turn rate and the time-of-day clock. Simple per-update counters that never read the frame rate cannot be found this way. If something feels faster or slower with the fix on (stamina or Ki regeneration, buffs, hit-stop), compare with F10 and please open an issue.
- **Co-op:** the automatic switch-off in co-op uses the game's own online-play flag, found by reading the code. It has not yet been confirmed in a live co-op session.
- **Physics:** physics already follows real time, so it runs a slightly larger step at frame rates below 120.
- **Frame generation:** generated frames are not counted, so the update rate follows real rendered frames.

## Bug reports

Run `tools/Collect-Nioh3FpsLogs.bat` (it zips the log and trace files) and attach the zip to an issue. Please say what you saw and when. A marker (F9) at that moment helps a lot.

## Building from source

The build needs MinGW-w64 (gcc, `mingw32-make`) and Python 3.

```
mingw32-make            # debug/test build -> build/
mingw32-make release    # release build    -> build/release/
mingw32-make test       # unit tests
```

Some tests check patch patterns and unwind data against the game executable. Point `EXE=` at your local copy of the game executable; the tests never run the game. The static-analysis helpers in `tools/` need `pip install pefile capstone numpy`.

## How it works

- **Render heartbeat:** a hook at the point where the render thread picks up a new game update records each pickup.
- **Update gate:** at the start of each update, the update thread waits (bounded, and helping run the game's job queue while it waits) until the renderer has picked up the previous update. Updates and frames therefore alternate one to one.
- **Real-time scaling:** the game's clock and camera step are scaled by real elapsed time ÷ 1/120 s, so game speed stays exact.
- **Consumer fixes:** game code that derives timing from the fixed 120 rate receives the effective rate instead. Character sub-steps that would otherwise be scaled twice get the native step.

The design documents, reverse-engineering notes and measurement results are in [`docs/`](docs). The latest plans are `docs/PLAN_v8_lockstep.md` and `docs/PLAN_v9_consumers.md`, and the notes are in `docs/re/`.

## Credits and licence

- nioh3fps is MIT-licensed (see [LICENSE](LICENSE)).
- It uses [MinHook](https://github.com/TsudaKageyu/minhook) (BSD 2-Clause, `third_party/minhook`).
- This is an unofficial fan mod, not affiliated with or endorsed by Koei Tecmo or Team NINJA. Use it at your own risk.
