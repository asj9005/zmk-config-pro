#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Actual RX admission/decode/dequeue/bounded worker, with ring/PSA/work fakes.

Only the worker's protocol dispatch switch is replaced by a recorder. Packet
validation, key-stage selection and peripheral RX mutex ownership are actual C;
crypto results and a producer rekey during a PSA wait are controlled. No real
IRQ preemption, scheduler, radio or hardware cryptography is modeled.
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
    header = (ESB / "common.h").read_text(encoding="utf-8")
    app = (ESB / "app_esb.c").read_text(encoding="utf-8")
    app_h = (ESB / "app_esb.h").read_text(encoding="utf-8")
    worker = braced_definition(source, r"^static void process_rx_work_cb\([^;{}]*\)\s*\{")
    dispatch = braced_definition(worker, r"^        switch \(item_err\)\s*\{")
    worker = worker.replace(dispatch, """        switch (item_err) {
        case 0: record_dispatch(pipe, &env); break;
        default: worker_errors++; break;
        }""")
    storage = "\n".join(re.findall(
        r"^#define RX_RING_BUF_SIZE[^\n]*$|^static struct ring_buf rx_buf;|"
        r"^static uint8_t rx_buf_data\[[^;]+;|^static const uint8_t peripheral_id[^;]+;", source, re.M))
    sizes = re.search(r"#if ESB_MSG_HAS_POSTFIX\n[\s\S]*?\n#endif", source)
    radio = re.search(r"        case ESB_EVENT_RX_RECEIVED:\n([\s\S]*?)\n            break;", app)
    assert sizes and radio
    functions = "\n\n".join(braced_definition(common, pattern) for pattern in (
        r"^void zmk_split_esb_cb\([^;{}]*\)\s*\{",
        r"^static inline int diag_frame_result\([^;{}]*\)\s*\{",
        r"^static int decode_rx_packet\([^;{}]*\)\s*\{",
        r"^static int64_t rx_record_timestamp\([^;{}]*\)\s*\{",
        r"^static bool rx_record_peek_valid\([^;{}]*\)\s*\{",
        r"^static void rx_record_reset\([^;{}]*\)\s*\{",
        r"^bool zmk_split_esb_rx_pending_before\([^;{}]*\)\s*\{",
        r"^int zmk_split_esb_rx_get\([^;{}]*\)\s*\{",
    ))
    central = (ESB / "central.c").read_text(encoding="utf-8")
    replacements = {
        "APP_EVENT_TYPES": app_h[app_h.index("typedef enum {"):app_h.index("typedef struct {\n    uint8_t pipe;")],
        "WIRE_TYPES": header[header.index("#if IS_ENABLED(CONFIG_TOTEM_ESB_V3)"):
                             header.index("typedef void (*zmk_split_esb_process_rx_callback_t)")],
        "RX_RECORD": braced_definition(header, r"^struct esb_rx_record\s*\{") + " __packed;",
        "BATCH_SIZE": re.search(r"^#define ESB_RX_WORK_BATCH_SIZE[^\n]*", header, re.M).group(0),
        "TRANSPORT_STATE": braced_definition(header, r"^struct zmk_split_esb_state\s*\{") + ";",
        "RX_STORAGE": sizes.group(0) + "\n" + storage,
        "RX_INIT": braced_definition(source, r"^static void init_rx_buffers\(void\)\s*\{"),
        "STATE_INIT": braced_definition(source, r"^static struct zmk_split_esb_state state =\s*\{") + ";",
        "RX_FUNCTIONS": functions,
        "WORKER": worker,
        "RADIO_DISPATCH": "static void receive_from_radio(void) {\n    app_esb_event_t m_event = {0};\n" + radio[1] + "\n}",
        "POSITION_VALIDATION": braced_definition(central, r"^static bool event_payload_size_is_valid\([^;{}]*\)\s*\{"),
    }
    fixture = (ROOT / "tests/esb_rx_runtime_fixture.c").read_text(encoding="utf-8")
    for marker, contents in replacements.items():
        fixture = fixture.replace(f"/* ACTUAL_{marker} */", contents)
    assert "/* ACTUAL_" not in fixture
    return fixture


