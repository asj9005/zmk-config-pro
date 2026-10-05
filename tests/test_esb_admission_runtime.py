#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Run the actual v3 producer to check full-ring preflight and nonce commits."""
from __future__ import annotations

import os
from pathlib import Path
import unittest

from tests.test_esb_ack_runtime import braced_definition
from tests.test_esb_firmware import compiler_command
from tests.test_esb_tx_liveness_runtime import compile_and_run


ROOT = Path(__file__).resolve().parents[1]
ESB = ROOT / "compat/zmk-feature-split-esb/src/split/esb"


def fixture_source(*, remove_precheck: bool = False) -> str:
    source = (ESB / "peripheral.c").read_text(encoding="utf-8")
    header = (ESB / "common.h").read_text(encoding="utf-8")
    producer = braced_definition(source, r"^static int enqueue_v3_frame\([^;{}]*\)\s*\{")
    if remove_precheck:
        guard = braced_definition(
            producer, r"^    if \(ring_buf_space_get\(&tx_buf\) < frame_size\)\s*\{"
        )
        producer = producer.replace(guard, "", 1)
    structures = braced_definition(header, r"^enum esb_wire_event_type\s*\{") + ";\n"
    structures += "\n\n".join(
        braced_definition(header, rf"^struct {name}\s*\{{") + " __packed;"
        for name in ("esb_msg_prefix", "esb_key_state_payload", "esb_v3_recovery_payload",
                     "esb_event_payload", "esb_event_envelope", "esb_msg_postfix", "esb_msg_meta")
    )
    fixture = (ROOT / "tests/esb_admission_runtime_fixture.c").read_text(encoding="utf-8")
    return fixture.replace("/* ACTUAL_FRAME_STRUCTURES */", structures).replace(
        "/* ACTUAL_V3_ENQUEUE */", producer
    )


class ActualAdmissionRuntimeTest(unittest.TestCase):
    def test_full_ring_preflight_preserves_sequences(self) -> None:
        if compiler_command() is None:
            if os.environ.get("ESB_REQUIRE_C_COMPILER") == "1":
                self.fail("A host C compiler is required for the admission runtime test")
            self.skipTest("Set CC to execute the admission runtime test")
        result = compile_and_run(fixture_source())
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("4 v3 producer admission cases passed", result.stdout)
        negative = compile_and_run(fixture_source(remove_precheck=True))
        self.assertNotEqual(negative.returncode, 0)
        self.assertIn("full_ring_skips_crypto", negative.stderr)


if __name__ == "__main__":
    unittest.main()
