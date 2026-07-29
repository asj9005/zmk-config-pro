#!/usr/bin/env python3
# Copyright (c) 2026 asj9005
# SPDX-License-Identifier: MIT
#
"""Analyze Totem ESB benchmark logs without assuming synchronized clocks.

The firmware emits these machine-readable record families:

  BENCH_RX source=0 session=... seq=1 gap=0 source_tick=... dongle_tick=... wire=2 ...
  BENCH_RX_INVALID pipe=1 error=-22 dongle_tick=...
  BENCH_TX source=0 msg=1 attempts=1 retransmissions=0 success=1 source_tick=...
  BENCH_USB source=0 session=... seq=2 ... rx_tick=... queue_enter_tick=... queue_done_tick=...

Sequence integrity is calculated across every BENCH_RX packet because the wire
sequence is shared by ZMK, heartbeat, and synthetic benchmark packets. Cadence
is reported separately for accepted wire=2 packets, which are fresh synthetic
radio events. source_tick and dongle_tick are never subtracted from each other.
BENCH_USB timestamps are all captured on the dongle and can be compared, but
their FIFO correlation is only reliable for simple one-key/one-report tests.
"""

from __future__ import annotations

import argparse
import json
import math
import re
import sys
from collections import Counter, defaultdict
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable, Optional, TextIO


UINT32_MASK = (1 << 32) - 1
UINT32_HALF = 1 << 31

WIRE_NAMES = {
    0: "zmk",
    1: "heartbeat",
    2: "benchmark",
}

EVENT_NAMES = {
    0: "key_position",
    1: "sensor",
    2: "input",
    3: "battery",
}

LINK_METRIC_NAMES = {
    0: "tx_messages",
    1: "tx_attempts",
    2: "tx_failures",
    3: "app_queue_pressure",
    4: "producer_queue_overflow",
}

KV_RE = re.compile(r"([A-Za-z_][A-Za-z0-9_]*)=(-?(?:0[xX][0-9A-Fa-f]+|\d+))")


@dataclass(frozen=True)
class RxRecord:
    line_no: int
    source: int
    session_id: int
    sequence: int
    firmware_gap: int
    source_tick: int
    dongle_tick: int
    wire: int
    event: int
    position: int
    pressed: int


@dataclass(frozen=True)
class TxRecord:
    line_no: int
    source: int
    message_id: int
    attempts: int
    success: bool
    source_tick: int


@dataclass(frozen=True)
class UsbRecord:
    line_no: int
    source: int
    session_id: int
    sequence: int
    position: int
    pressed: int
    rx_tick: int
    queue_enter_tick: int
    queue_done_tick: int
    result: int


@dataclass(frozen=True)
class UsbUnmatchedRecord:
    line_no: int
    queue_enter_tick: int
    queue_done_tick: int
    result: int


def parse_int(value: str) -> int:
    return int(value, 0)


def parse_fields(line: str) -> dict[str, int]:
    return {key: parse_int(value) for key, value in KV_RE.findall(line)}


def percentile(values: list[float], percent: float) -> Optional[float]:
    """Return a linearly interpolated percentile, or None for no values."""
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


def modular_intervals(ticks: list[int]) -> tuple[list[int], int]:
    """Return forward uint32 deltas and the number of backward/reset deltas."""
    intervals: list[int] = []
    rejected = 0
    for previous, current in zip(ticks, ticks[1:]):
        delta = (current - previous) & UINT32_MASK
        if delta < UINT32_HALF:
            intervals.append(delta)
        else:
            rejected += 1
    return intervals, rejected


