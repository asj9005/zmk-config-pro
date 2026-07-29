#!/usr/bin/env python3
# Copyright (c) 2026 asj9005
# SPDX-License-Identifier: MIT
#
"""Analyze USBPcap/TShark CSV interrupt-IN timing and HID payload repetition.

This tool intentionally calls payload changes "transitions", not fresh radio
events. USB HID reports do not contain the ESB source sequence, so a USB capture
alone cannot prove that every report came from a new peripheral event.
"""

from __future__ import annotations

import argparse
import csv
import json
import math
import re
import sys
from collections import defaultdict
from pathlib import Path
from typing import Iterable, Optional, TextIO


TIME_COLUMNS = (
    "frame.time_epoch",
    "frame.time_relative",
    "timestamp",
    "time_s",
    "time",
)
FRAME_COLUMNS = ("frame.number", "frame", "number")
BUS_COLUMNS = ("usb.bus_id", "bus")
DEVICE_COLUMNS = ("usb.device_address", "device", "address")
ENDPOINT_COLUMNS = ("usb.endpoint_address", "endpoint", "ep")
URB_TYPE_COLUMNS = ("usb.urb_type", "urb_type")
PAYLOAD_COLUMNS = (
    "usbhid.data",
    "usb.capdata",
    "usb.data_fragment",
    "payload",
    "data",
)


def percentile(values: list[float], percent: float) -> Optional[float]:
    if not values:
        return None
    ordered = sorted(values)
    if len(ordered) == 1:
        return ordered[0]
    rank = (len(ordered) - 1) * percent / 100.0
    low = math.floor(rank)
    high = math.ceil(rank)
    if low == high:
        return ordered[low]
    fraction = rank - low
    return ordered[low] * (1.0 - fraction) + ordered[high] * fraction


def interval_summary(timestamps: list[float]) -> dict[str, object]:
    intervals_ms: list[float] = []
    non_monotonic = 0
    for previous, current in zip(timestamps, timestamps[1:]):
        delta = round((current - previous) * 1000.0, 9)
        if delta >= 0:
            intervals_ms.append(delta)
        else:
            non_monotonic += 1

    result: dict[str, object] = {
        "interval_count": len(intervals_ms),
        "non_monotonic_timestamps": non_monotonic,
        "min_ms": None,
        "typical_ms": None,
        "mean_ms": None,
        "p95_ms": None,
        "p99_ms": None,
        "max_ms": None,
        "observed_completion_rate_hz": None,
    }
    if not intervals_ms:
        return result

    result.update(
        {
            "min_ms": min(intervals_ms),
            "typical_ms": percentile(intervals_ms, 50),
            "mean_ms": sum(intervals_ms) / len(intervals_ms),
            "p95_ms": percentile(intervals_ms, 95),
            "p99_ms": percentile(intervals_ms, 99),
            "max_ms": max(intervals_ms),
        }
    )
    elapsed_seconds = sum(intervals_ms) / 1000.0
    if elapsed_seconds:
        result["observed_completion_rate_hz"] = len(intervals_ms) / elapsed_seconds
    return result


def canonical_headers(fieldnames: Iterable[str]) -> dict[str, str]:
    return {name.strip().lstrip("\ufeff").casefold(): name for name in fieldnames if name}


def choose_column(
    headers: dict[str, str],
    explicit: Optional[str],
    candidates: Iterable[str],
    required: bool,
) -> Optional[str]:
    if explicit:
        key = explicit.strip().casefold()
        if key not in headers:
            raise ValueError(f"CSV column not found: {explicit}")
        return headers[key]
    for candidate in candidates:
        if candidate.casefold() in headers:
            return headers[candidate.casefold()]
    if required:
        raise ValueError(f"required CSV column not found; tried: {', '.join(candidates)}")
    return None


def parse_numeric(value: str) -> Optional[int]:
    text = value.strip()
    if not text:
        return None
    try:
        return int(text, 0)
    except ValueError:
        if re.fullmatch(r"[0-9A-Fa-f]+", text) and any(
            character in "abcdefABCDEF" for character in text
        ):
            return int(text, 16)
    return None


def normalize_payload(value: str) -> str:
    text = value.strip().lower()
    if not text:
        return ""
    if text.startswith("0x"):
        text = text[2:]
    return re.sub(r"[^0-9a-f]", "", text)


def is_complete_urb(value: str) -> bool:
    text = value.strip().strip("'\"").upper()
    if not text:
        return True
    if text in {"C", "COMPLETE", "URB_COMPLETE", "0X43", "67"}:
        return True
    numeric = parse_numeric(text)
    return numeric == 0x43


def matches_numeric_filter(value: str, expected: Optional[int]) -> bool:
    if expected is None:
        return True
    parsed = parse_numeric(value)
    return parsed == expected


def stream_label(bus: str, device: str, endpoint: str) -> str:
    return f"bus={bus or '?'} device={device or '?'} endpoint={endpoint or '?'}"


