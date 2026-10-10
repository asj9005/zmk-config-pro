#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Analyze complete field snapshots without reconstructing typed input."""
from __future__ import annotations

import argparse
from collections import Counter
import json
import math
from pathlib import Path
import re
import sys

from field_diagnostics import (BIN_UPPER_US, DIAG_FIELDS, FILE_BYTES, FORMAT_VERSION,
                               HOLDTAPS, ISSUES, MARKERS, MAX_FILES, MAX_INPUT_BYTES,
                               MAX_JSON_BYTES, MAX_RECORDS, METRICS, TIMESTAMP_RE,
                               UINT32, UINT64, expand_snapshot, integer,
                               load_json_line, utc_now)

REASONS = {"removed", "read_failed", "unverified", "capture_stopped"}


def valid_record(record):
    if not isinstance(record, dict):
        return False
    common = {"kind", "record_index", "pc_utc", "pc_monotonic_ns"}
    if not common <= record.keys() or not integer(record["record_index"]) or not integer(record["pc_monotonic_ns"]):
        return False
    if not isinstance(record["kind"], str):
        return False
    if not isinstance(record["pc_utc"], str) or not TIMESTAMP_RE.fullmatch(record["pc_utc"]):
        return False
    if record["kind"] == "marker":
        return set(record) == common | {"symptom"} and isinstance(record["symptom"], str) and record["symptom"] in MARKERS
    common |= {"stream", "connection"}
    if not common <= record.keys() or not isinstance(record["stream"], str):
        return False
    if not re.fullmatch(r"console(?:[1-9]|1[0-6])", record["stream"]) or not integer(record["connection"], UINT32):
        return False
    kind = record["kind"]
    if kind in ("connect", "telemetry_stale", "telemetry_resumed"):
        return set(record) == common
    if kind == "disconnect":
        return set(record) == common | {"reason"} and isinstance(record["reason"], str) and record["reason"] in REASONS
    if kind == "verified":
        return set(record) == common | {"role"} and record["role"] in ("left", "right", "dongle")
    if kind == "snapshot":
        return set(record) == common | {"snapshot"} and expand_snapshot(record["snapshot"]) is not None
    return False


def capture_records(directory: Path, problems: Counter):
    directory = directory.resolve(strict=True)
    paths = sorted(directory.glob("capture-??.jsonl"))
    if len(paths) > MAX_FILES:
        raise ValueError("too many input files")
    ordered = []
    total_bytes = 0
    for path in paths:
        if path.is_symlink() or not path.is_file() or path.parent.resolve() != directory:
            raise ValueError("unsafe input path")
        size = path.stat().st_size
        total_bytes += size
        if size > FILE_BYTES or total_bytes > MAX_INPUT_BYTES:
            raise ValueError("input exceeds size cap")
        with path.open("rb") as source:
            first = source.readline(MAX_JSON_BYTES + 1)
        try:
            index = load_json_line(first).get("record_index")
            if not integer(index):
                raise ValueError("invalid index")
        except ValueError:
            problems["unreadable_file_start"] += 1
            continue
        ordered.append((index, path))
    seen, previous, consumed = 0, -1, 0
    for _, path in sorted(ordered):
        with path.open("rb") as source:
            file_consumed = 0
            while raw := source.readline(MAX_JSON_BYTES + 1):
                consumed += len(raw)
                file_consumed += len(raw)
                if consumed > MAX_INPUT_BYTES or file_consumed > FILE_BYTES:
                    raise ValueError("input grew beyond size cap")
                seen += 1
                if seen > MAX_RECORDS:
                    raise ValueError("too many input records")
                if len(raw) > MAX_JSON_BYTES or not raw.endswith(b"\n"):
                    problems["oversized_or_partial_record"] += 1
                    # Discard this whole oversized line without buffering it.
                    while raw and not raw.endswith(b"\n"):
                        raw = source.readline(MAX_JSON_BYTES + 1)
                        consumed += len(raw)
                        file_consumed += len(raw)
                        if consumed > MAX_INPUT_BYTES or file_consumed > FILE_BYTES:
                            raise ValueError("input grew beyond size cap")
                    continue
                try:
                    record = load_json_line(raw)
                except ValueError:
                    problems["malformed_records"] += 1
                    continue
                if not valid_record(record):
                    problems["rejected_records"] += 1
                    continue
                index = record["record_index"]
                if index <= previous:
                    problems["reordered_or_duplicate_records"] += 1
                    continue
                if previous >= 0 and index > previous + 1:
                    problems["missing_records"] += index - previous - 1
                previous = index
                yield record


