#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Execute actual mouse behavior C with deterministic layer/work/input fakes.

The production structures and all functions through binding release are included
unchanged. This checks movement state, not DT generation, hold-tap dispatch,
Zephyr concurrency, USB, or physical pointer feel; ARM and device checks remain.
"""
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


def fixture_source(mutation: str | None = None) -> str:
    source = (ROOT / "src/behavior_mouse_move.c").read_text(encoding="utf-8")
    actual = source[source.index("struct vector2d {"):source.index(
        "static const struct behavior_driver_api")]
    if mutation == "shared_remainder":
        old = "&(state->remainder[mode])"
        assert actual.count(old) == 1
        actual = actual.replace(old, "&(state->remainder[MOUSE_SPEED_NORMAL])")
    elif mutation == "resample_y":
        actual, count = re.subn(
            r"update_movement_1d\(config, config->y_code, &state->y, now, mode\)",
            "update_movement_1d(config, config->y_code, &state->y, now, current_speed_mode(config))",
            actual,
        )
        assert count == 1
    elif mutation == "keep_inactive_remainders":
        # Leave the tick's zero-speed cleanup intact: a net-zero transition
        # must clear all modes immediately even when no next tick is scheduled.
        start = actual.index("static void set_start_times_for_activity_1d(")
        before, tail = actual[:start], actual[start:]
        old = "i < MOUSE_SPEED_MODE_COUNT"
        assert tail.count(old) == 1
        actual = before + tail.replace(old, "i < 1", 1)
    elif mutation is not None:
        raise ValueError(mutation)
    fixture = (ROOT / "tests/mouse_move_runtime_fixture.c").read_text(encoding="utf-8")
    assert fixture.count("/* ACTUAL_MOUSE_BEHAVIOR */") == 1
    return fixture.replace("/* ACTUAL_MOUSE_BEHAVIOR */", actual)


def compile_and_run(source: str, *, minimal: int = 0, smooth: int = 0):
    compiler = compiler_command()
    if compiler is None:
        raise RuntimeError("C compiler required")
    environment = os.environ.copy()
    compiler_path = shutil.which(compiler[0]) or compiler[0]
    environment["PATH"] = str(Path(compiler_path).resolve().parent) + os.pathsep + environment.get("PATH", "")
    with tempfile.TemporaryDirectory(prefix="mouse-move-runtime-") as directory:
        work = Path(directory)
        test_c = work / "fixture.c"
        executable = work / ("fixture.exe" if os.name == "nt" else "fixture")
        test_c.write_text(source, encoding="utf-8")
        if Path(compiler[0]).stem.lower() in ("cl", "clang-cl"):
            arguments = ["/nologo", "/std:c11", "/W4", "/WX", "/wd4100", "/wd4189",
                         f"/DCONFIG_MINIMAL_LIBC={minimal}", f"/DCONFIG_ZMK_POINTING_SMOOTH_SCROLLING={smooth}",
                         f"/I{ROOT / 'include'}", f"/I{ROOT / 'config'}", str(test_c), f"/Fe:{executable}"]
        else:
            # Pinned behavior has intentionally unused callback parameters and
            # the existing input-report result local; do not rewrite its C.
            arguments = ["-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
                         "-Wno-unused-parameter", "-Wno-unused-but-set-variable",
                         f"-DCONFIG_MINIMAL_LIBC={minimal}", f"-DCONFIG_ZMK_POINTING_SMOOTH_SCROLLING={smooth}",
                         "-I", str(ROOT / "include"), "-I", str(ROOT / "config"),
                         str(test_c), "-lm", "-o", str(executable)]
        built = subprocess.run(compiler + arguments, cwd=work, env=environment,
                               capture_output=True, text=True, timeout=60)
        if built.returncode != 0:
            raise AssertionError(built.stdout + built.stderr)
        return subprocess.run([str(executable)], cwd=work, env=environment,
                              capture_output=True, text=True, timeout=10)


class ActualMouseMoveRuntimeTest(unittest.TestCase):
    def setUp(self) -> None:
        if compiler_command() is None:
            if os.environ.get("ESB_REQUIRE_C_COMPILER") == "1":
                self.fail("A host C compiler is required for the mouse movement runtime test")
            self.skipTest("Set CC to execute the mouse movement runtime test")

    def test_fixed_modes_preserve_normal_movement_and_release(self) -> None:
        for minimal, smooth in ((0, 0), (1, 0), (0, 1), (1, 1)):
            with self.subTest(minimal_libc=minimal, smooth_scrolling=smooth):
                run = compile_and_run(fixture_source(), minimal=minimal, smooth=smooth)
                self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
                self.assertIn("7 actual mouse behavior cases passed", run.stdout)

    def test_negative_controls_detect_state_and_snapshot_regressions(self) -> None:
        for mutation, case in (("shared_remainder", "normal_state_isolation"),
                               ("resample_y", "single_tick_snapshot"),
                               ("keep_inactive_remainders", "binding_release_after_base")):
            with self.subTest(mutation=mutation):
                run = compile_and_run(fixture_source(mutation))
                self.assertNotEqual(run.returncode, 0)
                self.assertIn(case, run.stderr)


if __name__ == "__main__":
    unittest.main()
