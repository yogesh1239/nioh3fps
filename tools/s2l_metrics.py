#!/usr/bin/env python3
"""Measure PLAN_v8_lockstep.md M1-M6 from per-event ticks CSV (stdlib only).

Run: python -I tools/s2l_metrics.py ticks.csv [--from T0] [--to T1] [--log mod.log]
Times use the CSV's t_s clock. Intervals are (previous beat, current beat], as
in src/writer.c. Only complete intervals within a single A/B span are measured;
M2 additionally requires the preceding interval in that same span. p95 uses
nearest rank. Beat attribution uses integer QPC, because HB can be drained late
and t_s is rounded; older CSVs without QPC fall back to t_s.
M3 counts beats over each state's entire real span, including quiet tails.

TOGGLE records define the requested A/B state; absent those, H1.enabled changes
define effective-state spans. Other records' enabled fields are NOT toggle state
(notably GATE_END.enabled means session_off). H1 is the P1 sample, not H2, and
speed_eff is sampled BEFORE P1: step = speed_req * r_applied / fps_a seconds.
Missing speed_req means unit speed. HB repeats remain beats; hb_other is excluded.

Clamp counters in CSV and H1/P1 log summaries are cumulative, never per-row
losses. M5 shows both game/real and game/(real - lost + gained). Legacy 0x5a6bb0
calls are informational: it runs several times per update. Log summaries are
used only when their entire windows fit a selected span, without prorating.
Log time is aligned through matching TOGGLE records, otherwise the initial
flight-recorder line (approximate to log timestamp precision). Missing evidence
is n/a; a short recording cannot certify a five-minute sweep or ten toggles.
"""

import argparse
from bisect import bisect_left, bisect_right
from collections import Counter
import csv
from dataclasses import dataclass
import math
from pathlib import Path
import re
import statistics
import sys


def number(value):
    try:
        result = float(value)
        return result if math.isfinite(result) else None
    except (TypeError, ValueError):
        return None


def integer(value):
    try:
        return int(value, 16 if str(value).lower().startswith("0x") else 10)
    except (TypeError, ValueError):
        return None


def bit(row, field, mask):
    value = integer(row.get(field))
    return bool(value) if value is not None else bool((integer(row.get("flags")) or 0) & mask)


@dataclass
class Event:
    t: float
    seq: int
    kind: str
    enabled: int = None
    step: float = None
    lost: float = None
    gained: float = None
    backoffs: int = None
    wait: float = None
    timeout: bool = None
    repeat: bool = False
    other: bool = False
    qpc: int = None


def read_csv(path):
    events, bad = [], 0
    kinds = {"H1", "HB", "XELL", "TOGGLE", "GATE_END"}
    with path.open(encoding="utf-8-sig", newline="") as stream:
        reader = csv.DictReader(line for line in stream if line.strip() and not line.lstrip().startswith("#"))
        if not reader.fieldnames or not {"t_s", "type"}.issubset(reader.fieldnames):
            raise ValueError("CSV needs t_s and type columns")
        for row in reader:
            t = number(row.get("t_s"))
            if t is None or None in row:
                bad += 1
                continue
            kind = row["type"].strip().upper()
            # Keep unrelated events for recording bounds, but no payload.
            e = Event(t, integer(row.get("seq")) or len(events), kind)
            e.qpc = integer(row.get("qpc"))
            if kind in kinds:
                state = integer(row.get("enabled"))
                e.enabled = state if state in (0, 1) else None
            if kind == "TOGGLE" and e.enabled is None:
                state = integer(row.get("aux"))
                e.enabled = state if state in (0, 1) else None
            if kind == "H1":
                fps, r = number(row.get("fps_a")), number(row.get("r_applied"))
                speed = number(row.get("speed_req")) if row.get("speed_req") else 1.0
                if fps is not None and fps > 0 and r is not None and r >= 0 and speed is not None and speed >= 0:
                    step = speed * r / fps
                    e.step = step if math.isfinite(step) else None
                e.lost, e.gained = number(row.get("time_lost_ms")), number(row.get("time_gained_ms"))
            elif kind == "HB":
                e.other, e.repeat = bit(row, "hb_other", 1), bit(row, "hb_repeat", 2)
            elif kind == "GATE_END":
                wait = number(row.get("gate_wait_us"))
                e.wait = wait / 1000 if wait is not None else number(row.get("dt_ms"))
                if e.wait is not None and e.wait < 0:
                    e.wait = None
                e.timeout = bit(row, "gate_timeout", 1) if row.get("gate_timeout") or row.get("flags") else None
                e.backoffs = integer(row.get("aux2"))
            events.append(e)
    events.sort(key=lambda e: (e.t, e.qpc if e.qpc is not None else e.seq))
    return events, bad


