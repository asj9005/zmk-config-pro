#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Exercise pinned sticky-key callbacks with delayed ingress timestamps.

Only the kernel, event routing and binding boundaries are faked. The RX timing
predicate has separate real queue coverage in test_esb_rx_runtime/hold_tap.
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
    source = (ROOT / "compat/zmk/app/src/behaviors/behavior_sticky_key.c").read_text(
        encoding="utf-8"
    )
    if negative == "overdue_unarmed":
        old = "ms_left > 0 ? K_MSEC(ms_left) : K_NO_WAIT"
        assert old in source
        source = source.replace(
            "    k_work_schedule(&sticky_key->release_timer, " + old + ");",
            "    if (ms_left > 0) { k_work_schedule(&sticky_key->release_timer, K_MSEC(ms_left)); }",
            1,
        )
    elif negative == "expiry_overtakes_rx":
        old = "totem_esb_rx_pending_before(sticky_key->release_at)"
        assert old in source
        source = source.replace(old, "false && " + old, 1)
    elif negative == "late_rx_extends_window":
        old = "totem_esb_rx_pending_before(sticky_key->release_at)"
        assert old in source
        source = source.replace(old, "totem_esb_rx_pending_before(INT64_MAX)", 1)
    elif negative is not None:
        raise ValueError(negative)
    structures = "\n\n".join(
        braced_definition(source, rf"^struct {name}\s*\{{") + ";"
        for name in ("behavior_sticky_key_config", "active_sticky_key")
    )
    names = (
        "store_sticky_key", "clear_sticky_key", "find_sticky_key",
        "press_sticky_key_behavior", "release_sticky_key_behavior",
        "on_sticky_key_timeout", "stop_timer", "on_sticky_key_binding_pressed",
        "on_sticky_key_binding_released", "sticky_key_keycode_state_changed_listener",
        "behavior_sticky_key_timer_handler",
    )
    functions = "\n\n".join(
        braced_definition(
            source, rf"^(?:static\s+)?(?:inline\s+)?(?:struct active_sticky_key\s*\*\s*|int\s+|void\s+){name}\([^;{{}}]*\)\s*\{{"
        ) for name in names
    )
    fixture = (ROOT / "tests/esb_sticky_key_runtime_fixture.c").read_text(encoding="utf-8")
    return fixture.replace("/* ACTUAL_STRUCTURES */", structures).replace(
        "/* ACTUAL_CALLBACKS */", functions
    )


class ActualStickyKeyRuntimeTest(unittest.TestCase):
    def test_ingress_timestamp_expiry_order(self) -> None:
        if compiler_command() is None:
            if os.environ.get("ESB_REQUIRE_C_COMPILER") == "1":
                self.fail("A host C compiler is required for the sticky-key runtime test")
            self.skipTest("Set CC to execute the sticky-key runtime test")
        result = compile_and_run(fixture_source())
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("11 sticky ingress/expiry cases passed", result.stdout)
        for negative in ("overdue_unarmed", "expiry_overtakes_rx", "late_rx_extends_window"):
            with self.subTest(negative=negative):
                result = compile_and_run(fixture_source(negative))
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("sticky assertion failed:", result.stderr)


if __name__ == "__main__":
    unittest.main()
