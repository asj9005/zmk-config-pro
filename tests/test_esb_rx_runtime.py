#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Run actual RX allocation/admission/IRQ C against deterministic ring fakes.

The worker's unchanged pipe-selection code is compiled with protocol processing
replaced by a visit assertion. Existing protocol tests cover that processing.
This does not model concurrent IRQ scheduling or measure hardware stack usage.
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
ESB = ROOT / "compat/zmk-feature-split-esb/src/split/esb"


def fixture_source(role: str) -> str:
    source = (ESB / ("central.c" if role == "central" else "peripheral.c")).read_text(encoding="utf-8")
    common = (ESB / "common.c").read_text(encoding="utf-8")
    common_h = (ESB / "common.h").read_text(encoding="utf-8")
    app = (ESB / "app_esb.c").read_text(encoding="utf-8")
    app_h = (ESB / "app_esb.h").read_text(encoding="utf-8")
    event_types = app_h[app_h.index("typedef enum {"):app_h.index("typedef struct {\n    uint8_t pipe;")]
    storage = "\n".join(re.findall(
        r"^#define RX_RING_BUF_SIZE[^\n]*$|^struct ring_buf rx_bufs\[[^;]+;|"
        r"^uint8_t rx_bufs_data\[[^;]+;|^static const uint8_t peripheral_id[^;]+;",
        source, re.MULTILINE,
    ))
    init = braced_definition(source, r"^static void init_rx_buffers\(void\)\s*\{")
    callback = braced_definition(common, r"^void zmk_split_esb_cb\([^;{}]*\)\s*\{")
    worker = braced_definition(source, r"^static void process_rx_work_cb\([^;{}]*\)\s*\{")
    processing = braced_definition(worker, r"^        while \(ring_buf_size_get\([^\n]+\)\s*\{")
    worker = worker.replace(processing, """        {
            CHECK(ring_buf_capacity_get(rx_buf) == RX_RING_BUF_SIZE);
            worker_mask |= 1U << pipe;
            goto next_pipe;
        }""")
    # The peripheral handler historically does not consume its work argument.
    worker = worker.replace("{\n", "{\n    ARG_UNUSED(work);\n", 1)
    rx_case = re.search(r"        case ESB_EVENT_RX_RECEIVED:\n([\s\S]*?)\n            break;", app)
    if rx_case is None:
        raise AssertionError("Actual radio RX dispatch case missing")
    dispatch = "static void receive_from_radio(void) {\n    app_esb_event_t m_event = {0};\n" + rx_case[1] + "\n}"
    fixture = (ROOT / "tests/esb_rx_runtime_fixture.c").read_text(encoding="utf-8")
    replacements = {
        "ACTUAL_APP_EVENT_TYPES": event_types,
        "ACTUAL_TRANSPORT_STATE": braced_definition(common_h, r"^struct zmk_split_esb_state\s*\{") + ";",
        "ACTUAL_RX_STORAGE": storage,
        "ACTUAL_RX_INIT": init,
        "ACTUAL_COMMON_CALLBACK": callback,
        "ACTUAL_WORKER_ROUTING": worker,
        "ACTUAL_RADIO_RX_DISPATCH": dispatch,
    }
    for marker, contents in replacements.items():
        fixture = fixture.replace(f"/* {marker} */", contents)
    return fixture


class ActualRxRuntimeTest(unittest.TestCase):
    def test_rx_storage_routing_and_lifetime(self) -> None:
        compiler = compiler_command()
        if compiler is None:
            if os.environ.get("ESB_REQUIRE_C_COMPILER") == "1":
                self.fail("A host C compiler is required for the RX runtime test")
            self.skipTest("Set CC to execute the RX runtime test")
        environment = os.environ.copy()
        compiler_path = shutil.which(compiler[0]) or compiler[0]
        environment["PATH"] = str(Path(compiler_path).resolve().parent) + os.pathsep + environment.get("PATH", "")
        variants = [
            ("central", 0, 3, False), ("central", 0, 5, False),
            ("left", 1, 3, False), ("right", 2, 3, False),
            ("left_without_admission_guard", 1, 3, True),
        ]
        with tempfile.TemporaryDirectory(prefix="esb-rx-runtime-") as directory:
            workdir = Path(directory)
            for name, peripheral_id, pipe_count, negative in variants:
                with self.subTest(variant=name):
                    generated = fixture_source("central" if name == "central" else "peripheral")
                    if negative:
                        self.assertIn("ring_buf_capacity_get(rx_buf) == 0", generated)
                        generated = generated.replace("ring_buf_capacity_get(rx_buf) == 0", "false", 1)
                    test_c = workdir / f"{name}_{pipe_count}.c"
                    executable = test_c.with_suffix(".exe" if os.name == "nt" else ".out")
                    test_c.write_text(generated, encoding="utf-8")
                    definitions = [f"TEST_CENTRAL={int(name == 'central')}",
                                   f"CONFIG_ZMK_SPLIT_ESB_PERIPHERAL_ID={peripheral_id}",
                                   f"CONFIG_ESB_PIPE_COUNT={pipe_count}"]
                    if Path(compiler[0]).stem.lower() in ("cl", "clang-cl"):
                        arguments = ["/nologo", "/std:c11", "/W4", "/WX",
                                     *(f"/D{item}" for item in definitions), str(test_c), f"/Fe:{executable}"]
                    else:
                        arguments = ["-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic",
                                     *(f"-D{item}" for item in definitions), str(test_c), "-o", str(executable)]
                    compiled = subprocess.run(compiler + arguments, cwd=workdir, env=environment,
                                              text=True, encoding="utf-8", errors="replace",
                                              stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=60)
                    self.assertEqual(compiled.returncode, 0, compiled.stdout)
                    result = subprocess.run([str(executable)], env=environment, text=True,
                                            encoding="utf-8", errors="replace",
                                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=10)
                    if negative:
                        self.assertNotEqual(result.returncode, 0, "Missing RX admission guard was not detected")
                        self.assertIn("ring->buffer != NULL", result.stdout)
                    else:
                        self.assertEqual(result.returncode, 0, result.stdout)
                        self.assertIn("buffer lifetime checks passed", result.stdout)


if __name__ == "__main__":
    unittest.main()
