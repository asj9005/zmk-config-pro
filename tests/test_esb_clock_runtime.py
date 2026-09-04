"""Compile the actual firmware clock callbacks against a deterministic driver fake."""
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


def function_text(source, name):
    match = re.search(r"static void " + name + r"\([^;]+?\)\s*\{", source, re.S)
    if not match:
        raise AssertionError(f"Firmware function missing: {name}")
    start = match.start()
    depth = 1
    end = match.end()
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


class ActualClockRuntimeTest(unittest.TestCase):
    def test_clock_callbacks(self):
        compiler = os.environ.get("CC") or shutil.which("gcc") or shutil.which("clang")
        if not compiler and Path(r"C:\QMK_MSYS\mingw64\bin\gcc.exe").is_file():
            compiler = r"C:\QMK_MSYS\mingw64\bin\gcc.exe"
        if not compiler:
            if os.environ.get("ESB_REQUIRE_C_COMPILER") == "1":
                self.fail("A C compiler is required for the firmware callback test")
            self.skipTest("A C compiler is required for the firmware callback test")
        environment = os.environ.copy()
        environment["PATH"] = str(Path(compiler).resolve().parent) + os.pathsep + environment.get("PATH", "")
        source = (ROOT / "compat/zmk-feature-split-esb/src/split/esb/app_esb.c").read_text(encoding="utf-8")
        actual = "\n\n".join(function_text(source, name) for name in
                               ("hf_clock_ready", "hf_clock_work_handler"))
        fixture = (ROOT / "tests/esb_clock_runtime_fixture.c").read_text(encoding="utf-8")
        with tempfile.TemporaryDirectory(prefix="esb-clock-") as directory:
            workdir = Path(directory)
            test_c = workdir / "test_clock.c"
            test_exe = workdir / ("test_clock.exe" if os.name == "nt" else "test_clock")
            test_c.write_text(fixture.replace("/* ACTUAL_FIRMWARE_FUNCTIONS */", actual), encoding="utf-8")
            subprocess.run([compiler, "-std=c11", "-Wall", "-Wextra", "-Werror",
                            str(test_c), "-o", str(test_exe)], check=True, env=environment, timeout=60)
            result = subprocess.run([str(test_exe)], check=True, capture_output=True,
                                    encoding="utf-8", errors="replace",
                                    env=environment, timeout=10)
            self.assertIn("cases passed", result.stdout)


if __name__ == "__main__":
    unittest.main()
