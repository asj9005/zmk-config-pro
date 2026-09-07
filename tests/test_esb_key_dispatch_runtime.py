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
            for negative in (False, True):
                with self.subTest(orphan_guard=not negative):
                    test_c = work / ("without_guard.c" if negative else "with_guard.c")
                    executable = work / (test_c.stem + (".exe" if os.name == "nt" else ""))
                    test_c.write_text(
                        generated.replace("if (!was_pressed)", "if (false)", 1) if negative else generated,
                        encoding="utf-8",
                    )
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
                        self.assertIn("orphan_release_stays_idle", run.stderr)
                    else:
                        self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
                        self.assertIn("7 actual central dispatch/snapshot/cleanup scenarios passed", run.stdout)


if __name__ == "__main__":
    unittest.main()
