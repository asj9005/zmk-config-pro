#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Compile the actual pinned hold-tap overlay at a synchronous fake ZMK boundary.

Only #include directives are removed from the C under test. Kernel time/work,
device lookup and event routing are host fakes; this is not an ARM scheduler,
radio or USB test. Actual AutoBase and central dispatch helpers are included
to check replay order across those boundaries rather than a Python model.
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


class ActualHoldTapRuntimeTests(unittest.TestCase):
    def test_capture_replay_and_overload(self) -> None:
        compiler = compiler_command()
        if compiler is None:
            if os.environ.get("ESB_REQUIRE_C_COMPILER") == "1":
                self.fail("A host C compiler is required for hold-tap runtime tests")
            self.skipTest("Set CC to execute hold-tap runtime tests")
        source = (ROOT / "compat/zmk/app/src/behaviors/behavior_hold_tap.c").read_text(encoding="utf-8")
        source = re.sub(r"^#include[^\n]*\n", "", source, flags=re.MULTILINE)
        central = (ROOT / "compat/zmk-feature-split-esb/src/split/esb/central.c").read_text(encoding="utf-8")
        central_helpers = "\n\n".join(
            braced_definition(central, rf"^static void\s+{name}\([^;{{}}]*\)\s*\{{")
            for name in ("release_source_keys", "emit_snapshot_key", "dispatch_wire_zmk_event")
        )
        auto_base = (ROOT / "src/behavior_auto_base.c").read_text(encoding="utf-8")
        auto_helpers = "\n\n".join(
            braced_definition(auto_base, rf"^static int\s+{name}\([^;{{}}]*\)\s*\{{")
            for name in ("auto_base_pressed", "auto_base_released")
        )
        fixture = (ROOT / "tests/hold_tap_runtime_fixture.c").read_text(encoding="utf-8")
        environment = os.environ.copy()
        compiler_path = shutil.which(compiler[0]) or compiler[0]
        environment["PATH"] = str(Path(compiler_path).resolve().parent) + os.pathsep + environment.get("PATH", "")
        variants = {
            "current": source,
            "unchecked_position_capture": source.replace("if (capture_event(&capture) != 0)",
                                                 "if ((capture_event(&capture), false))", 1),
            "unchecked_modifier_capture": "if ((capture_event(&capture), false))".join(
                source.rsplit("if (capture_event(&capture) != 0)", 1)),
            "replay_sleep": source.replace("captured_events[i].tag = ET_NONE;",
                "captured_events[i].tag = ET_NONE; if (undecided_hold_tap != NULL) k_msleep(10);", 1),
            "shared_replay_slot": source.replace(
                "struct captured_event captured_event = captured_events[i];",
                "struct captured_event *captured_event = &captured_events[i];").replace(
                    "captured_event.tag", "captured_event->tag").replace(
                    "captured_event.data", "captured_event->data"),
            "unchecked_held_slots": source.replace(
                "zmk_behavior_invoke_binding(&tap, event, true);", "(void)tap;").replace(
                "zmk_behavior_invoke_binding(&tap, event, false);", "(void)event;"),
        }
        for name, candidate in variants.items():
            if name != "current":
                self.assertNotEqual(candidate, source, name)
        with tempfile.TemporaryDirectory(prefix="totem-hold-tap-") as directory:
            work = Path(directory)
            # Relative filenames avoid MinGW's handling of non-ASCII source paths.
            shutil.copyfile(ROOT / "include/totem/esb_key_state.h", work / "esb_key_state.h")
            shutil.copyfile(ROOT / "include/totem/esb_diagnostics.h", work / "esb_diagnostics.h")
            for variant, actual in variants.items():
                with self.subTest(variant=variant):
                    generated = fixture.replace("/* ACTUAL_HOLD_TAP_SOURCE */", actual)
                    generated = generated.replace("/* ACTUAL_CENTRAL_HELPERS */", central_helpers)
                    generated = generated.replace("/* ACTUAL_AUTO_BASE_HELPERS */", auto_helpers)
                    test_c = variant + ".c"
                    executable = variant + (".exe" if os.name == "nt" else "")
                    (work / test_c).write_text(generated, encoding="utf-8")
                    if Path(compiler[0]).stem.lower() in ("cl", "clang-cl"):
                        arguments = ["/nologo", "/std:c11", "/W4", test_c, f"/Fe:{executable}"]
                    else:
                        arguments = ["-std=gnu11", "-Wall", "-Wextra", "-Werror",
                                     "-Wno-unused-function", "-Wno-unused-parameter",
                                     "-Wno-unused-const-variable", "-Wno-sign-compare",
                                     test_c, "-o", executable]
                    build = subprocess.run(compiler + arguments, cwd=work, env=environment,
                                           capture_output=True, text=True, timeout=60)
                    self.assertEqual(build.returncode, 0, build.stdout + build.stderr)
                    run = subprocess.run([str(work / executable)], cwd=work, env=environment,
                                         capture_output=True, text=True, timeout=10)
                    if variant == "current":
                        self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
                        self.assertIn("actual hold-tap scenarios passed", run.stdout)
                        print(run.stdout.strip())
                    else:
                        self.assertNotEqual(run.returncode, 0, "Negative control unexpectedly passed")
                        self.assertIn("CHECK failed", run.stderr)


if __name__ == "__main__":
    unittest.main()
