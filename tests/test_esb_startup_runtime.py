#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Execute the real peripheral input callbacks across deferred ESB startup.

The fixture models only the mutex, bounded input queue and scheduled work. It
does not model the Zephyr scheduler, radio, crypto backend or USB enumeration.
"""

from __future__ import annotations

import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

from tests.test_esb_ack_runtime import braced_definition
from tests.test_esb_firmware import compiler_command


ROOT = Path(__file__).resolve().parents[1]


class ActualStartupRuntimeTest(unittest.TestCase):
    def test_input_during_deferred_startup(self) -> None:
        compiler = compiler_command()
        if compiler is None:
            if os.environ.get("ESB_REQUIRE_C_COMPILER") == "1":
                self.fail("A host C compiler is required for the startup runtime test")
            self.skipTest("Set CC to execute the startup runtime test")

        source = (ROOT / "compat/zmk-feature-split-esb/src/split/esb/peripheral.c").read_text(
            encoding="utf-8"
        )
        actual = "\n\n".join(
            braced_definition(source, rf"^static int\s+{name}\([^;{{}}]*\)\s*\{{")
            for name in ("report_event_locked", "split_peripheral_esb_report_event")
        )
        fixture = (ROOT / "tests/esb_startup_runtime_fixture.c").read_text(encoding="utf-8")
        fixture = fixture.replace("/* ACTUAL_FIRMWARE_FUNCTIONS */", actual)
        environment = os.environ.copy()
        compiler_path = shutil.which(compiler[0]) or compiler[0]
        environment["PATH"] = (
            str(Path(compiler_path).resolve().parent) + os.pathsep
            + environment.get("PATH", "")
        )

        with tempfile.TemporaryDirectory(prefix="esb-startup-runtime-") as directory:
            workdir = Path(directory)
            test_c = workdir / "test_startup.c"
            executable = workdir / ("test_startup.exe" if os.name == "nt" else "test_startup")
            test_c.write_text(fixture, encoding="utf-8")
            if Path(compiler[0]).stem.lower() in ("cl", "clang-cl"):
                arguments = ["/nologo", "/std:c11", "/W4", "/WX", str(test_c), f"/Fe:{executable}"]
            else:
                arguments = [
                    "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic",
                    str(test_c), "-o", str(executable),
                ]
            compiled = subprocess.run(
                compiler + arguments, cwd=workdir, env=environment, text=True,
                encoding="utf-8", errors="replace",
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=60,
            )
            self.assertEqual(compiled.returncode, 0, compiled.stdout)
            result = subprocess.run(
                [str(executable)], env=environment, text=True,
                encoding="utf-8", errors="replace",
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=10,
            )
            self.assertEqual(result.returncode, 0, result.stdout)
            self.assertIn("8 peripheral startup input cases passed", result.stdout)


if __name__ == "__main__":
    unittest.main()
