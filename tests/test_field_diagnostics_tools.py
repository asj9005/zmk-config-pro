# SPDX-License-Identifier: MIT
"""Offline field-capture regressions; no serial devices, SDK or private keys."""
from __future__ import annotations

from collections import Counter
import contextlib
import copy
import io
import json
from pathlib import Path
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest import mock

TOOLS = Path(__file__).resolve().parents[1] / "tools"
sys.path.insert(0, str(TOOLS))
import field_diagnostics as schema
import collect_field_diagnostics as collector
import analyze_field_diagnostics as analyzer


def parts(seq=1, uptime=30000, count=0, boot="0123456789abcdef", fw="a" * 40):
    """A full schema-1 frame; known samples are 200us in the second bin."""
    common = {"schema": 1, "role": "dongle", "boot": boot, "seq": seq, "total": 72}
    result = []
    def add(kind, **fields):
        result.append(dict(common, kind=kind, part=len(result), **fields))
    add("begin", format="TOTEM_FIELD_V1", fw=fw, tree="b" * 40, dirty=0,
        uptime_ms=uptime, tick_hz=32768, rx_queue_resolution_us=1000,
        tx_drop=0, interval_ms=30000)
    for name in schema.METRICS:
        bins = {f"b{i}": count if i == 1 else 0 for i in range(len(schema.BIN_UPPER_US))}
        add("metric", name=name, count=count, sum_us=count * 200,
            max_us=200 if count else 0, flags=0, **bins)
    for name in schema.ISSUES:
        for scope in range(3):
            add("issue", name=name, scope=scope, count=0, last_ms=0, code=0)
    for name in schema.HOLDTAPS:
        add("holdtap", name=name, tap=0, hold=0)
    for name, fields in schema.DIAG_FIELDS.items():
        add("diag", name=name, **dict.fromkeys(fields, 0))
    add("end", uptime_ms=uptime + 80, tx_drop=0)
    return result


def wire(part):
    return ("[totem-field] " + " ".join(f"{key}={value}" for key, value in part.items())).encode("ascii")


def snapshot(rows):
    assembler = schema.SnapshotAssembler()
    result = None
    for row in rows:
        parsed = schema.parse_wire(wire(row))
        if parsed is None:
            raise AssertionError(f"invalid fixture row {row['part']}")
        result = assembler.add(parsed) or result
    if result is None:
        raise AssertionError("fixture did not assemble")
    return result


def record(rows, index=0, connection=1):
    return {"kind": "snapshot", "record_index": index, "stream": "console1", "connection": connection,
            "pc_utc": "2026-10-10T12:00:00.000Z", "pc_monotonic_ns": (index + 1) * 30_000_000_000,
            "snapshot": snapshot(rows)}


class FieldFormatTests(unittest.TestCase):
    def test_complete_frame_out_of_order_and_duplicate(self):
        values = parts(count=4)
        assembler = schema.SnapshotAssembler()
        self.assertIsNone(assembler.add(values[71]))
        self.assertIsNone(assembler.add(values[71]))
        result = None
        for row in reversed(values[:71]):
            result = assembler.add(row) or result
        self.assertEqual(schema.expand_snapshot(result), values)
        self.assertEqual(assembler.stats["duplicate_parts"], 1)

    def test_partial_conflicting_and_mixed_boot_frames_never_count(self):
        assembler = schema.SnapshotAssembler()
        values = parts()
        for row in values[:-1]:
            self.assertIsNone(assembler.add(row))
        changed = dict(values[1], max_us=99)
        self.assertIsNone(assembler.add(changed))
        self.assertEqual(assembler.stats["conflicting_parts"], 1)
        for boot in ("1" * 16, "2" * 16, "3" * 16):
            self.assertIsNone(assembler.add(parts(boot=boot)[0]))
        assembler.finish()
        self.assertEqual(assembler.stats["incomplete_frames"], 3)
        self.assertEqual(assembler.stats["complete_frames"], 0)

    def test_wire_allowlist_rejects_raw_keys_extra_fields_bad_bins_and_types(self):
        good = wire(parts()[0])
        secret_text = b"SYNTHETIC_TYPED_TEXT_do_not_store"
        for data in (b"[esb-diag] raw=" + secret_text, secret_text,
                     good + b" typed=" + secret_text, good + b" schema=1",
                     good.replace(b"schema=1", b"schema=2"), good + b"\xff"):
            self.assertIsNone(schema.parse_wire(data))
        row = parts()[1]
        for changed in (dict(row, count=1), dict(row, flags=8), dict(row, count=-1),
                        dict(row, count=1 << 32), dict(row, name="key_position"),
                        dict(row, sum_us=1), dict(row, max_us=1)):
            self.assertIsNone(schema.parse_wire(wire(changed)))
        observed = parts(count=2)[1]
        for changed in (dict(observed, max_us=9999), dict(observed, sum_us=99),
                        dict(observed, sum_us=9999)):
            self.assertIsNone(schema.parse_wire(wire(changed)))
        self.assertIsNone(schema.validate_part(dict(row, name=[])))
        self.assertIsNone(schema.validate_part(dict(row, count=True)))

    def test_line_limit_discards_suffix_until_newline_and_recovers(self):
        buffer = schema.LineBuffer()
        self.assertEqual(buffer.feed(b"x" * (schema.MAX_LINE_BYTES + 1)), [])
        self.assertEqual(buffer.feed(wire(parts()[0]) + b"\n"), [])
        self.assertEqual(buffer.oversized, 1)
        line = wire(parts()[0])
        self.assertEqual(buffer.feed(line[:10]), [])
        self.assertEqual(buffer.feed(line[10:] + b"\r\n"), [line])

    def test_json_duplicate_keys_nonfinite_and_nested_input_rejected(self):
        for raw in (b'{"kind":1,"kind":2}', b'{"value":NaN}', b'[]', b'\xff',
                    b'{"value":' + b'[' * 2000 + b'0' + b']' * 2000 + b'}'):
            with self.subTest(raw_len=len(raw)), self.assertRaises(ValueError):
                schema.load_json_line(raw)


