# Review round 4: issues raised against plan v4 (fixed in v5)
MAJOR1 unwind: host code offsets (2–19 for 0x5ad5f8, 12–37 for 0x39e4e4) mean codes get skipped at small stub offsets. Fix: zero CodeOffset in each code's first slot; copy 4+2*((Count+1)&~1) bytes, 4-byte aligned, Count unchanged.
MAJOR2 Smooth: dropping dt > 2/fps freezes the history during a sustained dip below 60, so r settles around 1.48 and 760 ms is lost. Fix: push min(dt, 2/fps).
MINOR: dt<=0 → reset, float division; hitch r=2 consistent with a spike; S4.2 must measure the game's own clock (0x5a6bb0 rate = 60/s); stale v1–v3 text; ScaleMax capped at 2.0; AOB at expected RVA else PASSIVE; overlap counter semantics; G1b H2 exactly once per H1 tick. Also: no written fallback if the second camera path F9d is live.
Sim results (review4/sim4.py): r in [0.5,2], no NaN; Raw error 0; Smooth error ±1.6 ms over 20k frames, std(r) 0.21→0.10 (jitter) and 0.25→0.03 (80/120 alternating); spike repaid over ~10 frames at +15%.