@dataclass
class LogWindow:
    start: float
    end: float
    kind: str
    values: tuple


def read_log(path, events):
    if path is None:
        return [], [], None, "not supplied"
    lines, day, previous = [], 0, None
    clock = re.compile(r"^(\d{2}):(\d{2}):(\d{2}\.\d+)\s+(.*)")
    with path.open(encoding="utf-8-sig", errors="replace") as stream:
        for line in stream:
            match = clock.match(line)
            if not match:
                continue
            hh, mm, ss, message = match.groups()
            local = int(hh) * 3600 + int(mm) * 60 + float(ss)
            if previous is not None and previous - local > 43200:
                day += 86400
            previous = local
            lines.append((day + local, message))
    csv_toggles = [e for e in events if e.kind == "TOGGLE" and e.enabled is not None]
    log_toggles = [(t, int(m.group(1) == "ON")) for t, s in lines
                   if (m := re.search(r"TOGGLE: (ON|OFF)\b", s))]
    origin = None
    alignment = "unaligned"
    if csv_toggles and len(csv_toggles) == len(log_toggles) and all(e.enabled == v for e, (_, v) in zip(csv_toggles, log_toggles)):
        offsets = [t - e.t for e, (t, _) in zip(csv_toggles, log_toggles)]
        origin = statistics.median(offsets)
        if max(offsets) - min(offsets) > 0.1:
            origin = None
        else:
            alignment = "TOGGLE timestamps"
    if origin is None:
        origin = next((t for t, s in lines if s.startswith("flight recorder:")), None)
        if origin is not None:
            alignment = "flight-recorder timestamp (approximate)"
    initial = next((int(m.group(1)) for _, s in lines
                    if (m := re.search(r"\bStartEnabled=([01])\b", s))), None)
    if origin is None:
        return [], [], initial, alignment
    windows, backoffs = [], []
    previous_loss = (0.0, 0.0)
    for t, text in lines:
        t -= origin
        if "GATE BACKOFF:" in text:
            backoffs.append(t)
        match = re.search(r"GATE ([\d.]+)s:.*?waits=(\d+) mean/max wait=([\d.]+)/([\d.]+)ms timeouts=(\d+) backoffs=(\d+)", text)
        if match:
            secs, waits, mean, maximum, timeouts, count = map(float, match.groups())
            windows.append(LogWindow(t - secs, t, "gate", (int(waits), mean, maximum, int(timeouts), int(count))))
        match = re.search(r"PROBE ([\d.]+)s: legacy tick .*? = (\d+) \(", text)
        if match:
            secs, count = map(float, match.groups())
            windows.append(LogWindow(t - secs, t, "legacy", (count,)))
        match = re.search(r"SUMMARY ([\d.]+)s H1/P1:.*?time_lost=([\d.]+)ms time_gained=([\d.]+)ms", text)
        if match:
            secs, lost, gained = map(float, match.groups())
            delta = tuple(v - old if v >= old else v for v, old in zip((lost, gained), previous_loss))
            windows.append(LogWindow(t - secs, t, "loss", delta))
            previous_loss = lost, gained
    return windows, backoffs, initial, alignment


@dataclass
class Span:
    start: float
    end: float
    state: int


def split_spans(events, low, high, initial=None):
    toggles = [e for e in events if e.kind == "TOGGLE" and e.enabled is not None]
    if toggles:
        # A real toggle flips the prior requested state, even if L3 was unarmed.
        state = initial if initial is not None else 1 - toggles[0].enabled
        changes = toggles
        source = "TOGGLE records"
    else:
        samples = [e for e in events if e.kind == "H1" and e.enabled is not None]
        state = samples[0].enabled if samples else initial
        changes, last = [], state
        for e in samples:
            if e.enabled != last:
                changes.append(e)
                last = e.enabled
        toggles = changes
        source = "H1.enabled (transitions inferred)"
    spans, start = [], low
    for e in changes:
        if e.t <= low:
            state = e.enabled
        elif e.t < high:
            spans.append(Span(start, e.t, state))
            start, state = e.t, e.enabled
    if high >= start:
        spans.append(Span(start, high, state))
    return spans, [e for e in toggles if low <= e.t <= high], source