def counter_delta(old, new, bits=32):
    if new >= old:
        return new - old, False
    maximum = (1 << bits) - 1
    # Only accept a plausible single wrap near the numeric boundary. All other
    # decreases invalidate the interval; a reset is never a giant event burst.
    if old >= maximum - maximum // 16 and new <= maximum // 16:
        return (new - old) & maximum, True
    return None, False


def percentile_bounds(bins, percentile):
    count = sum(bins)
    if not count:
        return None
    target, cumulative = math.ceil(count * percentile / 100), 0
    for index, value in enumerate(bins):
        cumulative += value
        if cumulative >= target:
            return {"lower_us": 0 if index == 0 else BIN_UPPER_US[index - 1] + 1,
                    "upper_us": BIN_UPPER_US[index]}
    raise ValueError("invalid histogram")


class Analyzer:
    def __init__(self):
        self.groups = []
        self.active = {}
        self.problems = Counter()
        self.markers = []
        self.connections = Counter()
        self.timeline = []
        self.snapshots = 0

    def _new_group(self, record, rows, reason):
        snap, begin = record["snapshot"], rows[0]
        group = {"stream": record["stream"], "connection": record["connection"],
                 "role": snap["role"], "boot": snap["boot"], "fw": begin["fw"],
                 "tree": begin["tree"], "dirty": begin["dirty"], "start_reason": reason,
                 "first_pc_utc": record["pc_utc"], "last_pc_utc": record["pc_utc"],
                 "first_uptime_ms": begin["uptime_ms"], "last_uptime_ms": begin["uptime_ms"],
                 "complete_snapshots": 0, "missing_snapshots": 0, "covered_intervals": 0,
                 "interval_span_ms": 0, "counter_wraps": 0, "tx_drop_delta": 0,
                 "metrics": {}, "issues": {}, "holdtap": {}, "diag_event_deltas": {},
                 "warnings": Counter()}
        for name in METRICS:
            group["metrics"][name] = {"count": 0, "sum_us": 0,
                                       "bins": [0] * len(BIN_UPPER_US), "valid_intervals": 0,
                                       "invalid_intervals": 0, "boot_max_observed_us": 0,
                                       "observed_flags": 0}
        self.groups.append(group)
        return {"group": group, "previous": None, "record": None}

    def add(self, record):
        kind = record["kind"]
        if kind == "marker":
            if len(self.markers) < 10000:
                self.markers.append({key: record[key] for key in ("symptom", "pc_utc", "pc_monotonic_ns")})
            else:
                self.problems["marker_limit"] += 1
            return
        if kind != "snapshot":
            self.connections[kind] += 1
            if len(self.timeline) < 10000:
                self.timeline.append({key: value for key, value in record.items() if key != "record_index"})
            else:
                self.problems["timeline_limit"] += 1
            return
        rows = expand_snapshot(record["snapshot"])
        snap, begin = record["snapshot"], rows[0]
        identity = (record["stream"], record["connection"], snap["role"], snap["boot"],
                    begin["fw"], begin["tree"], begin["dirty"])
        state = self.active.get(record["stream"])
        if state is None or state["identity"] != identity:
            if len(self.groups) >= 4096:
                self.problems["segment_limit"] += 1
                return
            state = self._new_group(record, rows, "first_observation_or_identity_change")
            state["identity"] = identity
            self.active[record["stream"]] = state
        previous, group = state["previous"], state["group"]
        if previous is not None:
            sequence_delta = (snap["seq"] - previous[0]["seq"]) & UINT32
            if sequence_delta == 0 or sequence_delta >= (1 << 31):
                group["warnings"]["duplicate_or_out_of_order_snapshot"] += 1
                return
            elapsed = begin["uptime_ms"] - previous[0]["uptime_ms"]
            if elapsed <= 0 or record["pc_monotonic_ns"] <= state["record"]["pc_monotonic_ns"]:
                group["warnings"]["clock_reset_or_inconsistent_order"] += 1
                state = self._new_group(record, rows, "clock_reset")
                state["identity"] = identity
                self.active[record["stream"]] = state
                group, previous = state["group"], None
            else:
                group["missing_snapshots"] += sequence_delta - 1
                group["interval_span_ms"] += elapsed
                group["covered_intervals"] += 1
                if elapsed > max(1, begin["interval_ms"]) * 2:
                    group["warnings"]["long_telemetry_gap"] += 1
                self._deltas(group, previous, rows)
        group["complete_snapshots"] += 1
        self.snapshots += 1
        group["last_pc_utc"] = record["pc_utc"]
        group["last_uptime_ms"] = begin["uptime_ms"]
        group["latest_diag"] = {row["name"]: {key: value for key, value in row.items()
                                  if key not in ("schema", "role", "boot", "seq", "part", "total", "kind", "name")}
                                for row in rows[66:71]}
        group["last_issue_observations"] = {
            f"{row['name']}:scope{row['scope']}": {"boot_count": row["count"],
                "last_uptime_ms": row["last_ms"], "code": row["code"]}
            for row in rows[13:61] if row["count"]}
        for row in rows[1:13]:
            item = group["metrics"][row["name"]]
            item["boot_max_observed_us"] = max(item["boot_max_observed_us"], row["max_us"])
            item["observed_flags"] |= row["flags"]
        state["previous"], state["record"] = rows, record

    def _deltas(self, group, previous, rows):
        for old, new in zip(previous[1:13], rows[1:13]):
            item = group["metrics"][new["name"]]
            # Histograms saturate explicitly. A flagged or decreasing series
            # has no reliable mean/percentile for this interval.
            names = ("count", "sum_us") + tuple(f"b{i}" for i in range(len(BIN_UPPER_US)))
            if old["flags"] or new["flags"] or new["max_us"] < old["max_us"] or any(new[name] < old[name] for name in names):
                item["invalid_intervals"] += 1
                continue
            delta = {name: new[name] - old[name] for name in names}
            if delta["count"] != sum(delta[f"b{i}"] for i in range(len(BIN_UPPER_US))):
                item["invalid_intervals"] += 1
                continue
            if (delta["count"] == 0 and delta["sum_us"] != 0) or delta["sum_us"] > delta["count"] * new["max_us"]:
                item["invalid_intervals"] += 1
                continue
            item["count"] += delta["count"]
            item["sum_us"] += delta["sum_us"]
            item["valid_intervals"] += 1
            item["bins"] = [value + delta[f"b{i}"] for i, value in enumerate(item["bins"])]

        def accumulate(target, key, old, new):
            delta, wrapped = counter_delta(old, new)
            if delta is None:
                group["warnings"]["counter_reset_intervals"] += 1
                return
            target[key] = target.get(key, 0) + delta
            group["counter_wraps"] += wrapped

        drops = {}
        accumulate(drops, "drop", previous[0]["tx_drop"], rows[0]["tx_drop"])
        group["tx_drop_delta"] += drops.get("drop", 0)
        for old, new in zip(previous[13:61], rows[13:61]):
            key = f"{new['name']}:scope{new['scope']}"
            accumulate(group["issues"], key, old["count"], new["count"])
        for old, new in zip(previous[61:66], rows[61:66]):
            for action in ("tap", "hold"):
                accumulate(group["holdtap"], f"{new['name']}:{action}", old[action], new[action])
        for key in DIAG_FIELDS["events"]:
            if key != "last_frame_err":
                accumulate(group["diag_event_deltas"], key, previous[67][key], rows[67][key])

    def report(self):
        for group in self.groups:
            group["warnings"] = dict(group["warnings"])
            for item in group["metrics"].values():
                item["mean_us"] = item["sum_us"] / item["count"] if item["count"] else None
                item["p50_bounds_us"] = percentile_bounds(item["bins"], 50)
                item["p95_bounds_us"] = percentile_bounds(item["bins"], 95)
                item["p99_bounds_us"] = percentile_bounds(item["bins"], 99)
        return {"format": FORMAT_VERSION, "generated_utc": utc_now(),
                "complete_snapshots": self.snapshots, "segments": self.groups,
                "capture_events": dict(self.connections), "input_problems": dict(self.problems),
                "connection_timeline": self.timeline,
                "markers": self.markers,
                "interpretation": [
                    "Missing or invalid telemetry is unknown, never proof of healthy operation.",
                    "Deltas cover only consecutive observations in the same connection/role/boot/firmware segment; first snapshots are baselines.",
                    "Snapshot rows are sampled separately, not one global atomic instant. Gaps may contain bursts and cannot identify exact event order.",
                    "Means use valid cumulative count/sum differences. Percentiles are histogram bounds, not exact values.",
                    "Maxima are largest boot-scoped values seen, possibly from before capture; they are not interval maxima.",
                    "Counter wrap handling assumes at most one wrap near the 32-bit boundary; other decreases invalidate that counter interval.",
                    "PC timestamps mark USB console receipt and human markers, not physical key-to-PC, USB HID, or RF latency.",
                    "Timing observed over USB may differ from battery-only operation; no battery-only interval is silently filled in."]}


