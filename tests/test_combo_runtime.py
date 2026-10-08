#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Run the pinned combo state machine with bounded RX/workqueue boundary fakes.

The real combo structs, state and functions are compiled unchanged. Only the
devicetree table and external kernel/event APIs are supplied by this fixture;
the shared RX timestamp hook is separately exercised by hold-tap runtime tests.
"""
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


def fixture_source(source: str) -> str:
    structures = "\n\n".join(braced_definition(source, rf"^struct {name}\s*\{{") + ";"
                              for name in ("combo_cfg", "active_combo"))
    body = source[source.index("uint8_t pressed_keys_count = 0;"):
                  source.index("static int position_state_changed_listener(")]
    init = braced_definition(source, r"^static int combo_init\(void\)\s*\{")
    fixture = (ROOT / "tests/combo_runtime_fixture.c").read_text(encoding="utf-8")
    return (fixture.replace("/* ACTUAL_STRUCTURES */", structures)
            .replace("/* ACTUAL_COMBO_BODY */", body + "\n" + init))


class ActualComboRuntimeTests(unittest.TestCase):
    def test_ingress_deadline_order_and_empty_candidates(self) -> None:
        compiler = compiler_command()
        if compiler is None:
            if os.environ.get("ESB_REQUIRE_C_COMPILER") == "1":
                self.fail("A host C compiler is required for combo runtime tests")
            self.skipTest("Set CC to execute combo runtime tests")
        source = (ROOT / "compat/zmk/app/src/combo.c").read_text(encoding="utf-8")
        variants = {
            "current": source,
            "timer_overtakes_rx": source.replace(
                "if (totem_esb_rx_pending_before(timeout_task_timeout_at))",
                "if (false && totem_esb_rx_pending_before(timeout_task_timeout_at))", 1),
            "new_traffic_extends_deadline": source.replace(
                "totem_esb_rx_pending_before(timeout_task_timeout_at)",
                "totem_esb_rx_pending_before(LLONG_MAX)", 1),
            "arm_empty_timeout": source.replace("return LLONG_MAX;", "return LONG_MAX;", 1),
            "no_candidates_adds_sentinel": source.replace(
                "return first_timeout == LLONG_MAX ? LLONG_MAX\n"
                "                                     : pressed_keys[0].data.timestamp + first_timeout;",
                "return pressed_keys[0].data.timestamp + first_timeout;", 1),
        }
        environment = os.environ.copy()
        compiler_path = shutil.which(compiler[0]) or compiler[0]
        environment["PATH"] = str(Path(compiler_path).resolve().parent) + os.pathsep + environment.get("PATH", "")
        with tempfile.TemporaryDirectory(prefix="totem-combo-") as directory:
            work = Path(directory)
            for variant, candidate in variants.items():
                with self.subTest(variant=variant):
                    if variant != "current":
                        self.assertNotEqual(candidate, source, variant)
                    generated = fixture_source(candidate)
                    self.assertNotIn("/* ACTUAL_", generated)
                    test_c = variant + ".c"
                    executable = variant + (".exe" if os.name == "nt" else "")
                    (work / test_c).write_text(generated, encoding="utf-8")
                    if Path(compiler[0]).stem.lower() in ("cl", "clang-cl"):
                        arguments = ["/nologo", "/std:c11", "/W4", test_c, f"/Fe:{executable}"]
                    else:
                        arguments = ["-std=gnu11", "-Wall", "-Wextra", "-Werror",
                                     "-Wno-unused-function", "-Wno-unused-parameter", "-Wno-sign-compare",
                                     test_c, "-o", executable]
                    build = subprocess.run(compiler + arguments, cwd=work, env=environment,
                                           capture_output=True, text=True, timeout=60)
                    self.assertEqual(build.returncode, 0, build.stdout + build.stderr)
                    run = subprocess.run([str(work / executable)], cwd=work, env=environment,
                                         capture_output=True, text=True, timeout=10)
                    if variant == "current":
                        self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
                        self.assertIn("actual combo scenarios passed", run.stdout)
                        print(run.stdout.strip())
                    else:
                        self.assertNotEqual(run.returncode, 0, "Negative control unexpectedly passed")
                        self.assertIn("CHECK failed", run.stderr)


if __name__ == "__main__":
    unittest.main()
