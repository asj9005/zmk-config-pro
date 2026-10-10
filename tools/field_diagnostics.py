#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Shared, privacy-restricted format and storage for Totem field diagnostics."""
from __future__ import annotations

from collections import Counter
from datetime import datetime, timezone
import json
import os
from pathlib import Path
import re
import time

FORMAT_VERSION = 1
MAX_LINE_BYTES = 4096
MAX_JSON_BYTES = 32768
MAX_FILES = 16
FILE_BYTES = 8 * 1024 * 1024
MAX_INPUT_BYTES = 256 * 1024 * 1024
MAX_RECORDS = 500_000
MARKERS = frozenset(("left_disconnect", "right_disconnect", "both_disconnect",
                     "key_repeat", "mouse_stuck", "input_delay", "other"))
ROLES = frozenset(("dongle", "left", "right"))
UINT32 = (1 << 32) - 1
UINT64 = (1 << 64) - 1
METRICS = ("rx_queue", "rx_process", "hold_tap_base_e", "hold_tap_base_r",
           "hold_tap_mouse_fast", "hold_tap_mouse_slow", "hold_tap_other",
           "timer_late", "usb_queue", "usb_queue_recovery", "usb_queue_resync", "usb_transfer")
ISSUES = ("peer_timeout", "auth_restart", "session_established", "handshake_timeout",
          "tx_queue_full", "presession_overflow", "rx_overflow", "scan_overflow",
          "usb_overflow", "usb_retry", "input_overflow", "hold_tap_overflow",
          "usb_reset", "usb_suspend", "usb_resume", "usb_timing_discard")
HOLDTAPS = ("base_e", "base_r", "mouse_fast", "mouse_slow", "other")
BIN_UPPER_US = (100, 250, 500, 1000, 2000, 5000, 10000, 20000, 50000, 100000,
                150000, 180000, 200000, 220000, 250000, 500000, 1000000, None)
TIMESTAMP_RE = re.compile(r"\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}\.\d{3}Z")
DIAG_FIELDS = {
    "startup": ("crypto_init", "crypto_kat", "crypto_roots", "boot_nonce", "clock", "radio",
                "transport", "kat_step", "kat_status"),
    "events": ("tx_ok", "tx_fail", "rx", "frame_ok", "frame_err", "hello", "challenge", "ready",
               "session_ok", "rx_drop", "bad_position", "ht_overflow", "scan_overflow", "scan_resync",
               "usb_retry", "usb_overflow", "input_retry", "input_overflow", "last_frame_err"),
    "tx": tuple(name + suffix for name in ("send", "write", "start", "hf_request", "hf_callback", "hf_wait", "radio_busy")
                for suffix in ("_n", "_rc")),
    "radio": ("available", "sdk_state", "radio_state", "tx_queued", "retries", "irq_flags",
              "radio_events", "timer_events", "timer_shorts", "radio_irq", "timer_irq", "late_ack"),
    "high": ("rx_high", "rx_age_max", "scan_high", "usb_high"),
}
COMMON = {"schema", "role", "boot", "seq", "part", "total", "kind"}


def parse_wire(raw: bytes):
    """Return canonical numeric/enum fields only; no raw console bytes survive."""
    if len(raw) > MAX_LINE_BYTES or not raw.startswith(b"[totem-field] "):
        return None
    try:
        text = raw.decode("ascii")
    except UnicodeError:
        return None
    tokens = text.split(" ")
    fields = {}
    for token in tokens[1:]:
        if not re.fullmatch(r"[a-z][a-z0-9_]*=[A-Za-z0-9_-]+", token):
            return None
        key, value = token.split("=")
        if key in fields:
            return None
        fields[key] = value
    return validate_part(fields, from_wire=True)


