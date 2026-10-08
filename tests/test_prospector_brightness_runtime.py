#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Run the complete pinned brightness overlay with sensor/LED/sleep substitutes."""
from __future__ import annotations

import os
from pathlib import Path
import re
import unittest

from tests.test_esb_firmware import compiler_command
from tests.test_esb_tx_liveness_runtime import compile_and_run

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "compat/prospector-zmk-module/boards/shields/prospector_adapter/src/brightness.c"
FIXTURE = ROOT / "tests/fixtures/prospector/brightness-main.c"


def fixture_source(*, ambient: bool, mutation: str | None = None) -> str:
    source = SOURCE.read_text(encoding="utf-8")
    changes = {
        "missing_sensor_used": ('printk("sensor: device not ready.\\n");\n        return;',
                                'printk("sensor: device not ready.\\n");'),
        "failed_sample_used": ('if (read_ambient_light(dev, &intensity) != 0) {',
                               'if (read_ambient_light(dev, &intensity) == INT_MAX) {'),
        "final_pwm_missing": ('led_set_brightness(pwm_leds_dev, DISP_BL, next)',
                              'led_set_brightness(pwm_leds_dev, DISP_BL, current_brightness)'),
        "failed_pwm_committed": ('LOG_ERR("Failed to set brightness");\n            return err;',
                                 'LOG_ERR("Failed to set brightness");\n            current_brightness = next;\n            return 0;'),
        "no_error_backoff": ('#define ERROR_RETRY_SLEEP_MS             1000',
                             '#define ERROR_RETRY_SLEEP_MS             100'),
        "fixed_pwm_error_hidden": ('return led_set_brightness(pwm_leds_dev, DISP_BL, CONFIG_PROSPECTOR_FIXED_BRIGHTNESS);',
                                   '(void)led_set_brightness(pwm_leds_dev, DISP_BL, CONFIG_PROSPECTOR_FIXED_BRIGHTNESS);\n    return 0;'),
    }
    if mutation is not None:
        old, new = changes[mutation]
        if old not in source:
            raise AssertionError(f"Brightness mutation target missing: {mutation}")
        source = source.replace(old, new, 1)
    # Retain all production functions and branches. Only external API headers
    # and device/thread declarations are substituted by the fixture.
    source = re.sub(r"^#include[^\n]*", "", source, flags=re.M)
    fixture = FIXTURE.read_text(encoding="utf-8")
    return f"#define TEST_AMBIENT {int(ambient)}\n" + fixture.replace("/* ACTUAL_BRIGHTNESS_SOURCE */", source)


class ProspectorBrightnessTests(unittest.TestCase):
    def test_sensor_and_pwm_failures_preserve_applied_brightness(self) -> None:
        if compiler_command() is None:
            if os.environ.get("ESB_REQUIRE_C_COMPILER") == "1":
                self.fail("A host C compiler is required for brightness runtime tests")
            self.skipTest("Set CC to execute the brightness runtime test")
        for ambient in (False, True):
            with self.subTest(ambient=ambient):
                run = compile_and_run(fixture_source(ambient=ambient))
                self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
                self.assertIn("brightness scenarios passed", run.stdout)
        for mutation in ("missing_sensor_used", "failed_sample_used", "final_pwm_missing",
                         "failed_pwm_committed", "no_error_backoff", "fixed_pwm_error_hidden"):
            with self.subTest(mutation=mutation):
                run = compile_and_run(fixture_source(ambient=mutation != "fixed_pwm_error_hidden",
                                                    mutation=mutation))
                self.assertNotEqual(run.returncode, 0, "Brightness defect escaped: " + mutation)
                self.assertIn("brightness assertion failed", run.stderr)


if __name__ == "__main__":
    unittest.main()