def analyze_csv(
    stream: TextIO,
    *,
    time_column: Optional[str],
    payload_column: Optional[str],
    endpoint_filter: Optional[int],
    bus_filter: Optional[int],
    device_filter: Optional[int],
    report_id: Optional[int],
    include_all_urbs: bool,
) -> dict[str, object]:
    reader = csv.DictReader(stream)
    if not reader.fieldnames:
        raise ValueError("CSV has no header row")

    headers = canonical_headers(reader.fieldnames)
    time_col = choose_column(headers, time_column, TIME_COLUMNS, True)
    frame_col = choose_column(headers, None, FRAME_COLUMNS, False)
    bus_col = choose_column(headers, None, BUS_COLUMNS, False)
    device_col = choose_column(headers, None, DEVICE_COLUMNS, False)
    endpoint_col = choose_column(headers, None, ENDPOINT_COLUMNS, False)
    urb_type_col = choose_column(headers, None, URB_TYPE_COLUMNS, False)

    if payload_column:
        payload_cols = [
            choose_column(headers, payload_column, (), True),
        ]
    else:
        payload_cols = [
            headers[candidate.casefold()]
            for candidate in PAYLOAD_COLUMNS
            if candidate.casefold() in headers
        ]
        if not payload_cols:
            raise ValueError(
                "no HID payload column found; export usbhid.data and usb.capdata "
                "or pass --payload-column"
            )

    streams: dict[tuple[str, str, str], list[dict[str, object]]] = defaultdict(list)
    row_count = 0
    skipped = defaultdict(int)

    for row_count, row in enumerate(reader, 1):
        urb_type = row.get(urb_type_col, "") if urb_type_col else ""
        if urb_type_col and not include_all_urbs and not is_complete_urb(urb_type):
            skipped["non_complete_urb"] += 1
            continue

        bus = row.get(bus_col, "").strip() if bus_col else ""
        device = row.get(device_col, "").strip() if device_col else ""
        endpoint = row.get(endpoint_col, "").strip() if endpoint_col else ""
        if not matches_numeric_filter(bus, bus_filter):
            skipped["bus_filter"] += 1
            continue
        if not matches_numeric_filter(device, device_filter):
            skipped["device_filter"] += 1
            continue
        if not matches_numeric_filter(endpoint, endpoint_filter):
            skipped["endpoint_filter"] += 1
            continue

        raw_time = row.get(time_col, "").strip()
        try:
            timestamp = float(raw_time)
        except ValueError:
            skipped["invalid_timestamp"] += 1
            continue

        payload = ""
        for column in payload_cols:
            payload = normalize_payload(row.get(column, ""))
            if payload:
                break
        if not payload:
            skipped["empty_payload"] += 1
            continue
        if len(payload) % 2:
            skipped["odd_length_payload"] += 1
            continue

        payload_bytes = bytes.fromhex(payload)
        if report_id is not None and (
            not payload_bytes or payload_bytes[0] != report_id
        ):
            skipped["report_id_filter"] += 1
            continue

        key = (bus, device, endpoint)
        streams[key].append(
            {
                "timestamp": timestamp,
                "payload": payload,
                "frame": row.get(frame_col, "").strip() if frame_col else "",
            }
        )

    stream_results: dict[str, object] = {}
    total_reports = 0
    total_repeats = 0
    total_transitions = 0

    for key, reports in sorted(streams.items()):
        timestamps = [float(report["timestamp"]) for report in reports]
        payloads = [str(report["payload"]) for report in reports]
        transition_timestamps: list[float] = []
        repeated = 0
        previous: Optional[str] = None
        for timestamp, payload in zip(timestamps, payloads):
            if previous is None or payload != previous:
                transition_timestamps.append(timestamp)
            else:
                repeated += 1
            previous = payload

        total_reports += len(reports)
        total_repeats += repeated
        total_transitions += len(transition_timestamps)
        label = stream_label(*key)
        stream_results[label] = {
            "report_completions_with_payload": len(reports),
            "consecutive_repeated_payloads": repeated,
            "payload_transitions_including_first": len(transition_timestamps),
            "repeat_ratio": repeated / len(reports) if reports else None,
            "all_completion_intervals": interval_summary(timestamps),
            "payload_transition_intervals": interval_summary(transition_timestamps),
            "first_frame": reports[0]["frame"] if reports else None,
            "last_frame": reports[-1]["frame"] if reports else None,
        }

    return {
        "input": {
            "csv_rows": row_count,
            "time_column": time_col,
            "payload_columns_in_priority_order": payload_cols,
            "urb_type_column": urb_type_col,
            "complete_urb_filter_applied": bool(urb_type_col and not include_all_urbs),
            "report_id_filter": report_id,
            "skipped_rows": dict(sorted(skipped.items())),
        },
        "totals": {
            "streams": len(stream_results),
            "report_completions_with_payload": total_reports,
            "consecutive_repeated_payloads": total_repeats,
            "payload_transitions_including_first": total_transitions,
        },
        "streams": stream_results,
        "interpretation": {
            "typical_definition": "median (p50)",
            "completion_warning": (
                "USBPcap captures host URBs, not every raw USB bus transaction"
            ),
            "freshness_warning": (
                "a changed HID payload is a USB state transition, not proof of a "
                "fresh ESB event; HID carries no ESB sequence"
            ),
        },
    }


