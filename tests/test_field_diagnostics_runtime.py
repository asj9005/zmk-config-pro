#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Run actual field telemetry core and pure stats with deterministic boundaries."""
from __future__ import annotations
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import unittest
from tests.test_esb_firmware import compiler_command

ROOT = Path(__file__).resolve().parents[1]


def fixture_source() -> str:
    core = (ROOT / "src/field_diagnostics.c").read_text(encoding="utf-8")
    core = re.sub(r"^#include[^\n]*\n", "", core, flags=re.MULTILINE)
    return (ROOT / "tests/field_diagnostics_runtime_fixture.c").read_text(encoding="utf-8").replace(
        "/* ACTUAL_FIELD_CORE */", core)


class ActualFieldDiagnosticsTest(unittest.TestCase):
    def test_aggregates_and_bounded_nonblocking_cdc(self) -> None:
        compiler = compiler_command()
        if compiler is None:
            if os.environ.get("ESB_REQUIRE_C_COMPILER") == "1":
                self.fail("A host C compiler is required for field diagnostics")
            self.skipTest("Set CC to execute field diagnostics")
        environment = os.environ.copy()
        environment["PATH"] = str(Path(shutil.which(compiler[0]) or compiler[0]).resolve().parent) + os.pathsep + environment.get("PATH", "")
        source = fixture_source()
        variants = {
            "current": source,
            "deadline_lost": source.replace("now, field_connected()", "0, field_connected()", 1),
            "partial_skipped": source.replace("line_offset += (unsigned int)accepted;", "line_offset += requested;", 1),
            "suspend_ignored": source.replace("zmk_usb_get_status() != USB_DC_SUSPEND &&", "", 1),
        }
        with tempfile.TemporaryDirectory(prefix="field-diagnostics-") as directory:
            work = Path(directory)
            for name, generated in variants.items():
                with self.subTest(variant=name):
                    if name != "current": self.assertNotEqual(generated, source)
                    path = work / f"{name}.c"
                    executable = work / (name + (".exe" if os.name == "nt" else ""))
                    path.write_text(generated, encoding="utf-8")
                    if Path(compiler[0]).stem.lower() in ("cl", "clang-cl"):
                        args = ["/nologo", "/std:c11", "/W4", f"/I{ROOT / 'include'}", str(path), f"/Fe:{executable}"]
                    else:
                        args = ["-std=c11", "-Wall", "-Wextra", "-Werror", "-Wno-unused-function", "-Wno-unused-parameter",
                                "-I", str(ROOT / "include"), str(path), "-o", str(executable)]
                    built = subprocess.run(compiler + args, cwd=work, env=environment, capture_output=True, text=True, timeout=60)
                    self.assertEqual(built.returncode, 0, built.stdout + built.stderr)
                    result = subprocess.run([str(executable)], cwd=work, env=environment, capture_output=True, text=True, timeout=10)
                    if name == "current":
                        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                        self.assertIn("actual field diagnostics", result.stdout)
                        frame = subprocess.run([str(executable), "frame"], cwd=work, env=environment, capture_output=True, text=True, timeout=10)
                        self.assertEqual(frame.returncode, 0, frame.stdout + frame.stderr)
                        rows = [line for line in frame.stdout.splitlines() if line]
                        self.assertEqual(len(rows), 72)
                        for part, line in enumerate(rows):
                            self.assertIn(f"part={part} total=72", line)
                            self.assertLess(len(line), 768)
                        self.assertIn("kind=begin", rows[0]); self.assertIn("kind=end", rows[-1])
                        sys.path.insert(0, str(ROOT / "tools"))
                        try:
                            from field_diagnostics import parse_wire, SnapshotAssembler, expand_snapshot
                            assembler = SnapshotAssembler()
                            complete = []
                            for line in rows:
                                parsed = parse_wire(line.encode("ascii"))
                                self.assertIsNotNone(parsed, line)
                                assembled = assembler.add(parsed)
                                if assembled is not None:
                                    complete.append(assembled)
                            self.assertEqual(len(complete), 1)
                            self.assertEqual(len(expand_snapshot(complete[0])), 72)
                        finally:
                            sys.path.pop(0)
                    else:
                        self.assertNotEqual(result.returncode, 0, f"Undetected mutation: {name}")
                        self.assertIn("CHECK failed", result.stderr)


if __name__ == "__main__":
    unittest.main()