class ActualRxRuntimeTest(unittest.TestCase):
    def test_rx_fifo_frames_fairness_and_lifetime(self) -> None:
        compiler = compiler_command()
        if compiler is None:
            if os.environ.get("ESB_REQUIRE_C_COMPILER") == "1":
                self.fail("A host C compiler is required for the RX runtime test")
            self.skipTest("Set CC to execute the RX runtime test")
        environment = os.environ.copy()
        environment["PATH"] = str(Path(shutil.which(compiler[0]) or compiler[0]).resolve().parent) + os.pathsep + environment.get("PATH", "")
        variants = [
            ("central_v3", 0, 3, 1, 1, None), ("central_extra_pipes", 0, 5, 1, 1, None),
            ("central_v2", 0, 3, 0, 1, None), ("central_no_crc", 0, 3, 0, 0, None),
            ("left", 1, 3, 1, 1, None), ("right", 2, 3, 1, 1, None),
            ("left_v2", 1, 3, 0, 1, None),
            ("no_budget", 0, 3, 1, 1, "budget"), ("no_quota", 0, 3, 1, 1, "quota"),
            ("no_exact_length", 0, 3, 1, 1, "length"),
            ("left_no_rx_lock", 1, 3, 1, 1, "rx_lock"),
        ]
        mutations = {
            "budget": ("processed < ESB_RX_WORK_BATCH_SIZE", "processed < 10000U", "bounded_worker"),
            "quota": ("state->rx_pipe_bytes[event->pipe] + record_size >\n"
                      "                    state->rx_pipe_capacity", "false", "per_pipe_quota"),
            "length": ("if (packet_size != (payload_to_read", "if (packet_size < (payload_to_read", "packet_boundaries"),
        }
        with tempfile.TemporaryDirectory(prefix="esb-rx-runtime-") as directory:
            work = Path(directory)
            for name, peer, pipes, v3, crc, negative in variants:
                with self.subTest(variant=name):
                    generated = fixture_source("central" if peer == 0 else "peripheral")
                    if negative == "rx_lock":
                        for call in ("        k_mutex_lock(&event_mutex, K_FOREVER);",
                                     "        k_mutex_unlock(&event_mutex);"):
                            self.assertIn(call, generated)
                            generated = generated.replace(call, "", 1)
                        failure = "rx_rekey_exclusion"
                    elif negative:
                        old, new, failure = mutations[negative]
                        self.assertIn(old, generated)
                        generated = generated.replace(old, new, 1)
                    path = work / f"{name}.c"
                    executable = path.with_suffix(".exe" if os.name == "nt" else ".out")
                    path.write_text(generated, encoding="utf-8")
                    defs = [f"TEST_CENTRAL={int(peer == 0)}", f"CONFIG_ZMK_SPLIT_ESB_PERIPHERAL_ID={peer}",
                            f"CONFIG_ESB_PIPE_COUNT={pipes}", f"CONFIG_TOTEM_ESB_V3={v3}",
                            f"CONFIG_ZMK_SPLIT_ESB_MSG_POSTFIX_CRC={crc}"]
                    if Path(compiler[0]).stem.lower() in ("cl", "clang-cl"):
                        args = ["/nologo", "/std:c11", "/W4", "/WX", f"/I{ROOT / 'include'}",
                                *(f"/D{x}" for x in defs), str(path), f"/Fe:{executable}"]
                    else:
                        args = ["-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic", "-I", str(ROOT / "include"),
                                *(f"-D{x}" for x in defs), str(path), "-o", str(executable)]
                    compiled = subprocess.run(compiler + args, cwd=work, env=environment,
                                              capture_output=True, text=True, timeout=60)
                    self.assertEqual(compiled.returncode, 0, compiled.stdout + compiled.stderr)
                    result = subprocess.run([str(executable)], env=environment, capture_output=True, text=True, timeout=10)
                    output = result.stdout + result.stderr
                    if negative:
                        self.assertNotEqual(result.returncode, 0, f"Mutation {negative} was not detected")
                        self.assertIn(failure, output)
                    else:
                        self.assertEqual(result.returncode, 0, output)
                        self.assertIn("11 actual RX scenarios passed", output)


if __name__ == "__main__":
    unittest.main()