def format_number(value: object, digits: int = 3) -> str:
    if value is None:
        return "n/a"
    if isinstance(value, float):
        return f"{value:.{digits}f}"
    return str(value)


def format_interval(summary: dict[str, object]) -> str:
    if summary["interval_count"] == 0:
        return "intervals=0"
    return (
        f"intervals={summary['interval_count']} "
        f"typical={format_number(summary['typical_ms'])} ms "
        f"p95={format_number(summary['p95_ms'])} ms "
        f"p99={format_number(summary['p99_ms'])} ms "
        f"max={format_number(summary['max_ms'])} ms "
        f"rate={format_number(summary['observed_completion_rate_hz'])} Hz"
    )


def print_text(result: dict[str, object]) -> None:
    input_info = result["input"]
    totals = result["totals"]
    print("USBPcap/TShark HID analysis")
    print(
        f"csv_rows={input_info['csv_rows']} streams={totals['streams']} "
        f"reports={totals['report_completions_with_payload']} "
        f"repeats={totals['consecutive_repeated_payloads']} "
        f"transitions={totals['payload_transitions_including_first']}"
    )
    print(f"skipped={input_info['skipped_rows']}")
    for label, data in result["streams"].items():
        print()
        print(label)
        print(
            "  reports: "
            f"{data['report_completions_with_payload']} "
            f"repeated={data['consecutive_repeated_payloads']} "
            f"transitions={data['payload_transitions_including_first']} "
            f"repeat_ratio={format_number(data['repeat_ratio'])}"
        )
        print(
            "  completion cadence: "
            + format_interval(data["all_completion_intervals"])
        )
        print(
            "  payload-transition cadence: "
            + format_interval(data["payload_transition_intervals"])
        )
    print()
    print(
        "NOTE: USBPcap observes URB completions, not every raw bus poll. A payload "
        "transition is not proof of a fresh ESB event."
    )


def parse_cli_int(value: str) -> int:
    try:
        parsed = int(value, 0)
    except ValueError as exc:
        raise argparse.ArgumentTypeError(str(exc)) from exc
    if parsed < 0:
        raise argparse.ArgumentTypeError("value must be non-negative")
    return parsed


def open_input(path: str) -> tuple[TextIO, bool]:
    if path == "-":
        return sys.stdin, False
    return Path(path).open("r", encoding="utf-8-sig", newline=""), True


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description=(
            "Analyze interrupt-IN HID reports exported as CSV by TShark. "
            "The CSV must include a timestamp and HID/raw payload column."
        )
    )
    parser.add_argument("csv", help="TShark CSV file, or - for stdin")
    parser.add_argument("--time-column", help="override timestamp column name")
    parser.add_argument("--payload-column", help="override HID payload column name")
    parser.add_argument("--bus", type=parse_cli_int, help="keep one usb.bus_id")
    parser.add_argument(
        "--device", type=parse_cli_int, help="keep one usb.device_address"
    )
    parser.add_argument(
        "--endpoint",
        type=parse_cli_int,
        help="keep one endpoint address, for example 0x81",
    )
    parser.add_argument(
        "--report-id",
        type=parse_cli_int,
        help="keep payloads whose first byte is this HID report ID",
    )
    parser.add_argument(
        "--include-all-urbs",
        action="store_true",
        help="do not discard non-complete URBs when usb.urb_type is present",
    )
    parser.add_argument(
        "--json",
        action="store_true",
        help="emit machine-readable JSON instead of text",
    )
    return parser


def main(argv: Optional[list[str]] = None) -> int:
    args = build_parser().parse_args(argv)
    stream, should_close = open_input(args.csv)
    try:
        try:
            result = analyze_csv(
                stream,
                time_column=args.time_column,
                payload_column=args.payload_column,
                endpoint_filter=args.endpoint,
                bus_filter=args.bus,
                device_filter=args.device,
                report_id=args.report_id,
                include_all_urbs=args.include_all_urbs,
            )
        except ValueError as exc:
            print(f"error: {exc}", file=sys.stderr)
            return 2
    finally:
        if should_close:
            stream.close()

    if args.json:
        json.dump(result, sys.stdout, ensure_ascii=False, indent=2, sort_keys=True)
        print()
    else:
        print_text(result)

    if result["totals"]["report_completions_with_payload"] == 0:
        print("error: no matching HID payload records found", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