def analyze(directory: Path):
    analyzer = Analyzer()
    for record in capture_records(directory, analyzer.problems):
        analyzer.add(record)
    report = analyzer.report()
    metadata = directory / "capture-meta.json"
    if metadata.is_file() and not metadata.is_symlink() and metadata.stat().st_size <= 16384:
        try:
            raw = json.loads(metadata.read_text(encoding="utf-8"))
            # Metadata is untrusted too: include only bounded numeric summaries.
            report["collector"] = {key: raw[key] for key in ("rotations", "records") if integer(raw.get(key))}
            counts = raw.get("counts", {})
            allowed = ("incomplete_frames", "conflicting_parts", "duplicate_parts", "inconsistent_frames",
                       "discarded_lines", "oversized_lines", "disconnects", "snapshots", "open_failed")
            report["collector"]["counts"] = {key: counts[key] for key in allowed if integer(counts.get(key))}
            for key in ("start_utc", "end_utc", "updated_utc"):
                if isinstance(raw.get(key), str) and TIMESTAMP_RE.fullmatch(raw[key]):
                    report["collector"][key] = raw[key]
            if raw.get("status") in ("running", "stopped"):
                report["collector"]["status"] = raw["status"]
            if report["collector"].get("rotations", 0) >= MAX_FILES:
                report["interpretation"].append("Ring rotation removed older capture files; this report covers retained records only.")
        except (ValueError, OSError, AttributeError, TypeError):
            report["input_problems"]["invalid_metadata"] = 1
    return report


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("capture", type=Path)
    parser.add_argument("--output", type=Path, help="New JSON report file; existing files are never replaced")
    args = parser.parse_args(argv)
    try:
        report = analyze(args.capture)
        if args.output:
            with args.output.open("x", encoding="utf-8") as output:
                json.dump(report, output, indent=2, ensure_ascii=True)
                output.write("\n")
        else:
            print(json.dumps(report, indent=2, ensure_ascii=True))
        print(f"Complete snapshots: {report['complete_snapshots']}; segments: {len(report['segments'])}. Missing telemetry remains unknown.", file=sys.stderr)
        return 0 if report["segments"] else 3
    except (ValueError, OSError, TypeError, RecursionError):
        print("Analysis stopped: invalid or excessive input, or output already exists. Raw input is not printed.", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