def interval_summary(intervals: list[int], clock_hz: Optional[float]) -> dict[str, object]:
    summary: dict[str, object] = {
        "interval_count": len(intervals),
        "rejected_backward_or_reset": 0,
    }
    if not intervals:
        summary.update(
            {
                "min_cycles": None,
                "typical_cycles": None,
                "mean_cycles": None,
                "p95_cycles": None,
                "p99_cycles": None,
                "max_cycles": None,
            }
        )
        if clock_hz:
            summary.update(
                {
                    "min_ms": None,
                    "typical_ms": None,
                    "mean_ms": None,
                    "p95_ms": None,
                    "p99_ms": None,
                    "max_ms": None,
                    "observed_rate_hz": None,
                }
            )
        return summary

    values = [float(value) for value in intervals]
    cycle_values = {
        "min_cycles": min(values),
        "typical_cycles": percentile(values, 50),
        "mean_cycles": sum(values) / len(values),
        "p95_cycles": percentile(values, 95),
        "p99_cycles": percentile(values, 99),
        "max_cycles": max(values),
    }
    summary.update(cycle_values)

    if clock_hz:
        scale = 1000.0 / clock_hz
        for name in ("min", "typical", "mean", "p95", "p99", "max"):
            summary[f"{name}_ms"] = cycle_values[f"{name}_cycles"] * scale
        elapsed_cycles = sum(intervals)
        summary["observed_rate_hz"] = (
            len(intervals) * clock_hz / elapsed_cycles if elapsed_cycles else None
        )
    return summary


def modular_durations(pairs: Iterable[tuple[int, int]]) -> tuple[list[int], int]:
    durations: list[int] = []
    rejected = 0
    for start, end in pairs:
        delta = (end - start) & UINT32_MASK
        if delta < UINT32_HALF:
            durations.append(delta)
        else:
            rejected += 1
    return durations, rejected


def duration_summary(
    pairs: Iterable[tuple[int, int]], clock_hz: Optional[float]
) -> dict[str, object]:
    durations, rejected = modular_durations(pairs)
    summary = interval_summary(durations, clock_hz)
    summary["sample_count"] = summary.pop("interval_count")
    summary["rejected_backward_or_reset"] = rejected
    summary.pop("observed_rate_hz", None)
    return summary


def classify_sequence(records: list[RxRecord]) -> tuple[list[RxRecord], dict[str, int]]:
    """Mirror the central firmware's uint32 sequence acceptance rules."""
    accepted: list[RxRecord] = []
    last_sequence: Optional[int] = None
    last_session: Optional[int] = None
    metrics = {
        "received": 0,
        "accepted_fresh": 0,
        "duplicates": 0,
        "out_of_order": 0,
        "missing_sequences": 0,
        "gap_records": 0,
        "firmware_gap_sum": 0,
        "gap_mismatches": 0,
        "session_changes": 0,
    }

    for record in records:
        metrics["received"] += 1
        metrics["firmware_gap_sum"] += record.firmware_gap
        computed_gap = 0

        if last_session is None or record.session_id != last_session:
            if last_session is not None:
                metrics["session_changes"] += 1
            last_session = record.session_id
            last_sequence = record.sequence
            accept = True
        else:
            delta = (record.sequence - last_sequence) & UINT32_MASK
            if delta == 0:
                metrics["duplicates"] += 1
                accept = False
            elif delta < UINT32_HALF:
                computed_gap = delta - 1
                last_sequence = record.sequence
                accept = True
            else:
                metrics["out_of_order"] += 1
                accept = False

        if record.firmware_gap != computed_gap:
            metrics["gap_mismatches"] += 1
        if computed_gap:
            metrics["missing_sequences"] += computed_gap
            metrics["gap_records"] += 1
        if accept:
            metrics["accepted_fresh"] += 1
            accepted.append(record)

    return accepted, metrics


def cadence_for(
    records: list[RxRecord],
    dongle_clock_hz: Optional[float],
    source_clock_hz: Optional[float],
) -> dict[str, object]:
    dongle_intervals: list[int] = []
    source_intervals: list[int] = []
    dongle_rejected = 0
    source_rejected = 0
    for previous, current in zip(records, records[1:]):
        if previous.session_id != current.session_id:
            continue
        dongle_delta = (current.dongle_tick - previous.dongle_tick) & UINT32_MASK
        source_delta = (current.source_tick - previous.source_tick) & UINT32_MASK
        if dongle_delta < UINT32_HALF:
            dongle_intervals.append(dongle_delta)
        else:
            dongle_rejected += 1
        if source_delta < UINT32_HALF:
            source_intervals.append(source_delta)
        else:
            source_rejected += 1
    dongle = interval_summary(dongle_intervals, dongle_clock_hz)
    source = interval_summary(source_intervals, source_clock_hz)
    dongle["rejected_backward_or_reset"] = dongle_rejected
    source["rejected_backward_or_reset"] = source_rejected
    return {
        "event_count": len(records),
        "dongle_rx_intervals": dongle,
        "source_generation_intervals": source,
    }


