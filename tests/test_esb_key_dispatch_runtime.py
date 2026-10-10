#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Execute actual central wire dispatch and key-state recovery at a fake ZMK boundary."""
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


class ActualKeyDispatchRuntimeTest(unittest.TestCase):
    def test_wire_edges_snapshots_and_cleanup(self) -> None:
        compiler = compiler_command()
        if compiler is None:
            if os.environ.get("ESB_REQUIRE_C_COMPILER") == "1":
                self.fail("A host C compiler is required for the key dispatch runtime test")
            self.skipTest("Set CC to execute the key dispatch runtime test")
        source = (ROOT / "compat/zmk-feature-split-esb/src/split/esb/central.c").read_text(
            encoding="utf-8"
        )
        helpers = "\n\n".join(
            braced_definition(source, rf"^static void\s+{name}\([^;{{}}]*\)\s*\{{")
            for name in ("release_source_keys", "emit_snapshot_key", "dispatch_wire_zmk_event")
        )
        fixture = (ROOT / "tests/esb_key_dispatch_runtime_fixture.c").read_text(encoding="utf-8")
        generated = fixture.replace("/* ACTUAL_CENTRAL_HELPERS */", helpers)
        self.assertEqual(generated.count("if (!was_pressed)"), 1)
        environment = os.environ.copy()
        compiler_path = shutil.which(compiler[0]) or compiler[0]
        environment["PATH"] = str(Path(compiler_path).resolve().parent) + os.pathsep + environment.get("PATH", "")
        with tempfile.TemporaryDirectory(prefix="esb-key-dispatch-") as directory:
            work = Path(directory)
            for negative in (None, "orphan", "position", "source", "snapshot_time"):
                with self.subTest(removed_guard=negative):
                    test_c = work / f"guard_{negative}.c"
                    executable = work / (test_c.stem + (".exe" if os.name == "nt" else ""))
                    variant = generated
                    if negative == "orphan":
                        variant = variant.replace("if (!was_pressed)", "if (false)", 1)
                    elif negative == "position":
                        variant = variant.replace(
                            "if (position >= CONFIG_ZMK_SPLIT_ESB_AUTO_HEAL_KEY_POS_MAX ||\n"
                            "            position >= ZMK_KEYMAP_LEN)", "if (false)", 1)
                    elif negative == "source":
                        variant = variant.replace("if (source >= CONFIG_ZMK_SPLIT_ESB_PERIPHERAL_COUNT)", "if (false)", 1)
                    elif negative == "snapshot_time":
                        variant = variant.replace("    event.position = position;",
                            "    event.timestamp = k_uptime_get();\n    event.position = position;", 1)
                    if negative:
                        self.assertNotEqual(variant, generated)
                    test_c.write_text(variant, encoding="utf-8")
                    if Path(compiler[0]).stem.lower() in ("cl", "clang-cl"):
                        arguments = ["/nologo", "/std:c11", "/W4", "/WX", f"/I{ROOT / 'include'}",
                                     str(test_c), f"/Fe:{executable}"]
                    else:
                        arguments = ["-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic",
                                     "-I", str(ROOT / "include"), str(test_c), "-o", str(executable)]
                    build = subprocess.run(compiler + arguments, cwd=work, env=environment,
                                           capture_output=True, text=True, timeout=60)
                    self.assertEqual(build.returncode, 0, build.stdout + build.stderr)
                    run = subprocess.run([str(executable)], cwd=work, env=environment,
                                         capture_output=True, text=True, timeout=10)
                    if negative:
                        self.assertNotEqual(run.returncode, 0)
                        expected = ("orphan_release_stays_idle" if negative == "orphan" else
                                    "wire_and_snapshot_keep_ingress_order" if negative == "snapshot_time" else
                                    "invalid_source_and_position_stay_outside_zmk")
                        self.assertIn(expected, run.stderr)
                    else:
                        self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
                        self.assertIn("9 actual central dispatch/snapshot/cleanup scenarios passed", run.stdout)


if __name__ == "__main__":
    unittest.main()
