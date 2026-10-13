# Review round 5: issues raised against plan v5 (fixed in v6)
MAJOR1 Smooth: after a sustained dip the history is full of 2/fps values, so recovery overshoots (r 2.0→0.82 while dt is steady 8.3 ms; up to 18.6 ms ahead, 1.9 ms cancelled). Fix: dt >= ScaleMax/fps is a hitch (clear, debt 0, r=ScaleMax).
MINOR: overlap rule contradicted the pseudo-code, so check overlap first and change nothing; hardcoded 0.5/2.0, so use ScaleMin/Max and bound AvgFrames/SmoothK; pin jmp rax = FF E0; stale text; SafeMode undefined.
Verified OK: Raw zero error outside clamps; all paths consistent; unwind v5 simulated at every stub offset (Windows and Wine rules) returns the host's caller with correct RSP and saved registers (review5/uw5.py); no crash or save risk.