def analyze(
    lines: Iterable[str],
    dongle_clock_hz: Optional[float],
    source_clock_hz: Optional[float],
) -> dict[str, object]:
    rx_records: list[RxRecord] = []
    tx_records: list[TxRecord] = []
    usb_records: list[UsbRecord] = []
    usb_unmatched_records: list[UsbUnmatchedRecord] = []
    usb_queue_overflow: list[tuple[int, int]] = []
    invalid_errors: Counter[int] = Counter()
    invalid_pipes: Counter[int] = Counter()
    rx_overflow_latest: dict[int, int] = {}
    link_metric_latest: dict[tuple[int, int], tuple[int, int]] = {}
    diagnostics: Counter[str] = Counter()
    malformed_markers = 0
    line_count = 0

    required_rx = {
        "source",
        "seq",
        "gap",
        "source_tick",
        "dongle_tick",
        "wire",
        "event",
        "position",
        "pressed",
    }
    required_tx = {"source", "msg", "attempts", "success", "source_tick"}
    required_usb = {
        "source",
        "seq",
        "position",
        "pressed",
        "rx_tick",
        "queue_enter_tick",
        "queue_done_tick",
        "result",
    }

    for line_count, line in enumerate(lines, 1):
        lower = line.lower()
        if "data corruption in received peripheral event" in lower:
            diagnostics["crc_failure_messages"] += 1
        if "prefix mismatch" in lower:
            diagnostics["prefix_mismatch_messages"] += 1
        if "no room to receive" in lower or "rx overrun" in lower:
            diagnostics["rx_overflow_messages"] += 1
        if (
            "msgq full" in lower
            or "no room to send event" in lower
            or "no room to send command" in lower
        ):
            diagnostics["tx_queue_full_messages"] += 1
        if "messages dropped" in lower or (
            "dropped" in lower and "message" in lower and "log" in lower
        ):
            diagnostics["logger_drop_messages"] += 1

        if "BENCH_RX_OVERFLOW" in line:
            fields = parse_fields(line)
            if {"pipe", "count"}.issubset(fields):
                rx_overflow_latest[fields["pipe"]] = max(
                    fields["count"], rx_overflow_latest.get(fields["pipe"], 0)
                )
            else:
                malformed_markers += 1
            continue

        if "BENCH_RX_INVALID" in line:
            fields = parse_fields(line)
            if {"pipe", "error"}.issubset(fields):
                invalid_pipes[fields["pipe"]] += 1
                invalid_errors[fields["error"]] += 1
            else:
                malformed_markers += 1
            continue

        if "BENCH_LINK" in line:
            fields = parse_fields(line)
            if {"source", "session", "metric", "value"}.issubset(fields):
                link_metric_latest[(fields["source"], fields["metric"])] = (
                    fields["session"] & UINT32_MASK,
                    fields["value"] & UINT32_MASK,
                )
            else:
                malformed_markers += 1
            continue

        if "BENCH_USB_QUEUE_OVERFLOW" in line:
            fields = parse_fields(line)
            if {"source", "seq"}.issubset(fields):
                usb_queue_overflow.append((fields["source"], fields["seq"]))
            else:
                malformed_markers += 1
            continue

        if "BENCH_USB_UNMATCHED" in line:
            fields = parse_fields(line)
            if {"queue_enter_tick", "queue_done_tick", "result"}.issubset(fields):
                usb_unmatched_records.append(
                    UsbUnmatchedRecord(
                        line_no=line_count,
                        queue_enter_tick=fields["queue_enter_tick"] & UINT32_MASK,
                        queue_done_tick=fields["queue_done_tick"] & UINT32_MASK,
                        result=fields["result"],
                    )
                )
            else:
                malformed_markers += 1
            continue

        if "BENCH_USB" in line:
            fields = parse_fields(line)
            if required_usb.issubset(fields):
                usb_records.append(
                    UsbRecord(
                        line_no=line_count,
                        source=fields["source"],
                        session_id=fields.get("session", 0) & UINT32_MASK,
                        sequence=fields["seq"] & UINT32_MASK,
                        position=fields["position"],
                        pressed=fields["pressed"],
                        rx_tick=fields["rx_tick"] & UINT32_MASK,
                        queue_enter_tick=fields["queue_enter_tick"] & UINT32_MASK,
                        queue_done_tick=fields["queue_done_tick"] & UINT32_MASK,
                        result=fields["result"],
                    )
                )
            else:
                malformed_markers += 1
            continue

        if "BENCH_RX" in line:
            fields = parse_fields(line)
            if required_rx.issubset(fields):
                rx_records.append(
                    RxRecord(
                        line_no=line_count,
                        source=fields["source"],
                        session_id=fields.get("session", 0) & UINT32_MASK,
                        sequence=fields["seq"] & UINT32_MASK,
                        firmware_gap=fields["gap"],
                        source_tick=fields["source_tick"] & UINT32_MASK,
                        dongle_tick=fields["dongle_tick"] & UINT32_MASK,
                        wire=fields["wire"],
                        event=fields["event"],
                        position=fields["position"],
                        pressed=fields["pressed"],
                    )
                )
            else:
                malformed_markers += 1
            continue

        if "BENCH_TX" in line:
            fields = parse_fields(line)
            if required_tx.issubset(fields):
                tx_records.append(
                    TxRecord(
                        line_no=line_count,
                        source=fields["source"],
                        message_id=fields["msg"],
                        attempts=fields["attempts"],
                        success=bool(fields["success"]),
                        source_tick=fields["source_tick"] & UINT32_MASK,
                    )
                )
            else:
                malformed_markers += 1

    by_source: dict[int, list[RxRecord]] = defaultdict(list)
    for record in rx_records:
        by_source[record.source].append(record)
    usb_by_source: dict[int, list[UsbRecord]] = defaultdict(list)
    for record in usb_records:
        usb_by_source[record.source].append(record)

    key_rx_by_identity: dict[tuple[int, int, int], list[RxRecord]] = defaultdict(list)
    for record in rx_records:
        if record.wire == 0 and record.event == 0:
            key_rx_by_identity[
                (record.source, record.session_id, record.sequence)
            ].append(record)

    source_results: dict[str, object] = {}
    total_missing = 0
    total_duplicates = 0
    total_out_of_order = 0

    for source, records in sorted(by_source.items()):
        accepted, sequence_metrics = classify_sequence(records)
        total_missing += sequence_metrics["missing_sequences"]
        total_duplicates += sequence_metrics["duplicates"]
        total_out_of_order += sequence_metrics["out_of_order"]

        wire_counts = Counter(record.wire for record in records)
        event_counts = Counter(
            record.event for record in records if record.wire == 0
        )
        accepted_benchmark = [record for record in accepted if record.wire == 2]
        accepted_keys = [
            record for record in accepted if record.wire == 0 and record.event == 0
        ]
        source_usb_records = usb_by_source.get(source, [])
        usb_identity_not_in_rx = 0
        usb_field_mismatches = 0
        for usb_record in source_usb_records:
            matches = key_rx_by_identity.get(
                (usb_record.source, usb_record.session_id, usb_record.sequence), []
            )
            if not matches:
                usb_identity_not_in_rx += 1
            elif not any(
                rx_record.position == usb_record.position
                and rx_record.pressed == usb_record.pressed
                for rx_record in matches
            ):
                usb_field_mismatches += 1

        source_results[str(source)] = {
            "logical_source": source,
            "expected_role": "left" if source == 0 else ("right" if source == 1 else "unknown"),
            "sequence": sequence_metrics,
            "wire_counts": {
                f"{wire}:{WIRE_NAMES.get(wire, 'unknown')}": count
                for wire, count in sorted(wire_counts.items())
            },
            "zmk_event_counts": {
                f"{event}:{EVENT_NAMES.get(event, 'unknown')}": count
                for event, count in sorted(event_counts.items())
            },
            "all_accepted_packet_cadence": cadence_for(
                accepted, dongle_clock_hz, source_clock_hz
            ),
            "benchmark_wire_2_cadence": cadence_for(
                accepted_benchmark, dongle_clock_hz, source_clock_hz
            ),
            "key_position_events": {
                "accepted_count": len(accepted_keys),
                "press_count": sum(record.pressed != 0 for record in accepted_keys),
                "release_count": sum(record.pressed == 0 for record in accepted_keys),
                "dongle_rx_cadence": cadence_for(
                    accepted_keys, dongle_clock_hz, source_clock_hz
                )["dongle_rx_intervals"],
            },
            "usb_queue_correlation": {
                "record_count": len(source_usb_records),
                "accepted_key_event_count": len(accepted_keys),
                "record_count_minus_accepted_key_events": (
                    len(source_usb_records) - len(accepted_keys)
                ),
                "result_codes": {
                    str(result): count
                    for result, count in sorted(
                        Counter(record.result for record in source_usb_records).items()
                    )
                },
                "identity_not_found_in_bench_rx": usb_identity_not_in_rx,
                "position_or_pressed_mismatches": usb_field_mismatches,
                "rx_to_queue_enter": duration_summary(
                    (
                        (record.rx_tick, record.queue_enter_tick)
                        for record in source_usb_records
                    ),
                    dongle_clock_hz,
                ),
                "queue_call_duration": duration_summary(
                    (
                        (record.queue_enter_tick, record.queue_done_tick)
                        for record in source_usb_records
                    ),
                    dongle_clock_hz,
                ),
                "rx_to_queue_done": duration_summary(
                    (
                        (record.rx_tick, record.queue_done_tick)
                        for record in source_usb_records
                    ),
                    dongle_clock_hz,
                ),
            },
        }

    for source, source_usb_records in sorted(usb_by_source.items()):
        if str(source) in source_results:
            continue
        source_results[str(source)] = {
            "logical_source": source,
            "expected_role": "unknown",
            "sequence": None,
            "wire_counts": {},
            "zmk_event_counts": {},
            "all_accepted_packet_cadence": None,
            "benchmark_wire_2_cadence": None,
            "key_position_events": None,
            "usb_queue_correlation": {
                "record_count": len(source_usb_records),
                "accepted_key_event_count": 0,
                "record_count_minus_accepted_key_events": len(source_usb_records),
                "result_codes": {
                    str(result): count
                    for result, count in sorted(
                        Counter(record.result for record in source_usb_records).items()
                    )
                },
                "identity_not_found_in_bench_rx": len(source_usb_records),
                "position_or_pressed_mismatches": 0,
                "rx_to_queue_enter": duration_summary(
                    (
                        (record.rx_tick, record.queue_enter_tick)
                        for record in source_usb_records
                    ),
                    dongle_clock_hz,
                ),
                "queue_call_duration": duration_summary(
                    (
                        (record.queue_enter_tick, record.queue_done_tick)
                        for record in source_usb_records
                    ),
                    dongle_clock_hz,
                ),
                "rx_to_queue_done": duration_summary(
                    (
                        (record.rx_tick, record.queue_done_tick)
                        for record in source_usb_records
                    ),
                    dongle_clock_hz,
                ),
            },
        }

    tx_by_id: dict[tuple[int, int], list[TxRecord]] = defaultdict(list)
    for record in tx_records:
        tx_by_id[(record.source, record.message_id)].append(record)
    tx_attempts = [record.attempts for record in tx_records]
    tx_attempt_summary = interval_summary(tx_attempts, None)
    tx_attempt_summary = {
        key.replace("_cycles", "_reported_attempts"): value
        for key, value in tx_attempt_summary.items()
        if key not in {"rejected_backward_or_reset"}
    }
    tx_result = {
        "callback_records": len(tx_records),
        "message_ids_seen": len(tx_by_id),
        "success_callbacks": sum(record.success for record in tx_records),
        "failure_callbacks": sum(not record.success for record in tx_records),
        "message_ids_with_success": sum(
            any(record.success for record in records) for records in tx_by_id.values()
        ),
        "message_ids_with_failure": sum(
            any(not record.success for record in records) for records in tx_by_id.values()
        ),
        "message_ids_failure_only": sum(
            not any(record.success for record in records) for records in tx_by_id.values()
        ),
        "reported_attempts_total": sum(tx_attempts),
        "reported_extra_attempts_lower_bound": sum(max(value - 1, 0) for value in tx_attempts),
        "reported_attempts_distribution": tx_attempt_summary,
        "by_source": {
            str(source): {
                "callback_records": len(records),
                "success_callbacks": sum(record.success for record in records),
                "failure_callbacks": sum(not record.success for record in records),
                "reported_attempts_total": sum(record.attempts for record in records),
                "reported_retransmissions_total": sum(
                    max(record.attempts - 1, 0) for record in records
                ),
            }
            for source, records in (
                (
                    source,
                    [record for record in tx_records if record.source == source],
                )
                for source in sorted({record.source for record in tx_records})
            )
        },
    }

    return {
        "input": {
            "line_count": line_count,
            "parsed_rx_records": len(rx_records),
            "parsed_tx_records": len(tx_records),
            "parsed_usb_records": len(usb_records),
            "parsed_usb_unmatched_records": len(usb_unmatched_records),
            "malformed_benchmark_markers": malformed_markers,
            "dongle_clock_hz": dongle_clock_hz,
            "source_clock_hz": source_clock_hz,
        },
        "totals": {
            "sources_seen": len(by_source),
            "missing_sequences": total_missing,
            "duplicates": total_duplicates,
            "out_of_order": total_out_of_order,
            "invalid_rx_callbacks": sum(invalid_errors.values()),
            "usb_queue_records": len(usb_records),
            "usb_unmatched_reports": len(usb_unmatched_records),
            "usb_queue_overflows": len(usb_queue_overflow),
        },
        "sources": source_results,
        "invalid_rx": {
            "errors": {str(key): value for key, value in sorted(invalid_errors.items())},
            "pipes": {str(key): value for key, value in sorted(invalid_pipes.items())},
            "overflow_latest_by_pipe": {
                str(key): value for key, value in sorted(rx_overflow_latest.items())
            },
        },
        "link_metrics": {
            str(source): {
                LINK_METRIC_NAMES.get(metric, f"unknown_{metric}"): {
                    "session": session,
                    "value": value,
                }
                for (metric_source, metric), (session, value) in sorted(
                    link_metric_latest.items()
                )
                if metric_source == source
            }
            for source in sorted(
                {source for source, _metric in link_metric_latest}
            )
        },
        "tx": tx_result,
        "usb_queue": {
            "matched_fifo_records": len(usb_records),
            "unmatched_report_calls": len(usb_unmatched_records),
            "queue_overflows": len(usb_queue_overflow),
            "overflow_by_source": {
                str(source): count
                for source, count in sorted(
                    Counter(source for source, _sequence in usb_queue_overflow).items()
                )
            },
            "unmatched_result_codes": {
                str(result): count
                for result, count in sorted(
                    Counter(record.result for record in usb_unmatched_records).items()
                )
            },
            "unmatched_queue_call_duration": duration_summary(
                (
                    (record.queue_enter_tick, record.queue_done_tick)
                    for record in usb_unmatched_records
                ),
                dongle_clock_hz,
            ),
        },
        "diagnostics": dict(sorted(diagnostics.items())),
        "interpretation": {
            "typical_definition": "median (p50)",
            "fresh_radio_metric": "accepted BENCH_RX records with wire=2",
            "sequence_scope": "all wire types per logical source",
            "clock_warning": (
                "source_tick and dongle_tick are unsynchronized; their absolute "
                "difference is not one-way latency"
            ),
            "usb_queue_clock_scope": (
                "rx_tick, queue_enter_tick, and queue_done_tick share the dongle clock"
            ),
            "usb_fifo_warning": (
                "BENCH_USB uses FIFO correlation and is valid only for simple 1:1 "
                "key-to-keyboard-report tests; hold-tap, layer, combo, sticky, local, "
                "or otherwise coalesced reports can be unmatched or misaligned"
            ),
            "synthetic_usb_warning": (
                "wire=2 synthetic benchmark packets do not generate HID reports"
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
    if "typical_ms" in summary:
        return (
            f"intervals={summary['interval_count']} "
            f"typical={format_number(summary['typical_ms'])} ms "
            f"p95={format_number(summary['p95_ms'])} ms "
            f"p99={format_number(summary['p99_ms'])} ms "
            f"max={format_number(summary['max_ms'])} ms "
            f"rate={format_number(summary['observed_rate_hz'])} Hz"
        )
    return (
        f"intervals={summary['interval_count']} "
        f"typical={format_number(summary['typical_cycles'])} cycles "
        f"p95={format_number(summary['p95_cycles'])} cycles "
        f"p99={format_number(summary['p99_cycles'])} cycles "
        f"max={format_number(summary['max_cycles'])} cycles"
    )


def format_duration(summary: dict[str, object]) -> str:
    if summary["sample_count"] == 0:
        return "samples=0"
    if "typical_ms" in summary:
        return (
            f"samples={summary['sample_count']} "
            f"typical={format_number(summary['typical_ms'])} ms "
            f"p95={format_number(summary['p95_ms'])} ms "
            f"p99={format_number(summary['p99_ms'])} ms "
            f"max={format_number(summary['max_ms'])} ms"
        )
    return (
        f"samples={summary['sample_count']} "
        f"typical={format_number(summary['typical_cycles'])} cycles "
        f"p95={format_number(summary['p95_cycles'])} cycles "
        f"p99={format_number(summary['p99_cycles'])} cycles "
        f"max={format_number(summary['max_cycles'])} cycles"
    )


def print_text(result: dict[str, object]) -> None:
    input_info = result["input"]
    totals = result["totals"]
    print("Totem ESB benchmark analysis")
    print(
        f"lines={input_info['line_count']} rx={input_info['parsed_rx_records']} "
        f"tx={input_info['parsed_tx_records']} usb={input_info['parsed_usb_records']} "
        f"usb_unmatched={input_info['parsed_usb_unmatched_records']} "
        f"malformed={input_info['malformed_benchmark_markers']}"
    )
    print(
        f"sources={totals['sources_seen']} missing={totals['missing_sequences']} "
        f"duplicates={totals['duplicates']} out_of_order={totals['out_of_order']} "
        f"invalid_rx={totals['invalid_rx_callbacks']}"
    )

    for source, data in result["sources"].items():
        sequence = data["sequence"]
        print()
        print(f"source {source} ({data['expected_role']})")
        if sequence is None:
            print("  sequence: no matching BENCH_RX records")
        else:
            print(
                "  sequence: "
                f"received={sequence['received']} "
                f"accepted_fresh={sequence['accepted_fresh']} "
                f"missing={sequence['missing_sequences']} "
                f"gap_records={sequence['gap_records']} "
                f"duplicates={sequence['duplicates']} "
                f"out_of_order={sequence['out_of_order']} "
                f"gap_mismatches={sequence['gap_mismatches']} "
                f"session_changes={sequence['session_changes']}"
            )
            print(f"  wire counts: {data['wire_counts']}")
            benchmark = data["benchmark_wire_2_cadence"]
            print(f"  fresh benchmark events (wire=2): {benchmark['event_count']}")
            print(
                "    dongle RX: "
                + format_interval(benchmark["dongle_rx_intervals"])
            )
            print(
                "    source generation: "
                + format_interval(benchmark["source_generation_intervals"])
            )
            keys = data["key_position_events"]
            print(
                "  key events: "
                f"accepted={keys['accepted_count']} press={keys['press_count']} "
                f"release={keys['release_count']}"
            )
        usb_queue = data["usb_queue_correlation"]
        print(
            "  USB queue FIFO: "
            f"records={usb_queue['record_count']} "
            f"accepted_keys={usb_queue['accepted_key_event_count']} "
            f"count_delta={usb_queue['record_count_minus_accepted_key_events']} "
            f"results={usb_queue['result_codes']} "
            f"missing_rx_identity={usb_queue['identity_not_found_in_bench_rx']} "
            f"field_mismatches={usb_queue['position_or_pressed_mismatches']}"
        )
        print(
            "    RX -> queue enter: "
            + format_duration(usb_queue["rx_to_queue_enter"])
        )
        print(
            "    queue call: "
            + format_duration(usb_queue["queue_call_duration"])
        )
        print(
            "    RX -> queue done: "
            + format_duration(usb_queue["rx_to_queue_done"])
        )

    print()
    tx = result["tx"]
    print(
        "TX callbacks: "
        f"records={tx['callback_records']} ids={tx['message_ids_seen']} "
        f"success={tx['success_callbacks']} fail={tx['failure_callbacks']} "
        f"failure_only_ids={tx['message_ids_failure_only']} "
        f"reported_attempts={tx['reported_attempts_total']} "
        f"extra_attempts_lower_bound={tx['reported_extra_attempts_lower_bound']}"
    )
    print(f"TX by source: {tx['by_source']}")
    print(f"latest cumulative link metrics: {result['link_metrics']}")
    print(f"invalid RX: {result['invalid_rx']}")
    print(f"USB queue summary: {result['usb_queue']}")
    print(f"diagnostics: {result['diagnostics']}")
    print(
        "NOTE: source_tick - dongle_tick is not latency; the clocks are "
        "unsynchronized. 'typical' means median."
    )
    print(
        "NOTE: BENCH_USB FIFO correlation is only valid for simple 1:1 key "
        "tests; hold-tap/layer/combo/sticky/local reports can be unmatched or "
        "misaligned. wire=2 synthetic packets do not create HID reports."
    )


def open_input(path: str) -> tuple[TextIO, bool]:
    if path == "-":
        return sys.stdin, False
    return Path(path).open("r", encoding="utf-8", errors="replace"), True


def positive_float(value: str) -> float:
    parsed = float(value)
    if parsed <= 0:
        raise argparse.ArgumentTypeError("clock frequency must be positive")
    return parsed


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description=(
            "Analyze BENCH_RX/BENCH_TX/BENCH_USB records from Totem ESB RTT "
            "logs. No clock frequency is assumed."
        )
    )
    parser.add_argument("log", help="RTT/log file, or - for stdin")
    parser.add_argument(
        "--clock-hz",
        type=positive_float,
        help="set both source and dongle k_cycle_get_32() frequencies",
    )
    parser.add_argument(
        "--dongle-clock-hz",
        type=positive_float,
        help="dongle k_cycle_get_32() frequency; overrides --clock-hz",
    )
    parser.add_argument(
        "--source-clock-hz",
        type=positive_float,
        help="peripheral k_cycle_get_32() frequency; overrides --clock-hz",
    )
    parser.add_argument(
        "--json",
        action="store_true",
        help="emit machine-readable JSON instead of text",
    )
    return parser


def main(argv: Optional[list[str]] = None) -> int:
    args = build_parser().parse_args(argv)
    dongle_clock_hz = args.dongle_clock_hz or args.clock_hz
    source_clock_hz = args.source_clock_hz or args.clock_hz

    stream, should_close = open_input(args.log)
    try:
        result = analyze(stream, dongle_clock_hz, source_clock_hz)
    finally:
        if should_close:
            stream.close()

    if args.json:
        json.dump(result, sys.stdout, ensure_ascii=False, indent=2, sort_keys=True)
        print()
    else:
        print_text(result)

    if (
        result["input"]["parsed_rx_records"] == 0
        and result["input"]["parsed_tx_records"] == 0
        and result["input"]["parsed_usb_records"] == 0
        and result["input"]["parsed_usb_unmatched_records"] == 0
    ):
        print("error: no benchmark records found", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
