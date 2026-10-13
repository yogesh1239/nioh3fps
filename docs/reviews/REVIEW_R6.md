# Review round 6: issues raised against plan v6 (fixed in v7)
MAJOR M1 Smooth: overshoot persists for dips just above 60 fps (120→65→120: 11.7 ms behind, r 1.59…0.83; N=32: 45–53 ms), and the debt clamp silently drops time. Fix (tested, review6/sim6c.py): given = clamp(given, owed-1/fps, owed+1/fps); debt = owed - r/fps; cap only beyond ScaleMin/Max, logged as dropped.
MINOR: ScaleMax=1 makes every frame a hitch, so make it a plain clamp; state ini defaults; the "not repaid" wording applies to Raw only; the P2 trampoline is unused.
Verified OK: all R1–R5 closed or gated; Raw error-free; r in range and never NaN across all scenarios and edge configs; overlap path changes no state; P1/P2 bytes re-checked; no crash or save risk.