class AnalysisTests(unittest.TestCase):
    def test_delta_histograms_use_bounds_and_boot_max_not_exact_quantile(self):
        worker = analyzer.Analyzer()
        worker.add(record(parts(count=100), 0))
        worker.add(record(parts(seq=2, uptime=60000, count=110), 1))
        group = worker.report()["segments"][0]
        metric = group["metrics"]["usb_queue"]
        self.assertEqual(metric["count"], 10)
        self.assertEqual(metric["mean_us"], 200)
        self.assertEqual(metric["p95_bounds_us"], {"lower_us": 101, "upper_us": 250})
        self.assertEqual(metric["boot_max_observed_us"], 200)
        self.assertEqual(group["interval_span_ms"], 30000)
        overflow = [0] * 18
        overflow[-1] = 1
        self.assertEqual(analyzer.percentile_bounds(overflow, 99),
                         {"lower_us": 1000001, "upper_us": None})

    def test_loss_out_of_order_and_sequence_wrap(self):
        worker = analyzer.Analyzer()
        worker.add(record(parts(seq=schema.UINT32, count=10), 0))
        worker.add(record(parts(seq=0, uptime=60000, count=11), 1))
        worker.add(record(parts(seq=3, uptime=150000, count=14), 2))
        worker.add(record(parts(seq=2, uptime=120000, count=13), 3))
        group = worker.report()["segments"][0]
        self.assertEqual(group["complete_snapshots"], 3)
        self.assertEqual(group["missing_snapshots"], 2)
        self.assertEqual(group["metrics"]["rx_queue"]["count"], 4)
        self.assertEqual(group["warnings"]["duplicate_or_out_of_order_snapshot"], 1)
        self.assertEqual(group["warnings"]["long_telemetry_gap"], 1)

    def test_reconnection_boot_firmware_and_clock_reset_split_baselines(self):
        worker = analyzer.Analyzer()
        worker.add(record(parts(count=20), 0))
        worker.add(record(parts(seq=2, count=40, uptime=60000), 1, connection=2))
        worker.add(record(parts(seq=1, count=1, boot="f" * 16), 2, connection=2))
        worker.add(record(parts(seq=2, count=2, uptime=60000, boot="f" * 16, fw="c" * 40), 3, connection=2))
        worker.add(record(parts(seq=3, count=3, uptime=100, boot="f" * 16, fw="c" * 40), 4, connection=2))
        groups = worker.report()["segments"]
        self.assertEqual(len(groups), 5)
        self.assertTrue(all(group["metrics"]["rx_queue"]["count"] == 0 for group in groups))
        self.assertEqual(groups[-1]["start_reason"], "clock_reset")

    def test_event_wrap_reset_and_histogram_saturation_are_explicit(self):
        old, new = parts(count=1), parts(seq=2, uptime=60000, count=2)
        old[13]["count"], new[13]["count"] = schema.UINT32 - 1, 2
        old[14]["count"], new[14]["count"] = 100, 2
        new[1]["flags"] = 1
        new[2]["count"], new[2]["sum_us"], new[2]["b1"], new[2]["max_us"] = 0, 0, 0, 0
        worker = analyzer.Analyzer()
        worker.add(record(old, 0))
        worker.add(record(new, 1))
        group = worker.report()["segments"][0]
        self.assertEqual(group["issues"]["peer_timeout:scope0"], 4)
        self.assertNotIn("peer_timeout:scope1", group["issues"])
        self.assertEqual(group["counter_wraps"], 1)
        self.assertEqual(group["warnings"]["counter_reset_intervals"], 1)
        for name in ("rx_queue", "rx_process"):
            self.assertEqual(group["metrics"][name]["invalid_intervals"], 1)
            self.assertIsNone(group["metrics"][name]["mean_us"])

    def test_missing_telemetry_is_unknown_and_mutated_stored_row_rejected(self):
        sample = record(parts())
        del sample["snapshot"]["rows"][20]
        self.assertFalse(analyzer.valid_record(sample))
        worker = analyzer.Analyzer()
        report = worker.report()
        self.assertEqual(report["segments"], [])
        self.assertIn("unknown", report["interpretation"][0])


