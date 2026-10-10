#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Run actual battery admission, refresh and authenticated SESSION_OK C paths.

The fixture models queue admission and scheduling, not RF or sensor hardware.
"""
from __future__ import annotations

import os
from pathlib import Path
import unittest

from tests.test_esb_ack_runtime import braced_definition
from tests.test_esb_firmware import compiler_command
from tests.test_esb_tx_liveness_runtime import compile_and_run

ROOT = Path(__file__).resolve().parents[1]


def fixture_source(negative: str | None = None) -> str:
    source = (ROOT / "compat/zmk-feature-split-esb/src/split/esb/peripheral.c").read_text(
        encoding="utf-8"
    )
    functions = {}
    for name in ("battery_refresh_schedule_locked", "cache_battery_event_locked",
                 "battery_refresh_work_cb", "split_peripheral_esb_report_event"):
        functions[name] = braced_definition(
            source, rf"^static (?:int|void)\s+{name}\([^;{{}}]*\)\s*\{{"
        )
    downlink = braced_definition(source, r"^static int process_v3_downlink\([^;{}]*\)\s*\{")
    session_ok = braced_definition(
        downlink, r"^    if \(env->payload.wire_type == ESB_WIRE_COMMAND_V3_SESSION_OK\)\s*\{"
    )
    if negative == "no_session_replay":
        assert "        battery_refresh_schedule_locked();" in session_ok
        session_ok = session_ok.replace("        battery_refresh_schedule_locked();", "", 1)
    elif negative == "metadata_overtakes_edges":
        functions["battery_refresh_work_cb"] = functions["battery_refresh_work_cb"].replace(
            "k_msgq_num_used_get(&presession_events) == 0",
            "true || k_msgq_num_used_get(&presession_events) == 0", 1
        )
    elif negative == "send_unknown_zero":
        functions["battery_refresh_work_cb"] = functions["battery_refresh_work_cb"].replace(
            "!cached_battery_valid || ", "", 1
        )
    elif negative == "refresh_too_often":
        functions["battery_refresh_work_cb"] = functions["battery_refresh_work_cb"].replace(
            "MAX(1, CONFIG_ZMK_BATTERY_REPORT_INTERVAL)", "1", 1
        )
    elif negative is not None:
        raise ValueError(negative)
    actual = "\n\n".join(functions.values())
    actual += ("\nstatic int receive_session_ok(const struct fake_envelope *env) {\n"
               "    const uint8_t source = 0;\n" + session_ok + "\n    return -EINVAL;\n}\n")
    fixture = (ROOT / "tests/esb_battery_runtime_fixture.c").read_text(encoding="utf-8")
    return fixture.replace("/* ACTUAL_FIRMWARE_FUNCTIONS */", actual)


class ActualBatteryRuntimeTest(unittest.TestCase):
    def test_cached_battery_survives_reconnect_and_loss(self) -> None:
        if compiler_command() is None:
            if os.environ.get("ESB_REQUIRE_C_COMPILER") == "1":
                self.fail("A host C compiler is required for the battery runtime test")
            self.skipTest("Set CC to execute the battery runtime test")
        result = compile_and_run(fixture_source())
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("10 battery cache/reconnect cases passed", result.stdout)
        for negative in ("no_session_replay", "metadata_overtakes_edges",
                         "send_unknown_zero", "refresh_too_often"):
            with self.subTest(negative=negative):
                result = compile_and_run(fixture_source(negative))
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("battery assertion failed:", result.stderr)


if __name__ == "__main__":
    unittest.main()