def selected(events, spans):
    times = [e.t for e in events]
    return [e for s in spans for e in events[bisect_right(times, s.start):bisect_right(times, s.end)]]


def contained(windows, spans, kind):
    return [w for w in windows if w.kind == kind and w.end > w.start
            and any(s.start <= w.start and w.end <= s.end for s in spans)]


def stats(values):
    if not values:
        return None
    ordered = sorted(values)
    return statistics.fmean(values), ordered[math.ceil(0.95 * len(ordered)) - 1], ordered[-1]


def fmt(value, digits=3):
    return "n/a" if value is None else f"{value:.{digits}f}"


def intervals(beats, ticks, spans, have_h1):
    use_qpc = all(e.qpc is not None for e in beats) and all(e.qpc is not None for e in ticks)
    clock = (lambda e: e.qpc) if use_qpc else (lambda e: e.t)
    beats = sorted(beats, key=clock)
    bt, bq = [e.t for e in beats], [clock(e) for e in beats]
    counts, advances, invalid = [0] * len(beats), [0.0] * len(beats), [0] * len(beats)
    for e in ticks:
        # (previous beat, current beat]: seq is write order, not event time.
        end = bisect_left(bq, clock(e))
        if 0 < end < len(beats):
            counts[end] += 1
            advances[end] += e.step or 0.0
            invalid[end] += e.step is None
    hist, lagged, floor, unlagged = Counter(), [], [], []
    for s in spans:
        start, stop = bisect_left(bt, s.start), bisect_right(bt, s.end)
        previous_dt = None
        for end in range(start + 1, stop):
            dt = bt[end] - bt[end - 1]
            if dt <= 0:
                previous_dt = None
                continue
            if previous_dt is not None:
                floor.append(abs(dt - previous_dt) * 1000)
            if have_h1:
                hist[counts[end]] += 1
                if not invalid[end]:
                    advance = advances[end]
                    unlagged.append(abs(advance - dt) * 1000)
                    if previous_dt is not None:
                        lagged.append(abs(advance - previous_dt) * 1000)
            previous_dt = dt
    duration = sum(s.end - s.start for s in spans)
    count = sum(bisect_right(bt, s.end) - bisect_right(bt, s.start) for s in spans)
    rate = count / duration if duration > 0 and beats else None
    return hist, lagged, floor, unlagged, rate


def print_intervals(label, result):
    hist, lagged, floor, unlagged, rate = result
    if hist:
        percent = 100 * hist[1] / sum(hist.values())
        verdict = "PASS" if percent >= 98 else "FAIL"
        print(f"  M1 {label}: histogram {{{', '.join(f'{k}: {v}' for k, v in sorted(hist.items()))}}}; exactly-1={percent:.2f}% {verdict} (>=98%; n={sum(hist.values())})")
    else:
        print(f"  M1 {label}: n/a")
    actual, base = stats(lagged), stats(floor)
    if actual and base:
        limits = base[0] + 0.2, base[1] + 0.5
        verdict = "PASS" if actual[0] <= limits[0] + 1e-9 and actual[1] <= limits[1] + 1e-9 else "FAIL"
        print(f"  M2 {label} lagged: mean={actual[0]:.3f} ms p95={actual[1]:.3f} ms; floor mean={base[0]:.3f} ms p95={base[1]:.3f} ms; limits={limits[0]:.3f}/{limits[1]:.3f} ms {verdict} (n={len(lagged)})")
    else:
        print(f"  M2 {label} lagged: n/a; floor: " + (f"mean={base[0]:.3f} ms p95={base[1]:.3f} ms" if base else "n/a"))
    comparison = stats(unlagged)
    print(f"  M2 {label} unlagged: " + (f"mean={comparison[0]:.3f} ms p95={comparison[1]:.3f} ms (S1 reference 3.40/6.15 ms; n={len(unlagged)})" if comparison else "n/a"))
    print(f"  M3 {label}: {fmt(rate)} presents/s")


