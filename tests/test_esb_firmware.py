#!/usr/bin/env python3
# Copyright (c) 2026 asj9005
# SPDX-License-Identifier: MIT
"""Compile and execute the same portable C helpers used by the firmware.

These tests complement the protocol models: the C under test is included from
the firmware headers, not translated into a second Python implementation.
Set ESB_REQUIRE_C_COMPILER=1 in CI so an absent compiler is a failure. Locally,
CC may name a C compiler command; otherwise cc, clang, gcc and cl are searched.
The tests exercise host-side state handling, not the radio or PSA backend.
"""

from __future__ import annotations

import os
from pathlib import Path
import shlex
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]


def compiler_command() -> list[str] | None:
    configured = os.environ.get("CC")
    if configured:
        if Path(configured).is_file():
            return [configured]
        command = shlex.split(configured, posix=os.name != "nt")
        if os.name == "nt":
            command = [part.strip('"') for part in command]
        if command and shutil.which(command[0]):
            return command
        raise RuntimeError(f"CC does not name an available compiler: {configured}")
    for candidate in ("cc", "clang", "gcc", "cl"):
        found = shutil.which(candidate)
        if found:
            return [found]
    return None


class FirmwareCHelperTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        compiler = compiler_command()
        if compiler is None:
            message = "No host C compiler found; set CC to run firmware C regressions"
            if os.environ.get("ESB_REQUIRE_C_COMPILER") == "1":
                raise RuntimeError(message)
            raise unittest.SkipTest(message)

        directory = tempfile.TemporaryDirectory(prefix="totem-esb-c-tests-")
        cls.addClassCleanup(directory.cleanup)
        build_dir = Path(directory.name)
        cls.executables: list[tuple[str, Path]] = []
        source = ROOT / "tests" / "test_esb_firmware.c"
        include = ROOT / "include"
        cls.execution_env = os.environ.copy()
        # Standalone MinGW installs also need their runtime DLL directory when
        # invoking cc1 and the resulting executable via an absolute CC path.
        compiler_path = shutil.which(compiler[0]) or compiler[0]
        cls.execution_env["PATH"] = (
            str(Path(compiler_path).resolve().parent) + os.pathsep
            + cls.execution_env.get("PATH", "")
        )
        for variant, slots, payload in (("small_queue", 6, 48), ("production_queue", 64, 64)):
            executable = build_dir / (variant + (".exe" if os.name == "nt" else ""))
            definitions = [
                f"CONFIG_ZMK_SPLIT_ESB_PROTO_MSGQ_ITEMS={slots}",
                f"CONFIG_ESB_MAX_PAYLOAD_LENGTH={payload}",
            ]
            if Path(compiler[0]).stem.lower() in ("cl", "clang-cl"):
                arguments = [
                    "/nologo", "/std:c11", "/W4", "/WX", f"/I{include}",
                    *(f"/D{definition}" for definition in definitions),
                    str(source), f"/Fe:{executable}",
                ]
            else:
                arguments = [
                    "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic",
                    *(f"-D{definition}" for definition in definitions),
                    "-I", str(include), str(source), "-o", str(executable),
                ]
            result = subprocess.run(
                compiler + arguments, cwd=build_dir, text=True,
                encoding="utf-8", errors="replace",
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=60,
                env=cls.execution_env,
            )
            if result.returncode:
                raise RuntimeError(
                    f"C compilation failed for {variant} ({result.returncode}):\n{result.stdout}"
                )
            cls.executables.append((variant, executable))

    def test_firmware_helpers(self) -> None:
        for variant, executable in self.executables:
            listing = subprocess.run(
                [str(executable), "--list"], check=True, text=True,
                encoding="utf-8", errors="replace",
                capture_output=True, timeout=10, env=self.execution_env,
            )
            cases = listing.stdout.splitlines()
            self.assertTrue(cases, "C harness did not register any tests")
            for case in cases:
                with self.subTest(variant=variant, case=case):
                    result = subprocess.run(
                        [str(executable), case], text=True,
                        encoding="utf-8", errors="replace",
                        stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=10,
                        env=self.execution_env,
                    )
                    self.assertEqual(result.returncode, 0, result.stdout)


if __name__ == "__main__":
    unittest.main()
