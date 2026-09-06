"""Compile the production Auto Base callbacks with deterministic ZMK API fakes.

This checks callback ordering/error handling and pinned ZMK layer-lock semantics,
not Zephyr scheduling or USB timing.
The firmware build separately validates the devicetree and behavior registration.
"""

import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


def function_text(source, name):
    match = re.search(r"static int " + name + r"\([^;]+?\)\s*\{", source, re.S)
    if not match:
        raise AssertionError(f"Firmware function missing: {name}")
    depth = 1
    end = match.end()
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[match.start():end]


class ActualAutoBaseRuntimeTest(unittest.TestCase):
    def test_auto_base_callbacks(self):
        compiler = os.environ.get("CC") or shutil.which("gcc") or shutil.which("clang")
        if not compiler and Path(r"C:\QMK_MSYS\mingw64\bin\gcc.exe").is_file():
            compiler = r"C:\QMK_MSYS\mingw64\bin\gcc.exe"
        if not compiler:
            if os.environ.get("ESB_REQUIRE_C_COMPILER") == "1":
                self.fail("A C compiler is required for the Auto Base callback test")
            self.skipTest("A C compiler is required for the Auto Base callback test")
        environment = os.environ.copy()
        environment["PATH"] = (str(Path(compiler).resolve().parent) + os.pathsep
                               + environment.get("PATH", ""))
        source = (ROOT / "src/behavior_auto_base.c").read_text(encoding="utf-8")
        actual = "\n\n".join(function_text(source, name) for name in
                               ("auto_base_pressed", "auto_base_released"))
        fixture = (ROOT / "tests/auto_base_runtime_fixture.c").read_text(encoding="utf-8")
        with tempfile.TemporaryDirectory(prefix="auto-base-") as directory:
            workdir = Path(directory)
            test_c = workdir / "test_auto_base.c"
            test_exe = workdir / ("test_auto_base.exe" if os.name == "nt" else "test_auto_base")
            test_c.write_text(fixture.replace("/* ACTUAL_FIRMWARE_FUNCTIONS */", actual),
                              encoding="utf-8")
            subprocess.run([compiler, "-std=c11", "-Wall", "-Wextra", "-Werror",
                            str(test_c), "-o", str(test_exe)], check=True,
                           env=environment, timeout=60)
            result = subprocess.run([str(test_exe)], check=True, capture_output=True,
                                    encoding="utf-8", errors="replace",
                                    env=environment, timeout=10)
            self.assertIn("9 Auto Base callback cases passed", result.stdout)


if __name__ == "__main__":
    unittest.main()
