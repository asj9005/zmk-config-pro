#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Passively collect allowlisted Totem diagnostics; never send serial data."""
from __future__ import annotations

import argparse
from collections import Counter
import json
import os
from pathlib import Path
import re
import sys
import time
import uuid

from field_diagnostics import (FILE_BYTES, FORMAT_VERSION, MAX_FILES, MARKERS,
                               LineBuffer, RingWriter, SnapshotAssembler, TIMESTAMP_RE,
                               load_json_line, parse_wire, pc_stamp, utc_now, write_metadata)

VID, PID = 0x1D50, 0x615E


def windows_interfaces() -> dict[str, int]:
    """Read only Totem USB interface→COM mappings; never keep device serials."""
    if os.name != "nt":
        return {}
    import winreg
    result = {}
    try:
        with winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE,
                            r"SYSTEM\CurrentControlSet\Enum\USB") as usb:
            for i in range(winreg.QueryInfoKey(usb)[0]):
                name = winreg.EnumKey(usb, i)
                match = re.fullmatch(r"VID_1D50&PID_615E&MI_([0-9A-F]{2})", name.upper())
                if not match:
                    continue
                with winreg.OpenKey(usb, name) as interface:
                    for j in range(winreg.QueryInfoKey(interface)[0]):
                        try:
                            instance = winreg.EnumKey(interface, j)
                            with winreg.OpenKey(interface, instance + r"\Device Parameters") as parameters:
                                port = winreg.QueryValueEx(parameters, "PortName")[0]
                            if isinstance(port, str) and re.fullmatch(r"COM[0-9]{1,4}", port, re.I):
                                key, number = port.upper(), int(match[1], 16)
                                # Registry keeps disconnected instances too.
                                # A reused COM name with conflicting interfaces
                                # is ambiguous, never proof that it is console.
                                result[key] = number if key not in result or result[key] == number else -1
                        except OSError:
                            continue
    except OSError:
        pass
    return result


def console_candidates(ports, interfaces: dict, requested: set[str]):
    """Pinned USB layout: console MI_00, Studio MI_03. Unknown stays closed."""
    result = []
    for port in ports:
        if (port.vid, port.pid) != (VID, PID):
            continue
        if requested and port.device.casefold() not in requested:
            continue
        label = (getattr(port, "interface", None) or "").casefold()
        number = interfaces.get(port.device.upper())
        if number is None:
            match = re.search(r"\bMI_([0-9A-Fa-f]{2})\b", getattr(port, "hwid", "") or "")
            if match:
                number = int(match[1], 16)
        if "studio" in label or number == 3:
            continue
        if number == 0 or ("console" in label and number is None):
            result.append(port.device)
    return sorted(set(result))


def open_passive(serial_module, device):
    port = serial_module.Serial(port=None, baudrate=115200, timeout=0,
                                write_timeout=0, rtscts=False, dsrdtr=False)
    # Set states before opening; no serial.write, commands, breaks, or reset.
    port.dtr = False
    port.rts = False
    port.port = device
    port.open()
    return port


def mark(directory: Path, symptom: str):
    if symptom not in MARKERS:
        raise ValueError("unknown symptom")
    directory = directory.resolve(strict=True)
    metadata = directory / "capture-meta.json"
    if metadata.is_symlink() or metadata.stat().st_size > 16384:
        raise ValueError("invalid capture metadata")
    meta = json.loads(metadata.read_text(encoding="utf-8"))
    if meta.get("format") != FORMAT_VERSION or meta.get("status") != "running":
        raise ValueError("capture is not running")
    # One bounded request slot. Refuse an occupied slot; no user text accepted.
    request = directory / "marker-request.json"
    with request.open("x", encoding="utf-8") as output:
        json.dump(dict(pc_stamp(), symptom=symptom), output)
    print("Symptom marker queued.")


def take_marker(directory: Path, writer, stats):
    request = directory / "marker-request.json"
    if not request.exists():
        return
    if request.is_symlink() or not request.is_file():
        stats["invalid_marker"] += 1
        return
    try:
        with request.open("rb") as source:
            raw = source.read(1025)
        if len(raw) > 1024:
            raise ValueError("oversized marker")
        value = load_json_line(raw)
        if set(value) != {"symptom", "pc_utc", "pc_monotonic_ns"} or value["symptom"] not in MARKERS:
            raise ValueError("invalid marker")
        if not isinstance(value["pc_utc"], str) or not TIMESTAMP_RE.fullmatch(value["pc_utc"]):
            raise ValueError("invalid marker timestamp")
        if type(value["pc_monotonic_ns"]) is not int or value["pc_monotonic_ns"] < 0:
            raise ValueError("invalid marker clock")
        writer.write(dict(value, kind="marker"))
        stats["markers"] += 1
        request.unlink()
    except (ValueError, OSError):
        # A concurrent writer may not have finished yet. Retry on the next poll.
        stats["invalid_marker"] += 1


