#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Run real peripheral admission/common TX/app PTX code with bounded driver fakes."""
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
ESB = ROOT / "compat/zmk-feature-split-esb/src/split/esb"


def fixture_source(*, pump_candidate: bool = False, remove_pump: bool = False) -> str:
    app = (ESB / "app_esb.c").read_text(encoding="utf-8")
    common = (ESB / "common.c").read_text(encoding="utf-8")
    common_h = (ESB / "common.h").read_text(encoding="utf-8")
    peripheral = (ESB / "peripheral.c").read_text(encoding="utf-8")
    pull = braced_definition(app, r"^static int pull_packet_from_tx_msgq_unlocked\(void\)\s*\{")
    send = braced_definition(app, r"^int zmk_split_esb_send\([^;{}]*\)\s*\{")
    # This is a PTX regression; leave its validation/admission/drain C unchanged.
    # The separate PRX branch is replaced only to omit unrelated ACK queue fakes.
    prx = braced_definition(send, r"^    if \(m_mode == APP_ESB_MODE_PRX\)\s*\{")
    send = send.replace(prx, "    if (m_mode == APP_ESB_MODE_PRX) { return -ENOTSUP; }")
    send = send.replace("int zmk_split_esb_send(", "static int zmk_split_esb_send(", 1)
    transmit = braced_definition(common, r"^void zmk_split_esb_tx\([^;{}]*\)\s*\{")
    begin = braced_definition(peripheral, r"^static void begin_tx\(void\)\s*\{")
    begin = begin.replace("{\n", "{\n    pump_calls++;\n", 1)
    enqueue = braced_definition(peripheral, r"^static int enqueue_v3_frame\([^;{}]*\)\s*\{")
    admission = braced_definition(enqueue, r"^    if \(ring_buf_space_get\(&tx_buf\) < frame_size\)\s*\{")
    if pump_candidate and "begin_tx();" not in admission:
        admission = admission.replace("        k_spin_unlock(&tx_ring_lock, key);",
                                      "        begin_tx();\n        k_spin_unlock(&tx_ring_lock, key);", 1)
    if remove_pump:
        admission = admission.replace("        begin_tx();\n", "", 1)
    structures = "\n\n".join(
        braced_definition(common_h, rf"^struct {name}\s*\{{") + " __packed;"
        for name in ("esb_msg_prefix", "esb_msg_postfix", "esb_msg_meta")
    )
    fixture = (ROOT / "tests/esb_tx_liveness_runtime_fixture.c").read_text(encoding="utf-8")
    for marker, content in {
        "ACTUAL_FRAME_STRUCTURES": structures, "ACTUAL_PTX_PULL": pull,
        "ACTUAL_PTX_SEND": send, "ACTUAL_COMMON_TX": transmit,
        "ACTUAL_BEGIN_TX": begin, "ACTUAL_PRODUCER_ADMISSION": admission,
    }.items():
        fixture = fixture.replace(f"/* {marker} */", content)
    return fixture


def compile_and_run(source: str, *, lower_capacity: int = 4, producer_capacity: int = 3):
    compiler = compiler_command()
    if compiler is None:
        raise RuntimeError("C compiler required")
    environment = os.environ.copy()
    compiler_path = shutil.which(compiler[0]) or compiler[0]
    environment["PATH"] = str(Path(compiler_path).resolve().parent) + os.pathsep + environment.get("PATH", "")
    with tempfile.TemporaryDirectory(prefix="esb-tx-liveness-") as directory:
        work = Path(directory)
        test_c, executable = work / "fixture.c", work / ("fixture.exe" if os.name == "nt" else "fixture")
        test_c.write_text(source, encoding="utf-8")
        if Path(compiler[0]).stem.lower() in ("cl", "clang-cl"):
            source = source.replace("#define __packed __attribute__((packed))", "#define __packed\n#pragma pack(push, 1)")
            test_c.write_text(source, encoding="utf-8")
            arguments = ["/nologo", "/std:c11", "/W4", "/WX",
                         f"/DTEST_LOWER_CAPACITY={lower_capacity}", f"/DTEST_PRODUCER_CAPACITY={producer_capacity}",
                         str(test_c), f"/Fe:{executable}"]
        else:
            arguments = ["-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic",
                         f"-DTEST_LOWER_CAPACITY={lower_capacity}", f"-DTEST_PRODUCER_CAPACITY={producer_capacity}",
                         str(test_c), "-o", str(executable)]
        built = subprocess.run(compiler + arguments, cwd=work, env=environment,
                               capture_output=True, text=True, timeout=60)
        if built.returncode != 0:
            raise AssertionError(built.stdout + built.stderr)
        return subprocess.run([str(executable)], cwd=work, env=environment,
                              capture_output=True, text=True, timeout=10)


class ActualTxLivenessRuntimeTest(unittest.TestCase):
    def test_saturated_tx_recovers_without_new_admission(self) -> None:
        if compiler_command() is None:
            if os.environ.get("ESB_REQUIRE_C_COMPILER") == "1":
                self.fail("A host C compiler is required for the TX liveness test")
            self.skipTest("Set CC to execute the TX liveness test")
        for lower, producer in ((4, 3), (64, 128)):
            with self.subTest(lower_capacity=lower, producer_capacity=producer):
                run = compile_and_run(fixture_source(), lower_capacity=lower, producer_capacity=producer)
                self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
                self.assertIn("FIFO preservation and bounded pressure passed", run.stdout)
        negative = compile_and_run(fixture_source(remove_pump=True))
        self.assertNotEqual(negative.returncode, 0)
        self.assertIn("full_queue_write_failure", negative.stderr)
        # Reach the independent start failure in the same source fixture.
        negative_start = compile_and_run(fixture_source(remove_pump=True).replace(
            "    full_queues_recover(false);\n", "", 1))
        self.assertNotEqual(negative_start.returncode, 0)
        self.assertIn("full_queue_start_failure", negative_start.stderr)


if __name__ == "__main__":
    unittest.main()