def counter_deltas(events, attribute):
    result, previous = {}, None
    for e in events:
        value = getattr(e, attribute)
        # Positive initial cumulative values predate a possibly rotated CSV.
        result[e.seq] = (None if value is None or (previous is None and value != 0)
                         else value - previous if previous is not None and value >= previous else value)
        previous = value
    return result


def sum_known(values):
    values = list(values)
    return sum(values) if values and all(v is not None for v in values) else None


def print_gate(gates, spans, windows, log_backoffs, deltas):
    samples = selected(gates, spans)
    logs = contained(windows, spans, "gate")
    timeouts = sum_known(e.timeout for e in samples)
    backoffs = sum_known(deltas[e.seq] for e in samples)
    source = "CSV" if samples else "n/a"
    wait_stats = stats([e.wait for e in samples if e.wait is not None])
    mean, p95, maximum = wait_stats or (None, None, None)
    if logs:
        if timeouts is None:
            timeouts = sum(w.values[3] for w in logs)
            source = "log complete windows"
        if backoffs is None:
            backoffs = sum(w.values[4] for w in logs)
        if not samples:
            count = sum(w.values[0] for w in logs)
            if count:
                mean = sum(w.values[0] * w.values[1] for w in logs) / count
                maximum = max(w.values[2] for w in logs if w.values[0])
    direct = [t for t in log_backoffs if any(s.start < t <= s.end for s in spans)]
    if backoffs is None and direct:
        backoffs = len(direct)
    coverage = sum(w.end - w.start for w in logs)
    print(f"  M4: timeouts={fmt(timeouts, 0)} backoffs={fmt(backoffs, 0)} ({source}); wait mean/p95/max={fmt(mean)}/{fmt(p95)}/{fmt(maximum)} ms (CSV waits={len(samples)}, log windows={len(logs)}, log coverage={coverage:.3f} s)")


def print_game(ticks, spans, windows, lost_deltas, gained_deltas):
    samples = selected(ticks, spans)
    duration = sum(s.end - s.start for s in spans)
    advance = sum_known(e.step for e in samples)
    lost = sum_known(lost_deltas[e.seq] for e in samples)
    gained = sum_known(gained_deltas[e.seq] for e in samples)
    source = "CSV cumulative deltas"
    if lost is None or gained is None:
        logs = contained(windows, spans, "loss")
        coverage = sum(w.end - w.start for w in logs)
        if logs and abs(coverage - duration) <= 0.002:
            lost = sum(w.values[0] for w in logs)
            gained = sum(w.values[1] for w in logs)
            source = "log cumulative deltas"
    raw = advance / duration if advance is not None and duration > 0 else None
    expected = duration - (lost - gained) / 1000 if lost is not None and gained is not None else None
    adjusted = advance / expected if advance is not None and expected is not None and expected > 0 else None
    verdict = "n/a" if adjusted is None else "PASS" if abs(adjusted - 1) <= 0.005 else "FAIL"
    print(f"  M5: game={fmt(advance, 6)} s real={duration:.6f} s; game/real={fmt(raw, 6)}; lost={fmt(lost)} ms gained={fmt(gained)} ms ({source}); game/(real-lost+gained)={fmt(adjusted, 6)} {verdict} (+/-0.5%)")
    legacy = contained(windows, spans, "legacy")
    secs = sum(w.end - w.start for w in legacy)
    rate = sum(w.values[0] for w in legacy) / secs if secs else None
    print(f"  M5 legacy: {fmt(rate)} ticks/s INFO (0x5a6bb0 runs several times per update; log coverage={secs:.3f} s)")


def toggle_gaps(toggles, ticks):
    times = [e.t for e in ticks]
    result = []
    for e in toggles:
        # Include whole gaps overlapping the window, so a long hang cannot hide
        # merely because its endpoint fell outside the one-second radius.
        a = max(0, bisect_left(times, e.t - 1) - 1)
        b = min(len(times), bisect_right(times, e.t + 1) + 1)
        gaps = [(right - left) * 1000 for left, right in zip(times[a:b], times[a + 1:b])
                if right >= e.t - 1 and left <= e.t + 1]
        result.append((e, max(gaps) if gaps else None))
    return result


