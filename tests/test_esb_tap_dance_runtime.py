#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Actual tap-dance overlay, with controlled work/behavior/RX timing boundaries."""
from __future__ import annotations
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest
from tests.test_esb_firmware import compiler_command

ROOT = Path(__file__).resolve().parents[1]


def fixture_variants() -> dict[str, str]:
    source = (ROOT / "compat/zmk/app/src/behaviors/behavior_tap_dance.c").read_text(encoding="utf-8")
    source = re.sub(r"^#include[^\n]*\n", "", source, flags=re.MULTILINE)
    variants = {
        "current": source,
        "no_overdue_timer": source.replace(
            "k_work_schedule(&tap_dance->release_timer,\n                    ms_left > 0 ? K_MSEC(ms_left) : K_NO_WAIT);",
            "if (ms_left > 0) k_work_schedule(&tap_dance->release_timer, K_MSEC(ms_left));", 1),
        "timer_overtakes_rx": source.replace(
            "if (totem_esb_rx_pending_before(tap_dance->release_at))",
            "if (false && totem_esb_rx_pending_before(tap_dance->release_at))", 1),
        "expired_taps_merge": source.replace(
            "event.timestamp >= tap_dance->release_at", "false", 1),
        "cancelled_timer_requeued": source.replace(
            "if (tap_dance->timer_cancelled) {\n        return;\n    }", "", 1),
    }
    fixture = (ROOT / "tests/esb_tap_dance_runtime_fixture.c").read_text(encoding="utf-8")
    for name, variant in variants.items():
        if name != "current" and source == variant:
            raise AssertionError(f"Mutation failed: {name}")
    return {name: fixture.replace("/* ACTUAL_TAP_DANCE_SOURCE */", variant)
            for name, variant in variants.items()}


class ActualTapDanceTimingTest(unittest.TestCase):
    def test_delayed_ingress_keeps_dance_deadlines(self) -> None:
        compiler = compiler_command()
        if compiler is None:
            if os.environ.get("ESB_REQUIRE_C_COMPILER") == "1":
                self.fail("A host C compiler is required for tap-dance timing tests")
            self.skipTest("Set CC to execute tap-dance timing tests")
        environment = os.environ.copy()
        environment["PATH"] = str(Path(shutil.which(compiler[0]) or compiler[0]).resolve().parent) + os.pathsep + environment.get("PATH", "")
        with tempfile.TemporaryDirectory(prefix="esb-tap-dance-") as directory:
            work = Path(directory)
            for name, generated in fixture_variants().items():
                with self.subTest(variant=name):
                    path = work / f"{name}.c"
                    executable = work / (name + (".exe" if os.name == "nt" else ""))
                    path.write_text(generated, encoding="utf-8")
                    if Path(compiler[0]).stem.lower() in ("cl", "clang-cl"):
                        args = ["/nologo", "/std:c11", "/W4", str(path), f"/Fe:{executable}"]
                    else:
                        args = ["-std=gnu11", "-Wall", "-Wextra", "-Werror", "-Wno-unused-function",
                                "-Wno-unused-parameter", "-Wno-unused-const-variable", "-Wno-sign-compare",
                                str(path), "-o", str(executable)]
                    built = subprocess.run(compiler + args, cwd=work, env=environment,
                                           capture_output=True, text=True, timeout=60)
                    self.assertEqual(built.returncode, 0, built.stdout + built.stderr)
                    result = subprocess.run([str(executable)], cwd=work, env=environment,
                                            capture_output=True, text=True, timeout=10)
                    if name == "current":
                        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                        self.assertIn("10 actual tap-dance timing scenarios passed", result.stdout)
                    else:
                        self.assertNotEqual(result.returncode, 0, f"Undetected mutation: {name}")
                        self.assertIn("CHECK failed", result.stderr)


if __name__ == "__main__":
    unittest.main()