class CaptureBoundaryTests(unittest.TestCase):
    def test_metadata_required_studio_never_opened_and_no_serial_writes(self):
        def port(device, vid=collector.VID, pid=collector.PID, label=None):
            return SimpleNamespace(device=device, vid=vid, pid=pid, interface=label, hwid="")
        ports = [port("COM10"), port("COM4"), port("COM9"), port("COM2", vid=3)]
        mappings = {"COM10": 0, "COM4": 3}
        self.assertEqual(collector.console_candidates(ports, mappings, set()), ["COM10"])
        self.assertEqual(collector.console_candidates(ports, mappings, {"com4"}), [])
        self.assertEqual(collector.console_candidates(ports, mappings, {"com9"}), [])
        serial = mock.Mock()
        handle = mock.Mock()
        serial.Serial.return_value = handle
        self.assertIs(collector.open_passive(serial, "COM10"), handle)
        self.assertFalse(handle.dtr)
        self.assertFalse(handle.rts)
        handle.write.assert_not_called()
        handle.send_break.assert_not_called()
        handle.open.assert_called_once()

    def test_ring_rotation_retains_order_and_does_not_touch_existing_paths(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            writer = schema.RingWriter(root / "run", file_bytes=512, max_files=2)
            for index in range(20):
                writer.write({"kind": "marker", "symptom": "input_delay",
                              "pc_utc": "2026-10-10T12:00:00.000Z", "pc_monotonic_ns": index})
            writer.close()
            self.assertLessEqual(sum(path.stat().st_size for path in writer.owned), 1024)
            records = list(analyzer.capture_records(root / "run", Counter()))
            self.assertEqual(records[-1]["record_index"], 19)
            self.assertGreater(records[0]["record_index"], 0)
            self.assertEqual([r["record_index"] for r in records], sorted(r["record_index"] for r in records))
            with self.assertRaises(FileExistsError):
                schema.RingWriter(root / "run")
            self.assertEqual(list(analyzer.capture_records(root / "run", Counter())), records)

    def test_partial_malformed_and_private_text_do_not_escape_analysis(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            sample = record(parts())
            path = root / "capture-00.jsonl"
            sensitive = "SYNTHETIC_PRIVATE_TYPED_TEXT"
            path.write_text(json.dumps(sample) + "\n" + sensitive + "\n" +
                            json.dumps(dict(sample, key_content=sensitive)) + "\n" + '{"partial":', encoding="utf-8")
            report = analyzer.analyze(root)
            self.assertNotIn(sensitive, json.dumps(report))
            self.assertEqual(report["input_problems"]["malformed_records"], 1)
            self.assertEqual(report["input_problems"]["rejected_records"], 1)
            self.assertEqual(report["input_problems"]["oversized_or_partial_record"], 1)
            self.assertEqual(len(report["segments"]), 1)

    def test_full_collect_reconnect_and_sanitized_marker_without_hardware(self):
        clock = SimpleNamespace(now=0.0)
        opened = []
        sensitive = b"SYNTHETIC_TYPED_TEXT_MUST_NOT_PERSIST"
        class Port:
            def __init__(self, **kwargs):
                self.dtr, self.rts, self.port = True, True, None
                self.closed = False
                self.data = bytearray(sensitive + b"\n" + b"\n".join(wire(row) for row in parts()) + b"\n")
                opened.append(self)
            def open(self):
                assert self.dtr is False and self.rts is False
            def read(self, size):
                result = bytes(self.data[:size])
                del self.data[:size]
                return result
            def close(self):
                self.closed = True
            def write(self, _):
                raise AssertionError("serial writes are forbidden")
        serial = SimpleNamespace(Serial=Port, SerialException=OSError)
        p = SimpleNamespace(device="COM10", vid=collector.VID, pid=collector.PID, interface=None, hwid="")
        ports = SimpleNamespace(comports=lambda: [] if 3 <= clock.now < 6 else [p])
        with tempfile.TemporaryDirectory() as directory:
            args = SimpleNamespace(port=[], output=Path(directory), hours=0.0025)
            with mock.patch.object(collector, "windows_interfaces", return_value={"COM10": 0}), \
                 mock.patch.object(collector.time, "monotonic", side_effect=lambda: clock.now), \
                 mock.patch.object(collector.time, "sleep", side_effect=lambda n: setattr(clock, "now", clock.now + n)), \
                 contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(collector.collect(args, serial, ports), 0)
            capture = next(Path(directory).iterdir())
            all_bytes = b"".join(path.read_bytes() for path in capture.iterdir())
            self.assertNotIn(sensitive, all_bytes)
            self.assertEqual(len(opened), 2)
            self.assertTrue(all(port.closed for port in opened))
            report = analyzer.analyze(capture)
            self.assertEqual(len(report["segments"]), 2)
            self.assertEqual(report["capture_events"]["disconnect"], 2)
            self.assertEqual(report["collector"]["counts"]["discarded_lines"], 2)

    def test_marker_single_slot_and_free_text_refused(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            schema.write_metadata(root, {"format": 1, "status": "running"})
            with contextlib.redirect_stdout(io.StringIO()):
                collector.mark(root, "mouse_stuck")
                with self.assertRaises(FileExistsError):
                    collector.mark(root, "key_repeat")
                with self.assertRaises(ValueError):
                    collector.mark(root, "some typed text")
            writer = mock.Mock()
            stats = Counter()
            collector.take_marker(root, writer, stats)
            self.assertEqual(writer.write.call_args.args[0]["symptom"], "mouse_stuck")
            self.assertFalse((root / "marker-request.json").exists())
            self.assertEqual(stats["markers"], 1)

    def test_thirty_second_first_frame_and_stale_then_resumed_telemetry(self):
        clock = SimpleNamespace(now=0.0)
        opened = []
        class Port:
            def __init__(self, **kwargs):
                self.dtr, self.rts, self.port = True, True, None
                self.frames = [(30, bytearray(b"\n".join(wire(r) for r in parts()) + b"\n")),
                               (120, bytearray(b"\n".join(wire(r) for r in parts(seq=4, uptime=120000)) + b"\n"))]
                opened.append(self)
            def open(self):
                assert self.dtr is False and self.rts is False
            def read(self, size):
                if not self.frames or clock.now < self.frames[0][0]:
                    return b""
                data = self.frames[0][1]
                result = bytes(data[:size])
                del data[:size]
                if not data:
                    self.frames.pop(0)
                return result
            def close(self):
                pass
        serial = SimpleNamespace(Serial=Port, SerialException=OSError)
        p = SimpleNamespace(device="COM10", vid=collector.VID, pid=collector.PID, interface=None, hwid="")
        ports = SimpleNamespace(comports=lambda: [p])
        with tempfile.TemporaryDirectory() as directory:
            args = SimpleNamespace(port=[], output=Path(directory), hours=125 / 3600)
            with mock.patch.object(collector, "windows_interfaces", return_value={"COM10": 0}), \
                 mock.patch.object(collector.time, "monotonic", side_effect=lambda: clock.now), \
                 mock.patch.object(collector.time, "sleep", side_effect=lambda n: setattr(clock, "now", clock.now + n)), \
                 contextlib.redirect_stdout(io.StringIO()):
                collector.collect(args, serial, ports)
            self.assertEqual(len(opened), 1)  # Never close before the first 30s export.
            capture = next(Path(directory).iterdir())
            report = json.loads((capture / "report.json").read_text())
            self.assertEqual(report["capture_events"]["telemetry_stale"], 1)
            self.assertEqual(report["capture_events"]["telemetry_resumed"], 1)
            self.assertEqual(report["segments"][0]["missing_snapshots"], 2)

    def test_input_record_and_file_caps_are_enforced(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            line = json.dumps(record(parts())) + "\n"
            (root / "capture-00.jsonl").write_text(line + line, encoding="utf-8")
            with mock.patch.object(analyzer, "FILE_BYTES", 100), self.assertRaises(ValueError):
                list(analyzer.capture_records(root, Counter()))
            with mock.patch.object(analyzer, "MAX_RECORDS", 1), self.assertRaises(ValueError):
                list(analyzer.capture_records(root, Counter()))


if __name__ == "__main__":
    unittest.main()