def finite_time(value):
    result = number(value)
    if result is None:
        raise argparse.ArgumentTypeError("time must be finite seconds")
    return result


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("csv", type=Path)
    parser.add_argument("--from", dest="low", type=finite_time)
    parser.add_argument("--to", dest="high", type=finite_time)
    parser.add_argument("--log", type=Path)
    args = parser.parse_args(argv)
    if args.low is not None and args.high is not None and args.low >= args.high:
        parser.error("--from must be less than --to")
    try:
        events, bad = read_csv(args.csv)
        windows, log_backoffs, initial, alignment = read_log(args.log, events)
    except (OSError, ValueError, csv.Error) as exc:
        parser.exit(2, f"error: {exc}\n")
    print(f"S2L metrics: {args.csv}")
    if not events:
        print("M1-M6: n/a (no valid events)")
        return 0
    low = max(events[0].t, args.low) if args.low is not None else events[0].t
    high = min(events[-1].t, args.high) if args.high is not None else events[-1].t
    if high < low:
        print("M1-M6: n/a (window contains no events)")
        return 0
    spans, toggles, state_source = split_spans(events, low, high, initial)
    ticks = [e for e in events if e.kind == "H1"]
    gates = [e for e in events if e.kind == "GATE_END"]
    lost, gained = counter_deltas(ticks, "lost"), counter_deltas(ticks, "gained")
    backoffs = counter_deltas(gates, "backoffs")
    gaps = toggle_gaps(toggles, ticks)
    print(f"Window: {low:.6f}..{high:.6f} s; state: {state_source}; skipped malformed rows={bad}")
    print(f"Log alignment: {alignment}. p95: nearest rank; step=speed_req*r_applied/fps_a.")
    print("Complete beat intervals only; no intervals cross A/B boundaries. Five-minute sweep and ten-toggle coverage must be checked separately.")
    rates = {}
    signals = {"heartbeat": [e for e in events if e.kind == "HB" and not e.other],
               "XELL": [e for e in events if e.kind == "XELL"]}
    for state, name in ((1, "ON"), (0, "OFF"), (None, "unknown")):
        group = [s for s in spans if s.state == state]
        if state is None and not group:
            continue
        print(f"\nToggle-{name}: " + (", ".join(f"{s.start:.6f}..{s.end:.6f} s" for s in group) if group else "n/a (no span)"))
        if not group:
            continue
        for label, beats in signals.items():
            result = intervals(beats, ticks, group, bool(ticks))
            print_intervals(label, result)
            rates[state, label] = result[-1]
        primary = selected(signals["heartbeat"], group)
        other = selected([e for e in events if e.kind == "HB" and e.other], group)
        xell = selected(signals["XELL"], group)
        ratio = len(primary) / len(xell) if primary and xell else None
        repeats = sum(e.repeat for e in primary) if primary else None
        print(f"  HB diagnostics: primary={len(primary)} hb_other={len(other)} repeats={fmt(repeats, 0)} repeats/heartbeat={fmt(repeats / len(primary) if primary else None)} heartbeat:XELL={fmt(ratio)}")
        print_gate(gates, group, windows, log_backoffs, backoffs)
        print_game(ticks, group, windows, lost, gained)
        local = [(e, gap) for e, gap in gaps if e.enabled == state]
        print(f"  M6: toggles into {name}={len(local)}; max H1 gap within +/-1 s={fmt(max((g for _, g in local if g is not None), default=None))} ms")
    print("\nM3 ON vs OFF (100*(ON/OFF-1), within +/-3%):")
    for label in signals:
        on, off = rates.get((1, label)), rates.get((0, label))
        delta = 100 * (on / off - 1) if on is not None and off is not None and off > 0 else None
        verdict = "n/a" if delta is None else "PASS" if abs(delta) <= 3 else "FAIL"
        print(f"  {label}: " + (f"{delta:+.3f}% {verdict}" if delta is not None else "n/a"))
    toggle_count = len(toggles) if any(s.state is not None for s in spans) else None
    print(f"M6 total toggles={fmt(toggle_count, 0)} ({state_source}; target >=10)")
    for e, gap in gaps:
        verdict = "n/a" if gap is None else "PASS" if gap <= 50 else "FAIL"
        print(f"  t={e.t:.6f} s -> {'ON' if e.enabled else 'OFF'}: max H1 gap={fmt(gap)} ms {verdict} (<=50 ms)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