def validate_part(fields: dict, from_wire=False):
    if not isinstance(fields, dict) or len(fields) > 36:
        return None
    value = dict(fields)
    enum_names = {"role", "boot", "kind", "fw", "tree", "name", "format"}
    for key, item in value.items():
        if key in enum_names:
            if not isinstance(item, str):
                return None
        else:
            if from_wire:
                if not isinstance(item, str) or not re.fullmatch(r"-?(?:0|[1-9][0-9]{0,19})", item):
                    return None
                value[key] = int(item)
            elif type(item) is not int:
                return None
    common = COMMON
    if not common <= value.keys() or value["schema"] != 1 or value["total"] != 72:
        return None
    if value["role"] not in ROLES or not isinstance(value["boot"], str) or not re.fullmatch(r"[0-9a-f]{16}", value["boot"]):
        return None
    if not integer(value["seq"], UINT32) or not integer(value["part"], 71):
        return None
    kind = value["kind"]
    extras, signed, wide = set(), set(), set()
    if kind == "begin":
        extras = {"format", "fw", "tree", "dirty", "uptime_ms", "tick_hz", "rx_queue_resolution_us", "tx_drop", "interval_ms"}
        if value["part"] != 0 or value.get("dirty") not in (0, 1, 2):
            return None
        if value.get("format") != "TOTEM_FIELD_V1":
            return None
        for name in ("fw", "tree"):
            if not isinstance(value.get(name), str) or not re.fullmatch(r"(?:[0-9a-f]{40}|unknown)", value[name]):
                return None
        wide = {"uptime_ms"}
    elif kind == "metric":
        extras = {"name", "count", "sum_us", "max_us", "flags"} | {f"b{i}" for i in range(len(BIN_UPPER_US))}
        if value.get("name") not in METRICS or value["part"] != 1 + METRICS.index(value["name"]):
            return None
        wide = {"sum_us"}
        if not integer(value.get("flags"), 7):
            return None
    elif kind == "issue":
        extras = {"name", "scope", "count", "last_ms", "code"}
        if value.get("name") not in ISSUES or value.get("scope") not in (0, 1, 2):
            return None
        if value["part"] != 13 + ISSUES.index(value["name"]) * 3 + value["scope"]:
            return None
        wide, signed = {"last_ms"}, {"code"}
    elif kind == "holdtap":
        extras = {"name", "tap", "hold"}
        if value.get("name") not in HOLDTAPS or value["part"] != 61 + HOLDTAPS.index(value["name"]):
            return None
    elif kind == "diag":
        if value.get("name") not in DIAG_FIELDS or value["part"] != 66 + tuple(DIAG_FIELDS).index(value["name"]):
            return None
        extras = {"name"} | set(DIAG_FIELDS[value["name"]])
        if value["name"] == "startup":
            signed = set(DIAG_FIELDS["startup"])
        else:
            signed = {key for key in extras if key.endswith("_rc") or key == "last_frame_err"}
        if value["name"] == "radio" and value.get("available") not in (0, 1):
            return None
    elif kind == "end":
        extras, wide = {"uptime_ms", "tx_drop"}, {"uptime_ms"}
        if value["part"] != 71:
            return None
    else:
        return None  # Remaining schema kinds are explicitly registered below.
    if set(value) != common | extras:
        return None
    for key, item in value.items():
        if key in enum_names:
            continue
        if key in signed:
            if not -(1 << 31) <= item < (1 << 31):
                return None
        elif not integer(item, UINT64 if key in wide else UINT32):
            return None
    if kind == "metric":
        bins = [value[f"b{i}"] for i in range(len(BIN_UPPER_US))]
        if sum(bins) != value["count"]:
            return None
        if not value["flags"]:
            if not value["count"]:
                if value["sum_us"] or value["max_us"]:
                    return None
            else:
                maximum_bin = next((i for i, upper in enumerate(BIN_UPPER_US)
                                    if upper is None or value["max_us"] <= upper), None)
                if maximum_bin != max(i for i, count in enumerate(bins) if count):
                    return None
                minimum_sum = sum(count * (0 if i == 0 else BIN_UPPER_US[i - 1] + 1)
                                  for i, count in enumerate(bins))
                maximum_sum = sum(count * min(value["max_us"], upper if upper is not None else value["max_us"])
                                  for count, upper in zip(bins, BIN_UPPER_US))
                if not max(minimum_sum, value["max_us"]) <= value["sum_us"] <= maximum_sum:
                    return None
    return value


class SnapshotAssembler:
    """Bounded frame assembly; only complete schema-validated frames escape."""
    def __init__(self):
        self.pending = {}
        self.stats = Counter()

    def add(self, part):
        key = (part["role"], part["boot"], part["seq"])
        if key not in self.pending:
            if len(self.pending) >= 2:
                self.pending.pop(next(iter(self.pending)))
                self.stats["incomplete_frames"] += 1
            self.pending[key] = {}
        frame = self.pending[key]
        if part["part"] in frame:
            self.stats["duplicate_parts"] += 1
            if frame[part["part"]] != part:
                self.stats["conflicting_parts"] += 1
                self.pending.pop(key)
            return None
        frame[part["part"]] = part
        if len(frame) != 72:
            return None
        self.pending.pop(key)
        begin, end = frame[0], frame[71]
        if end["uptime_ms"] < begin["uptime_ms"] or end["tx_drop"] != begin["tx_drop"]:
            self.stats["inconsistent_frames"] += 1
            return None
        # Every row has fixed index/name semantics; removing its common framing
        # yields a ~10 KiB record instead of 72 repeated serial/PC envelopes.
        result = {key: part[key] for key in ("schema", "role", "boot", "seq")}
        result["rows"] = [{key: value for key, value in frame[index].items() if key not in COMMON}
                          for index in range(72)]
        self.stats["complete_frames"] += 1
        return result

    def finish(self):
        self.stats["incomplete_frames"] += len(self.pending)
        self.pending.clear()


