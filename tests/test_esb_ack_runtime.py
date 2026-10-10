#!/usr/bin/env python3
# Copyright (c) 2026 asj9005
# SPDX-License-Identifier: MIT
"""Execute the actual SDK ACK cancellation functions with deterministic IRQ stubs."""

from __future__ import annotations

import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest

from tests.test_esb_firmware import compiler_command


ROOT = Path(__file__).resolve().parents[1]


def braced_definition(source: str, declaration: str) -> str:
    """Extract unchanged C text, ignoring braces inside strings and comments."""
    match = re.search(declaration, source, re.MULTILINE)
    if match is None:
        raise AssertionError(f"SDK declaration missing: {declaration}")
    brace_start = match.end() - 1
    depth = 0
    tokens = re.finditer(
        r'/\*[\s\S]*?\*/|//[^\n]*|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|[{}]',
        source[brace_start:],
    )
    for token in tokens:
        if token.group() == "{":
            depth += 1
        elif token.group() == "}":
            depth -= 1
            if depth == 0:
                return source[match.start():brace_start + token.end()]
    raise AssertionError(f"Unclosed SDK declaration: {declaration}")


class ActualAckRuntimeTest(unittest.TestCase):
    def test_ack_count_and_cancellation(self) -> None:
        compiler = compiler_command()
        if compiler is None:
            if os.environ.get("ESB_REQUIRE_C_COMPILER") == "1":
                self.fail("A host C compiler is required for the SDK ACK runtime test")
            self.skipTest("Set CC to execute the SDK ACK runtime test")

        source = (ROOT / "compat/sdk-nrf/subsys/esb/esb.c").read_text(encoding="utf-8")
        structures = "\n\n".join(
            braced_definition(source, rf"^struct {name}\s*\{{") + ";"
            for name in ("pipe_info", "payload_wrap", "payload_tx_fifo")
        )
        functions = "\n\n".join(
            braced_definition(source, rf"^int {name}\([^;{{}}]*\)\s*\{{")
            for name in ("esb_get_ack_payload_count", "esb_flush_ack_payloads")
        )
        fixture = (ROOT / "tests/esb_ack_runtime_fixture.c").read_text(encoding="utf-8")
        fixture = fixture.replace("/* ACTUAL_SDK_STRUCTURES */", structures)
        fixture = fixture.replace("/* ACTUAL_SDK_FUNCTIONS */", functions)
        environment = os.environ.copy()
        compiler_path = shutil.which(compiler[0]) or compiler[0]
        environment["PATH"] = (
            str(Path(compiler_path).resolve().parent) + os.pathsep
            + environment.get("PATH", "")
        )
        with tempfile.TemporaryDirectory(prefix="esb-ack-runtime-") as directory:
            workdir = Path(directory)
            test_c = workdir / "test_ack.c"
            executable = workdir / ("test_ack.exe" if os.name == "nt" else "test_ack")
            test_c.write_text(fixture, encoding="utf-8")
            if Path(compiler[0]).stem.lower() in ("cl", "clang-cl"):
                arguments = ["/nologo", "/std:c11", "/W4", "/WX", str(test_c), f"/Fe:{executable}"]
            else:
                arguments = [
                    "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic",
                    str(test_c), "-o", str(executable),
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
            self.assertEqual(result.returncode, 0, result.stdout)
            self.assertIn("7 SDK ACK runtime cases passed", result.stdout)


if __name__ == "__main__":
    unittest.main()
