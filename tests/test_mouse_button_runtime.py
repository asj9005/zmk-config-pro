#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Run production mouse button callbacks with finite queue/failure injection."""
from pathlib import Path
import os
import unittest

from tests.test_esb_firmware import compiler_command
from tests.test_mouse_move_runtime import compile_and_run

ROOT = Path(__file__).resolve().parents[1]


def fixture_source(mutation=None):
    source = (ROOT / "compat/zmk/app/src/behaviors/behavior_mouse_key_press.c").read_text(encoding="utf-8")
    source = source.split("/* TESTABLE_BEGIN */", 1)[1].split("/* TESTABLE_END */", 1)[0]
    if mutation == "drop_failed_release":
        old = "if (err != 0) {"
        assert source.count(old) == 1
        source = source.replace(old, "if (err == -999) {")
    elif mutation == "forget_overflow_state":
        old = "data->resync = true;"
        assert source.count(old) == 1
        source = source.replace(old, "data->resync = false;")
    elif mutation == "sync_partial_mask":
        old = "packet.mask == bit, K_NO_WAIT"
        assert source.count(old) == 1
        source = source.replace(old, "true, K_NO_WAIT")
    elif mutation is not None:
        raise ValueError(mutation)
    fixture = (ROOT / "tests/mouse_button_runtime_fixture.c").read_text(encoding="utf-8")
    return fixture.replace("/* ACTUAL_BUTTON_BEHAVIOR */", source)


class ActualMouseButtonRuntimeTest(unittest.TestCase):
    def setUp(self):
        if compiler_command() is None:
            if os.environ.get("ESB_REQUIRE_C_COMPILER") == "1":
                self.fail("A host C compiler is required")
            self.skipTest("Set CC to execute mouse button runtime tests")

    def test_button_release_backpressure_and_overflow(self):
        run = compile_and_run(fixture_source())
        self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
        self.assertIn("8 actual mouse button cases passed", run.stdout)

    def test_negative_controls(self):
        for mutation, case in (("drop_failed_release", "final_release_retried_without_new_input"),
                               ("forget_overflow_state", "overflow_keeps_last_release"),
                               ("sync_partial_mask", "combined_masks_keep_final_sync")):
            with self.subTest(mutation=mutation):
                run = compile_and_run(fixture_source(mutation))
                self.assertNotEqual(run.returncode, 0)
                self.assertIn(case, run.stderr)


if __name__ == "__main__":
    unittest.main()
