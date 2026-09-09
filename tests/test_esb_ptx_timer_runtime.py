#!/usr/bin/env python3
# Copyright (c) 2026 asj9005
# SPDX-License-Identifier: MIT
"""Run actual SDK PTX functions against a deterministic timer/PPI model.

This exercises the non-FEM, 2 Mbps nRF52840 path and delayed RADIO servicing;
it does not emulate RF reception, nrfx IRQ latency, hardware, or Errata 216.
"""

from __future__ import annotations

import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest

from tests.test_esb_ack_runtime import braced_definition
from tests.test_esb_firmware import compiler_command


ROOT = Path(__file__).resolve().parents[1]


def fixture_source(mutation: str | None = None) -> str:
    source = (ROOT / "compat/sdk-nrf/subsys/esb/esb.c").read_text(encoding="utf-8")
    names = (
        "esb_fem_for_tx_set", "esb_fem_pa_reset", "esb_fem_for_rx_ack",
        "esb_fem_for_tx_retry", "esb_fem_for_tx_retry_clear", "esb_timer_handler",
        "on_radio_end_tx_noack", "on_radio_disabled_tx_noack",
        "on_radio_disabled_tx", "on_radio_disabled_tx_wait_for_ack",
    )
    definitions = {}
    for name in names:
        definitions[name] = braced_definition(
            source, rf"^(?:static )?void {name}\([^;{{}}]*\)\s*\{{"
        )
    if mutation == "disable_cc2_irq":
        definitions["esb_fem_for_tx_set"], count = re.subn(
            r"(nrfx_timer_compare\(&esb_timer,\s*NRF_TIMER_CC_CHANNEL2,\s*ramp_up,\s*)true(\s*\))",
            r"\1false\2", definitions["esb_fem_for_tx_set"],
        )
        assert count == 1, "The actual CC2 interrupt setup must be mutated"
    elif mutation == "restore_ack_clear":
        old = "nrfx_timer_compare(&esb_timer, NRF_TIMER_CC_CHANNEL0,"
        actual = definitions["on_radio_disabled_tx"]
        assert actual.count(old) == 1
        definitions["on_radio_disabled_tx"] = actual.replace(
            old, "nrf_timer_task_trigger(esb_timer.p_reg, NRF_TIMER_TASK_CLEAR);\n\t" + old
        )
    elif mutation is not None:
        raise ValueError(mutation)
    constants = []
    for name in ("RX_ACK_TIMEOUT_US_2MBPS", "TX_FAST_RAMP_UP_TIME_US",
                 "TX_RAMP_UP_TIME_US", "ADDR_EVENT_LATENCY_US"):
        match = re.search(rf"^#define {name}\s+[^\n]+", source, re.MULTILINE)
        assert match is not None, name
        constants.append(match.group())
    actual = "\n".join(constants) + "\n\n" + "\n\n".join(definitions.values())
    fixture = (ROOT / "tests/esb_ptx_timer_runtime_fixture.c").read_text(encoding="utf-8")
    assert fixture.count("/* ACTUAL_SDK_FUNCTIONS */") == 1
    return fixture.replace("/* ACTUAL_SDK_FUNCTIONS */", actual)


def compile_and_run(source: str, case: str | None = None):
    compiler = compiler_command()
    if compiler is None:
        raise RuntimeError("C compiler required")
    environment = os.environ.copy()
    compiler_path = shutil.which(compiler[0]) or compiler[0]
    environment["PATH"] = str(Path(compiler_path).resolve().parent) + os.pathsep + environment.get("PATH", "")
    with tempfile.TemporaryDirectory(prefix="esb-ptx-timer-") as directory:
        work = Path(directory)
        test_c = work / "fixture.c"
        executable = work / ("fixture.exe" if os.name == "nt" else "fixture")
        test_c.write_text(source, encoding="utf-8")
        if Path(compiler[0]).stem.lower() in ("cl", "clang-cl"):
            arguments = ["/nologo", "/std:c11", "/W4", "/WX", "/wd4100",
                         str(test_c), f"/Fe:{executable}"]
        else:
            arguments = ["-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
                         "-Wno-unused-parameter", str(test_c), "-o", str(executable)]
        built = subprocess.run(compiler + arguments, cwd=work, env=environment,
                               capture_output=True, text=True, timeout=60)
        if built.returncode != 0:
            raise AssertionError(built.stdout + built.stderr)
        return subprocess.run([str(executable)] + ([case] if case else []),
                              cwd=work, env=environment, capture_output=True,
                              text=True, timeout=10)


class ActualPtxTimerRuntimeTest(unittest.TestCase):
    def setUp(self) -> None:
        if compiler_command() is None:
            if os.environ.get("ESB_REQUIRE_C_COMPILER") == "1":
                self.fail("A host C compiler is required for the SDK PTX timer test")
            self.skipTest("Set CC to execute the SDK PTX timer runtime test")

    def test_delayed_radio_irq_and_timer_paths(self) -> None:
        result = compile_and_run(fixture_source())
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("5 SDK PTX timer runtime cases passed", result.stdout)

    def test_old_timer_mutations_fail(self) -> None:
        for mutation, case in (("disable_cc2_irq", "delayed_radio_irq"),
                               ("restore_ack_clear", "elapsed_ack_deadline")):
            with self.subTest(mutation=mutation):
                result = compile_and_run(fixture_source(mutation), case)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn(case + ":", result.stderr)


if __name__ == "__main__":
    unittest.main()
