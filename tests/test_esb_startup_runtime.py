#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Execute real peripheral admission/drain callbacks during startup and pressure.

The fixture models only the mutex, bounded input queue and scheduled work. It
does not model the Zephyr scheduler, radio, crypto backend or USB enumeration.
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


def fixture_source(*, negative: str | None = None) -> str:
    source = (ROOT / "compat/zmk-feature-split-esb/src/split/esb/peripheral.c").read_text(
        encoding="utf-8"
    )
    functions = {}
    for name in ("report_event_locked", "split_peripheral_esb_report_event",
                 "flush_presession_work_cb", "key_state_work_cb"):
        functions[name] = braced_definition(
            source, rf"^static (?:int|void)\s+{name}\([^;{{}}]*\)\s*\{{"
        )
    if negative == "drop_on_enospc":
        previous = functions["report_event_locked"]
        functions["report_event_locked"] = previous.replace(" || err == -ENOSPC", "", 1)
        assert functions["report_event_locked"] != previous
    elif negative == "pop_failed_head":
        previous = functions["flush_presession_work_cb"]
        functions["flush_presession_work_cb"] = previous.replace(
            "if (err != 0)", "if (err != 0 && err != -ENOSPC)", 1
        )
        assert functions["flush_presession_work_cb"] != previous
    elif negative == "snapshot_overtakes_edges":
        previous = functions["key_state_work_cb"]
        functions["key_state_work_cb"] = previous.replace(
            "if (k_msgq_num_used_get(&presession_events) > 0)", "if (false)", 1
        )
        assert functions["key_state_work_cb"] != previous
    elif negative is not None:
        raise ValueError(negative)
    batch = re.search(r"^#define ESB_V3_PRESESSION_FLUSH_BATCH .+$", source, re.MULTILINE)
    assert batch is not None
    actual = batch.group(0) + "\n\n" + "\n\n".join(functions.values())
    fixture = (ROOT / "tests/esb_startup_runtime_fixture.c").read_text(encoding="utf-8")
    return fixture.replace("/* ACTUAL_FIRMWARE_FUNCTIONS */", actual)


class ActualStartupRuntimeTest(unittest.TestCase):
    def test_input_during_deferred_startup(self) -> None:
        compiler = compiler_command()
        if compiler is None:
            if os.environ.get("ESB_REQUIRE_C_COMPILER") == "1":
                self.fail("A host C compiler is required for the startup runtime test")
            self.skipTest("Set CC to execute the startup runtime test")

        environment = os.environ.copy()
        compiler_path = shutil.which(compiler[0]) or compiler[0]
        environment["PATH"] = (
            str(Path(compiler_path).resolve().parent) + os.pathsep
            + environment.get("PATH", "")
        )

        with tempfile.TemporaryDirectory(prefix="esb-startup-runtime-") as directory:
            workdir = Path(directory)
            test_c = workdir / "test_startup.c"
            executable = workdir / ("test_startup.exe" if os.name == "nt" else "test_startup")
            cases = [(4, None), (32, None), (4, "drop_on_enospc"),
                     (4, "pop_failed_head"), (4, "snapshot_overtakes_edges")]
            for capacity, negative in cases:
                with self.subTest(capacity=capacity, negative=negative):
                    test_c.write_text(fixture_source(negative=negative), encoding="utf-8")
                    if Path(compiler[0]).stem.lower() in ("cl", "clang-cl"):
                        arguments = ["/nologo", "/std:c11", "/W4", "/WX",
                                     f"/DQUEUE_CAPACITY={capacity}U", str(test_c), f"/Fe:{executable}"]
                    else:
                        arguments = [
                            "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic",
                            f"-DQUEUE_CAPACITY={capacity}U", str(test_c), "-o", str(executable),
                        ]
                    compiled = subprocess.run(
                        compiler + arguments, cwd=workdir, env=environment, text=True,
                        encoding="utf-8", errors="replace",
                        stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=60,
                    )
                    self.assertEqual(compiled.returncode, 0, compiled.stdout)
                    result = subprocess.run(
                        [str(executable)], env=environment, text=True,
                        encoding="utf-8", errors="replace",
                        stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=10,
                    )
                    if negative is None:
                        self.assertEqual(result.returncode, 0, result.stdout)
                        self.assertIn("11 peripheral startup and backpressure input cases passed", result.stdout)
                    else:
                        self.assertNotEqual(result.returncode, 0, result.stdout)
                        self.assertIn("startup assertion failed:", result.stdout)


if __name__ == "__main__":
    unittest.main()