def collect(args, serial_module, list_ports):
    requested = {port.casefold() for port in args.port}
    stamp = utc_now().replace(":", "").replace(".", "")
    directory = args.output / ("capture-" + stamp + "-" + uuid.uuid4().hex[:8])
    writer = RingWriter(directory)
    stats = Counter()
    meta = {"format": FORMAT_VERSION, "status": "running", "start_utc": utc_now(),
            "duration_seconds": args.hours * 3600, "file_bytes": FILE_BYTES, "max_files": MAX_FILES,
            "schema": "totem-field", "serial_commands_sent": 0, "dtr": False, "rts": False,
            "clock_scope": "PC receipt clocks are not USB or radio latency measurements"}
    write_metadata(directory, meta)
    print(f"Capture: {directory}")
    print("Passive console capture; Ctrl+C stops and flushes. Default duration is 24 hours.")
    states, aliases, retry_after, connections = {}, {}, {}, Counter()
    started = time.monotonic()
    next_scan = next_meta = next_notice = 0.0

    def event(kind, alias, connection, **fields):
        writer.write(dict(pc_stamp(), kind=kind, stream=alias, connection=connection, **fields))

    def close(device, reason):
        state = states.pop(device)
        state["assembler"].finish()
        stats.update(state["assembler"].stats)
        try:
            state["serial"].close()
        except (OSError, serial_module.SerialException):
            pass
        event("disconnect", state["alias"], state["connection"], reason=reason)
        stats["disconnects"] += 1
        retry_after[device] = time.monotonic() + (30 if reason == "unverified" else 2)

    try:
        while time.monotonic() - started < args.hours * 3600:
            now = time.monotonic()
            if now >= next_scan:
                candidates = console_candidates(list_ports.comports(), windows_interfaces(), requested)
                for device in list(states):
                    if device not in candidates:
                        close(device, "removed")
                for device in candidates:
                    if device in states or now < retry_after.get(device, 0):
                        continue
                    if device not in aliases and len(aliases) >= 16:
                        stats["port_limit"] += 1
                        continue
                    alias = aliases.setdefault(device, f"console{len(aliases) + 1}")
                    try:
                        port = open_passive(serial_module, device)
                    except (OSError, serial_module.SerialException):
                        stats["open_failed"] += 1
                        retry_after[device] = now + 5
                        continue
                    connections[device] += 1
                    states[device] = {"serial": port, "alias": alias,
                                      "connection": connections[device], "buffer": LineBuffer(),
                                      "assembler": SnapshotAssembler(),
                                      "opened": now, "verified": False, "stale": False,
                                      "last_snapshot": now}
                    event("connect", alias, connections[device])
                next_scan = now + 3
            for device, state in list(states.items()):
                try:
                    raw = state["serial"].read(4096)
                except (OSError, serial_module.SerialException):
                    close(device, "read_failed")
                    continue
                before = state["buffer"].oversized
                for line in state["buffer"].feed(raw):
                    part = parse_wire(line)
                    if part is None:
                        stats["discarded_lines"] += 1
                        continue
                    if not state["verified"]:
                        state["verified"] = True
                        event("verified", state["alias"], state["connection"], role=part["role"])
                        print(f"Verified {state['alias']}: {part['role']}")
                    snapshot = state["assembler"].add(part)
                    if snapshot is not None:
                        if state["stale"]:
                            event("telemetry_resumed", state["alias"], state["connection"])
                        state["stale"] = False
                        state["last_snapshot"] = now
                        event("snapshot", state["alias"], state["connection"], snapshot=snapshot)
                        stats["snapshots"] += 1
                    stats["parts"] += 1
                stats["oversized_lines"] += state["buffer"].oversized - before
                # Allow more than two 30-second export periods before deciding
                # that a console cannot provide this schema.
                if not state["verified"] and now - state["opened"] > 75:
                    close(device, "unverified")
                elif state["verified"] and not state["stale"] and now - state["last_snapshot"] > 75:
                    state["stale"] = True
                    event("telemetry_stale", state["alias"], state["connection"])
                    stats["telemetry_stale"] += 1
            take_marker(directory, writer, stats)
            if now >= next_meta:
                write_metadata(directory, dict(meta, updated_utc=utc_now(), counts=dict(stats),
                                               records=writer.total_records, rotations=writer.rotations))
                next_meta = now + 15
            if now >= next_notice:
                verified = sum(state["verified"] and not state["stale"] for state in states.values())
                print(f"Elapsed {int((now-started)/60)} min; verified consoles {verified}; diagnostic parts {stats['parts']}.")
                next_notice = now + 60
            time.sleep(0.05)
    except KeyboardInterrupt:
        meta["stop_reason"] = "interrupted"
    finally:
        for device in list(states):
            close(device, "capture_stopped")
        writer.close()
        write_metadata(directory, dict(meta, status="stopped", end_utc=utc_now(), counts=dict(stats),
                                       records=writer.total_records, rotations=writer.rotations))
    print("Capture stopped; preparing the analysis report.")
    try:
        from analyze_field_diagnostics import analyze
        report = analyze(directory)
        with (directory / "report.json").open("x", encoding="utf-8") as output:
            json.dump(report, output, indent=2, ensure_ascii=True)
            output.write("\n")
        print(f"Analysis report: {directory / 'report.json'}")
    except (ValueError, OSError, TypeError):
        print("Automatic analysis could not finish. The capture remains available for separate analysis.")
    return 0


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, default=Path.cwd() / "captures",
                        help="Parent for a new capture directory; with --mark, exact capture directory")
    parser.add_argument("--port", action="append", default=[], help="Optional console port; repeatable")
    parser.add_argument("--hours", type=float, default=24, help="Duration, 0.001 to 72 hours")
    parser.add_argument("--mark", choices=sorted(MARKERS), help="Queue a symptom marker in --output capture")
    args = parser.parse_args(argv)
    if not 0.001 <= args.hours <= 72:
        parser.error("--hours must be between 0.001 and 72")
    try:
        if args.mark:
            mark(args.output, args.mark)
            return 0
        import serial
        from serial.tools import list_ports
        return collect(args, serial, list_ports)
    except (ValueError, OSError, ImportError) as error:
        # No raw exception strings: serial/JSON errors can contain device data.
        print(f"Capture could not continue ({type(error).__name__}). Check Python/pyserial, output permissions, and console availability.", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