def expand_snapshot(value):
    """Validate compact stored snapshots as strictly as the original wire."""
    if not isinstance(value, dict) or set(value) != {"schema", "role", "boot", "seq", "rows"}:
        return None
    rows = value["rows"]
    if not isinstance(rows, list) or len(rows) != 72:
        return None
    base = {key: value[key] for key in ("schema", "role", "boot", "seq")}
    result = []
    for index, row in enumerate(rows):
        if not isinstance(row, dict) or COMMON.intersection(row):
            return None
        kind = ("begin" if index == 0 else "metric" if index <= 12 else
                "issue" if index <= 60 else "holdtap" if index <= 65 else
                "diag" if index <= 70 else "end")
        parsed = validate_part(dict(base, part=index, total=72, kind=kind, **row))
        if parsed is None:
            return None
        result.append(parsed)
    if result[71]["uptime_ms"] < result[0]["uptime_ms"] or result[71]["tx_drop"] != result[0]["tx_drop"]:
        return None
    return result


def utc_now() -> str:
    return datetime.now(timezone.utc).isoformat(timespec="milliseconds").replace("+00:00", "Z")


def pc_stamp() -> dict:
    # Receipt clocks describe capture coverage, never firmware/USB latency.
    return {"pc_utc": utc_now(), "pc_monotonic_ns": time.monotonic_ns()}


def integer(value, maximum=UINT64) -> bool:
    return type(value) is int and 0 <= value <= maximum


class LineBuffer:
    """Bound memory and discard the entire oversized line, never its suffix."""
    def __init__(self):
        self.pending = bytearray()
        self.dropping = False
        self.oversized = 0

    def feed(self, chunk: bytes):
        lines = []
        for byte in chunk:
            if byte == 10:
                if not self.dropping:
                    lines.append(bytes(self.pending).rstrip(b"\r"))
                self.pending.clear()
                self.dropping = False
            elif not self.dropping:
                if len(self.pending) >= MAX_LINE_BYTES:
                    self.pending.clear()
                    self.dropping = True
                    self.oversized += 1
                else:
                    self.pending.append(byte)
        return lines


class RingWriter:
    """Only rotate files created by this writer in its exclusive new directory."""
    def __init__(self, directory: Path, file_bytes=FILE_BYTES, max_files=MAX_FILES):
        if not 1 <= max_files <= MAX_FILES or not 256 <= file_bytes <= FILE_BYTES:
            raise ValueError("invalid rotation bounds")
        self.directory = directory.resolve()
        self.directory.mkdir(parents=True, exist_ok=False)
        self.limit, self.max_files = file_bytes, max_files
        self.owned = set()
        self.handle = None
        self.index = self.bytes = self.total_records = self.rotations = 0
        self._open()

    def _open(self):
        target = self.directory / f"capture-{self.index:02d}.jsonl"
        if target.is_symlink() or target.parent.resolve() != self.directory:
            raise ValueError("unsafe rotation path")
        if target.exists():
            if target not in self.owned or not target.is_file():
                raise ValueError("unowned rotation target")
            target.unlink()  # Fixed, closed, collector-owned ring slot only.
        self.handle = target.open("xb")
        self.owned.add(target)
        self.bytes = 0

    def write(self, record: dict):
        record = dict(record, record_index=self.total_records)
        data = (json.dumps(record, separators=(",", ":"), ensure_ascii=True) + "\n").encode("ascii")
        if len(data) > min(MAX_JSON_BYTES, self.limit):
            raise ValueError("record too large")
        if self.bytes + len(data) > self.limit:
            self.handle.close()
            self.index = (self.index + 1) % self.max_files
            self.rotations += 1
            self._open()
        self.handle.write(data)
        self.handle.flush()
        self.bytes += len(data)
        self.total_records += 1

    def close(self):
        if self.handle is not None:
            self.handle.flush()
            self.handle.close()


def write_metadata(directory: Path, data: dict):
    # Only the collector's two fixed metadata files are replaced.
    target, temporary = directory / "capture-meta.json", directory / "capture-meta.tmp"
    if target.is_symlink() or temporary.is_symlink():
        raise ValueError("unsafe metadata path")
    with temporary.open("w", encoding="utf-8") as output:
        json.dump(data, output, indent=2, ensure_ascii=True)
        output.write("\n")
    os.replace(temporary, target)


def load_json_line(raw: bytes):
    if len(raw) > MAX_JSON_BYTES:
        raise ValueError("line too large")
    try:
        # Duplicate object keys are invalid, rather than silently last-wins.
        def unique(pairs):
            value = {}
            for key, item in pairs:
                if key in value:
                    raise ValueError("duplicate JSON key")
                value[key] = item
            return value
        value = json.loads(raw, object_pairs_hook=unique,
                           parse_constant=lambda _: (_ for _ in ()).throw(ValueError("nonfinite")))
    except (UnicodeError, json.JSONDecodeError, RecursionError) as error:
        raise ValueError("invalid JSON") from error
    if not isinstance(value, dict):
        raise ValueError("record must be object")
    pending = [(value, 0)]
    nodes = 0
    while pending:
        item, depth = pending.pop()
        nodes += 1
        if depth > 8 or nodes > 8192:
            raise ValueError("JSON structure exceeds cap")
        if isinstance(item, dict):
            pending.extend((child, depth + 1) for child in item.values())
        elif isinstance(item, list):
            pending.extend((child, depth + 1) for child in item)
    return value
