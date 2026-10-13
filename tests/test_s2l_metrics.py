#!/usr/bin/env python3
"""Generate temporary CSV fixtures and check metrics: python -I tests/test_s2l_metrics.py."""

import csv
from pathlib import Path
import runpy
import subprocess
import sys
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
SCRIPT = ROOT / "tools" / "s2l_metrics.py"
METRICS = runpy.run_path(str(SCRIPT))
# Use the actual converter's current column names, without importing from sys.path.
COLUMNS = runpy.run_path(str(ROOT / "tools" / "trace2csv.py"))["COLUMNS"].split(",")


def write_csv(path, rows, columns=COLUMNS, comment=True):
    with path.open("w", encoding="utf-8", newline="") as stream:
        if comment:
            stream.write("# synthetic trace (no game required)\n")
        writer = csv.DictWriter(stream, fieldnames=columns, extrasaction="ignore")
        writer.writeheader()
        for seq, row in enumerate(rows, 1):
            writer.writerow(dict(row, seq=row.get("seq", seq)))


def perfect_lockstep():
    """One H1 per beat; each applied step follows the PREVIOUS interval."""
    rows, beat = [], 0.0
    durations = [0.010, 0.014, 0.011, 0.015] * 4
    for i in range(len(durations) + 1):
        rows.extend([dict(type="HB", t_s=beat, enabled=0, hb_other=0, hb_repeat=0),
                     dict(type="XELL", t_s=beat, enabled=0)])
        if i == len(durations):
            break
        applied = durations[i - 1] if i else durations[i]
        rows.append(dict(type="GATE_END", t_s=beat + 0.0005, enabled=0,
                         gate_wait_us=200, gate_timeout=0, aux2=0))
        rows.append(dict(type="H1", t_s=beat + 0.001, enabled=1, fps_a=120,
                         speed_req=1, r_applied=applied * 120, r_would=99,
                         dt_ms=123, time_lost_ms=0, time_gained_ms=0))
        beat += durations[i]
    return rows


class MetricsTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix="s2l-metrics-")
        self.addCleanup(self.tmp.cleanup)
        self.csv = Path(self.tmp.name) / "ticks.csv"
        self.log = Path(self.tmp.name) / "mod.log"

    def run_cli(self, rows, *args, columns=COLUMNS):
        write_csv(self.csv, rows, columns)
        result = subprocess.run([sys.executable, "-I", str(SCRIPT), str(self.csv), *args],
                                text=True, capture_output=True, check=False)
        self.assertEqual(result.returncode, 0, result.stderr)
        return result.stdout

    def parsed(self, rows, columns=COLUMNS):
        write_csv(self.csv, rows, columns)
        return METRICS["read_csv"](self.csv)[0]

    def interval_result(self, rows, label="HB", spans=None):
        events = self.parsed(rows)
        spans = spans or [METRICS["Span"](events[0].t, events[-1].t, 1)]
        return METRICS["intervals"]([e for e in events if e.kind == label and not e.other],
                                    [e for e in events if e.kind == "H1"], spans, True)

    def test_perfect_lockstep_cli(self):
        output = self.run_cli(perfect_lockstep())
        self.assertIn("M1 heartbeat: histogram {1: 16}; exactly-1=100.00% PASS", output)
        self.assertIn("M1 XELL: histogram {1: 16}; exactly-1=100.00% PASS", output)
        lagged = [line for line in output.splitlines() if "lagged:" in line and "unlagged" not in line and "n/a" not in line]
        self.assertEqual(len(lagged), 2)
        self.assertTrue(all("mean=0.000 ms p95=0.000 ms" in line and "PASS" in line for line in lagged))
        self.assertIn("timeouts=0 backoffs=0", output)
        self.assertIn("wait mean/p95/max=0.200/0.200/0.200 ms", output)
        self.assertIn("M6 total toggles=0", output)

    def test_lag_direction_and_applied_factor(self):
        _, errors, floor, baseline, _ = self.interval_result(perfect_lockstep())
        self.assertEqual(len(errors), 15)
        self.assertLess(max(errors), 1e-10)
        self.assertGreater(sum(floor), 0)
        self.assertGreater(sum(baseline), 0)
        # Supplying the current interval should produce the floor, not zero.
        rows = perfect_lockstep()
        for i, row in enumerate(r for r in rows if r["type"] == "H1"):
            row["r_applied"] = [0.010, 0.014, 0.011, 0.015][i % 4] * 120
        _, errors, floor, _, _ = self.interval_result(rows)
        for error, jitter in zip(errors, floor):
            self.assertAlmostEqual(error, jitter)
        for row in rows:
            if row["type"] == "H1":
                row["r_applied"] *= 2
        output = self.run_cli(rows)
        self.assertTrue(any("M2 heartbeat lagged:" in line and "FAIL" in line for line in output.splitlines()))

    def test_qpc_attribution_when_h1_is_written_before_beat(self):
        rows = perfect_lockstep()
        qpf, origin = 10_000_000, 90_000_000_000_000_000
        for row in rows:
            row["qpc"] = origin + round(row["t_s"] * qpf)
        # Alternate rounded timestamp ties and distinct timestamps. Every H1
        # follows its beat in QPC, but gets a lower write seq than HB and XELL.
        ticks = [row for row in rows if row["type"] == "H1"]
        beats = [row for row in rows if row["type"] == "HB"]
        for i, (tick, beat) in enumerate(zip(ticks, beats)):
            tick["qpc"] = beat["qpc"] + (1 if i % 2 == 0 else 20)
            tick["t_s"] = round((tick["qpc"] - origin) / qpf, 6)
        rows.sort(key=lambda row: (row["t_s"], row["type"] != "H1"))
        # Even distinct timestamps must be emitted in write order.
        for tick, beat in zip(ticks, beats):
            rows.remove(tick)
            rows.insert(rows.index(beat), tick)
        for label in ("HB", "XELL"):
            with self.subTest(label=label):
                hist, lagged, _, _, _ = self.interval_result(rows, label)
                self.assertEqual(hist, {1: 16})
                self.assertEqual(len(lagged), 15)
                self.assertLess(max(lagged), 1e-10)
        output = self.run_cli(rows)
        for label in ("heartbeat", "XELL"):
            self.assertIn(f"M1 {label}: histogram {{1: 16}}; exactly-1=100.00% PASS", output)
            self.assertIn(f"M2 {label} lagged: mean=0.000 ms p95=0.000 ms", output)

    def test_speed_scaling_and_missing_game_fields(self):
        rows = perfect_lockstep()
        for row in rows:
            if row["type"] == "H1":
                row["speed_req"] = 2
                row["r_applied"] /= 2
        self.assertLess(max(self.interval_result(rows)[1]), 1e-10)
        del next(row for row in rows if row["type"] == "H1")["r_applied"]
        result = self.interval_result(rows)
        self.assertEqual(sum(result[0].values()), 16)
        self.assertEqual(len(result[3]), 15)  # Invalid steps never become zeros.

    def test_toggle_state_wins_over_effective_and_unused_enabled(self):
        rows = perfect_lockstep()
        rows.append(dict(type="TOGGLE", t_s=0.100, enabled=0))
        rows.append(dict(type="TOGGLE", t_s=0.150, enabled=1))
        # L3 can be passive even while the requested toggle is ON.
        for row in rows:
            if row["type"] == "H1":
                row["enabled"] = 0
        events = self.parsed(rows)
        spans, toggles, source = METRICS["split_spans"](events, 0.05, 0.19)
        self.assertEqual([(s.start, s.end, s.state) for s in spans],
                         [(0.05, 0.1, 1), (0.1, 0.15, 0), (0.15, 0.19, 1)])
        self.assertEqual(len(toggles), 2)
        self.assertEqual(source, "TOGGLE records")
        output = self.run_cli(rows, "--from", "0.12", "--to", "0.14")
        self.assertIn("Toggle-OFF: 0.120000..0.140000 s", output)
        self.assertIn("M6 total toggles=0", output)

    def test_enabled_fallback_and_no_cross_span_intervals(self):
        rows = perfect_lockstep()
        for row in rows:
            if row["type"] == "H1" and row["t_s"] > 0.1:
                row["enabled"] = 0
        events = self.parsed(rows)
        spans, toggles, _ = METRICS["split_spans"](events, 0, events[-1].t)
        self.assertEqual(len(toggles), 1)
        result = self.interval_result(rows, spans=spans)
        # The interval around the transition is excluded, not divided.
        self.assertEqual(sum(result[0].values()), 15)
        self.assertEqual(len(result[1]), 13)

    def test_missing_record_types_and_empty_input(self):
        output = self.run_cli([dict(type="MARKER", t_s=1), dict(type="MARKER", t_s=2)],
                              columns=["type", "t_s"])
        self.assertIn("M1 heartbeat: n/a", output)
        self.assertIn("M2 XELL lagged: n/a", output)
        self.assertIn("timeouts=n/a backoffs=n/a", output)
        self.assertIn("Toggle-unknown:", output)
        output = self.run_cli([])
        self.assertIn("M1-M6: n/a", output)
        rows = [r for r in perfect_lockstep() if r["type"] != "H1"]
        output = self.run_cli(rows)
        self.assertIn("M1 heartbeat: n/a", output)
        self.assertIn("floor: mean=", output)

    def test_hb_repeats_count_and_other_excluded(self):
        rows = perfect_lockstep()
        for row in rows:
            if row["type"] == "HB":
                row["hb_repeat"] = 1
        rows.append(dict(type="HB", t_s=0.005, hb_other=1, hb_repeat=0))
        output = self.run_cli(rows)
        self.assertIn("exactly-1=100.00% PASS", output)
        self.assertIn("hb_other=1 repeats=16 repeats/heartbeat=1.000", output)

    def test_gate_counter_deltas_and_raw_layout(self):
        rows = [dict(type="H1", t_s=0, enabled=1),
                dict(type="GATE_END", t_s=0.01, flags="0x1", dt_ms=2, aux2=0),
                dict(type="GATE_END", t_s=0.02, flags="0x0", dt_ms=4, aux2=1),
                dict(type="GATE_END", t_s=0.03, flags="0x0", dt_ms=6, aux2=1)]
        columns = ["seq", "type", "t_s", "enabled", "flags", "dt_ms", "aux2"]
        output = self.run_cli(rows, "--from", "0.015", columns=columns)
        self.assertIn("timeouts=0 backoffs=1", output)
        self.assertIn("wait mean/p95/max=5.000/6.000/6.000 ms", output)

    def test_clamp_losses_are_deltas_and_correct_expected_real(self):
        rows = [dict(type="H1", t_s=0, enabled=1, fps_a=100, r_applied=1,
                     time_lost_ms=0, time_gained_ms=0)]
        for i in range(1, 11):
            rows.append(dict(type="H1", t_s=i / 100, enabled=1, fps_a=100, r_applied=0.9,
                             time_lost_ms=i, time_gained_ms=0))
        output = self.run_cli(rows, "--from", "0.02", "--to", "0.08")
        self.assertIn("game=0.054000 s real=0.060000 s", output)
        self.assertIn("lost=6.000 ms gained=0.000 ms", output)
        self.assertIn("game/(real-lost+gained)=1.000000 PASS", output)

    def test_rates_aggregate_spans_without_toggle_gap(self):
        rows = [dict(type="HB", t_s=0), dict(type="XELL", t_s=0)]
        for start, interval, state, count in [(0, 0.010, 0, 100), (1, 0.020, 1, 50), (2, 0.010, 0, 100)]:
            for i in range(1, count + 1):
                t = start + i * interval
                rows.extend([dict(type="HB", t_s=t), dict(type="XELL", t_s=t),
                             dict(type="H1", t_s=t - 0.001, enabled=state, fps_a=100, r_applied=1)])
        rows.extend([dict(type="TOGGLE", t_s=1, enabled=1),
                     dict(type="TOGGLE", t_s=2, enabled=0)])
        output = self.run_cli(rows)
        self.assertIn("heartbeat: -50.000% FAIL", output)
        self.assertIn("XELL: -50.000% FAIL", output)

    def test_toggle_gap_uses_context_and_detects_hang_spanning_window(self):
        rows = [dict(type="H1", t_s=0, enabled=1), dict(type="H1", t_s=4, enabled=0),
                dict(type="TOGGLE", t_s=2, enabled=0)]
        output = self.run_cli(rows, "--from", "1.99", "--to", "2.01")
        self.assertIn("max H1 gap=4000.000 ms FAIL", output)

    def test_log_fallback_midnight_and_no_double_count(self):
        self.log.write_text("\n".join([
            "23:59:59.000 flight recorder: file-backed, 32768 records x 128 B",
            "23:59:59.000 CONFIG: StartEnabled=1",
            "00:00:00.000 GATE BACKOFF: three timeouts within 1 s",
            "00:00:01.000 GATE 2.0s: armed=100% waits=2 mean/max wait=1.000/2.000ms timeouts=3 backoffs=1",
            "00:00:01.000 PROBE 2.0s: legacy tick 0x5a6bb0 = 120 (60.00/s, game-time accuracy)",
            "00:00:01.000 SUMMARY 2.0s H1/P1: ticks=2 time_lost=0.000ms time_gained=0.000ms",
        ]), encoding="utf-8")
        rows = [dict(type="H1", t_s=i, enabled=1, fps_a=1, r_applied=1) for i in range(3)]
        output = self.run_cli(rows, "--log", str(self.log))
        self.assertIn("timeouts=3 backoffs=1 (log complete windows)", output)
        self.assertIn("wait mean/p95/max=1.000/n/a/2.000 ms", output)
        self.assertIn("game/(real-lost+gained)=1.000000 PASS", output)
        self.assertIn("legacy: 60.000 ticks/s INFO", output)
        self.assertNotIn("60 +/-0.3", output)
        self.log.write_text(self.log.read_text(encoding="utf-8").replace(
            "= 120 (60.00/s", "= 480 (240.00/s"), encoding="utf-8")
        output = self.run_cli(rows, "--log", str(self.log))
        self.assertIn("legacy: 240.000 ticks/s INFO", output)
        self.assertIn("game/(real-lost+gained)=1.000000 PASS", output)
        # A clipped window must not inherit the entire two-second log summary.
        output = self.run_cli(rows, "--from", "0.5", "--to", "1.5", "--log", str(self.log))
        self.assertIn("timeouts=n/a backoffs=1", output)
        self.assertIn("legacy: n/a", output)
        rows.append(dict(type="GATE_END", t_s=0.5, gate_timeout=1, gate_wait_us=3000, aux2=0))
        output = self.run_cli(rows, "--log", str(self.log))
        self.assertIn("timeouts=1 backoffs=0 (CSV)", output)


if __name__ == "__main__":
    unittest.main(verbosity=2)
