#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Public synthetic fixtures only: no entropy, real keys, radio or device access."""
from __future__ import annotations

import contextlib
import io
import json
from pathlib import Path
import struct
import tempfile
import unittest

from tools import verify_esb_v3_private_build as verifier

# Public, deterministic test bytes. These are deliberately not production keys.
TEST_LEFT = bytes(range(16))
TEST_RIGHT = bytes(range(16, 32))
TEST_KEYS = {"left": TEST_LEFT, "right": TEST_RIGHT}
STARTUP_FIXTURE = (b"ESB_DIAG DELAY_START delay_ms=",
                   b"ESB_DIAG AUTO_START transport_init",
                   b"ESB_DIAG AUTO_START transport_return")


def key_line(symbol, key):
    return symbol + '="' + key.hex() + '"\n'


def make_uf2(payload):
    payload += bytes(-len(payload) % 256)
    count = len(payload) // 256
    blocks = []
    for index in range(count):
        block = bytearray(512)
        struct.pack_into("<8I", block, 0, 0x0A324655, 0x9E5D5157, 0x2000,
                         0x27000 + index * 256, 256, index, count, 0xADA52840)
        block[32:288] = payload[index * 256:(index + 1) * 256]
        struct.pack_into("<I", block, 508, 0x0AB16F30)
        blocks.append(block)
    return b"".join(blocks)


def config_for(role):
    values = {
        "CONFIG_BOARD_TARGET": '"xiao_ble/nrf52840/zmk"', "CONFIG_SOC": '"nrf52840"',
        "CONFIG_SHIELD_TOTEM_ESB_V3": "y", "CONFIG_TOTEM_ESB_COMPAT": "y",
        "CONFIG_TOTEM_ESB_V3": "y", "CONFIG_TOTEM_ESB_DIAGNOSTICS": "y",
        "CONFIG_TOTEM_ESB_V3_CI_TEST_KEYS": "n", "CONFIG_ZMK_SPLIT": "y",
        "CONFIG_ZMK_SPLIT_ESB": "y", "CONFIG_ESB_MAX_PAYLOAD_LENGTH": "64",
        "CONFIG_SYSTEM_WORKQUEUE_STACK_SIZE": "4096", "CONFIG_INPUT_THREAD_STACK_SIZE": "4096",
        "CONFIG_SHIELD_TOTEM_" + role.upper(): "y",
        "CONFIG_SHIELD_TOTEM_ESB_" + role.upper(): "y",
    }
    if role == "dongle":
        values.update({"CONFIG_ZMK_SPLIT_ROLE_CENTRAL": "y",
                       "CONFIG_ZMK_SPLIT_ESB_PERIPHERAL_COUNT": "2", "CONFIG_ZMK_SPLIT_ESB_PERIPHERAL_ID": "0",
                       "CONFIG_TOTEM_ESB_PROSPECTOR": "y", "CONFIG_ZMK_LOW_PRIORITY_THREAD_STACK_SIZE": "768",
                       "CONFIG_LV_Z_VDB_SIZE": "25", "CONFIG_LV_Z_DOUBLE_VDB": "y", "CONFIG_ZMK_USB": "y",
                       "CONFIG_ZMK_STUDIO": "y", "CONFIG_USB_HID_POLL_INTERVAL_MS": "1",
                       verifier.LEFT: '"' + TEST_LEFT.hex() + '"', verifier.RIGHT: '"' + TEST_RIGHT.hex() + '"'})
    else:
        values.update({"CONFIG_ZMK_SPLIT_ESB_PERIPHERAL_COUNT": "0",
                       "CONFIG_ZMK_SPLIT_ESB_PERIPHERAL_ID": "1" if role == "left" else "2",
                       "CONFIG_ZMK_LOW_PRIORITY_THREAD_STACK_SIZE": "4096",
                       "CONFIG_TOTEM_ESB_DIAGNOSTIC_START_DELAY_MS": "3000",
                       verifier.LOCAL: '"' + TEST_KEYS[role].hex() + '"'})
    return "\n".join(symbol + "=" + value for symbol, value in values.items()) + "\n"


def header_for(role):
    keys = {"LOCAL": bytes(16), "LEFT": bytes(16), "RIGHT": bytes(16)}
    if role == "dongle":
        keys.update(LEFT=TEST_LEFT, RIGHT=TEST_RIGHT)
    else:
        keys["LOCAL"] = TEST_KEYS[role]
    return "/* Synthetic generated header */\n#pragma once\n" + "\n".join(
        "#define TOTEM_ESB_V3_" + symbol + "_KEY_BYTES " + ", ".join(f"0x{byte:02x}" for byte in key)
        for symbol, key in keys.items()) + "\n"


def payload_for(role):
    # The first key straddles a UF2 block boundary, requiring reassembly.
    payload = b"PUBLIC SYNTHETIC FIXTURE - NOT EXECUTABLE FIRMWARE\x00".ljust(250, b"!")
    payload += TEST_LEFT + TEST_RIGHT if role == "dongle" else TEST_KEYS[role]
    payload += b"\x00[esb-diag] role=%s\x00tx_steps=done:result\x00"
    payload += b"dongle\x00" if role == "dongle" else b"\x00".join(STARTUP_FIXTURE)
    return payload


class PrivateBuildVerifierTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="esb-public-verifier-fixture-")
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.keys = self.root / "public-fixture-keys"
        self.keys.mkdir()
        self.write_keys()

    def write_keys(self, left=TEST_LEFT, right=TEST_RIGHT):
        (self.keys / "left.keyconf").write_text(key_line(verifier.LOCAL, left))
        (self.keys / "right.keyconf").write_text(key_line(verifier.LOCAL, right))
        (self.keys / "dongle.keyconf").write_text(key_line(verifier.LEFT, left) + key_line(verifier.RIGHT, right))

    def build(self, role):
        build = self.root / role
        (build / "zephyr").mkdir(parents=True, exist_ok=True)
        header = build / "modules/totem-esb-compat/generated/totem/esb_v3_keys.h"
        header.parent.mkdir(parents=True, exist_ok=True)
        header.write_text(header_for(role))
        (build / "zephyr/.config").write_text(config_for(role))
        (build / "zephyr/zmk.uf2").write_bytes(make_uf2(payload_for(role)))
        return build

    def rejected(self, role=None, build=None):
        args = ["--key-dir", str(self.keys)]
        if role is not None:
            args += ["--role", role, "--build-dir", str(build)]
        output, errors = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(output), contextlib.redirect_stderr(errors):
            status = verifier.main(args)
        self.assertEqual(status, 2)
        self.assertFalse(json.loads(errors.getvalue())["verified"])
        self.assertEqual(output.getvalue(), "")
        for key in (TEST_LEFT, TEST_RIGHT, *verifier.PUBLIC_CI_KEYS):
            self.assertNotIn(key.hex(), errors.getvalue())
            self.assertNotIn(repr(key), errors.getvalue())

    def test_preflight_and_all_roles(self):
        summary = verifier.verify(self.keys)
        self.assertTrue(summary["key_set_matches"])
        for role in ("left", "right", "dongle"):
            with self.subTest(role=role):
                build = self.build(role)
                (build / "zephyr/zmk.bin").write_bytes(payload_for(role))
                result = verifier.verify(self.keys, role, build)
                self.assertEqual(result["role"], role)
                self.assertTrue(result["binary_checked"])
                self.assertTrue(result["uf2_role_keys_match"])
                for key in TEST_KEYS.values():
                    self.assertNotIn(key.hex(), json.dumps(result))

    def test_partial_or_mixed_key_set(self):
        (self.keys / "right.keyconf").unlink()
        self.rejected()
        self.write_keys()
        (self.keys / "dongle.keyconf").write_text(key_line(verifier.LEFT, TEST_RIGHT) + key_line(verifier.RIGHT, TEST_LEFT))
        self.rejected()

    def test_public_equal_and_zero_keys(self):
        for left, right in ((verifier.PUBLIC_CI_KEYS[0], TEST_RIGHT),
                            (TEST_LEFT, verifier.PUBLIC_CI_KEYS[1]),
                            (TEST_LEFT, TEST_LEFT), (bytes(16), TEST_RIGHT)):
            with self.subTest(case=left == right):
                self.write_keys(left, right)
                self.rejected()

    def test_duplicate_unknown_and_invalid_key_symbols(self):
        original = (self.keys / "left.keyconf").read_text()
        for contents in (original + original, original + "CONFIG_TOTEM_ESB_V3_CI_TEST_KEYS=n\n",
                         key_line(verifier.LEFT, TEST_LEFT),
                         'CONFIG_TOTEM_ESB_V3_LOCAL_KEY_HEX="bad"\n',
                         original + "invalid " + TEST_LEFT.hex()):
            (self.keys / "left.keyconf").write_text(contents)
            self.rejected()

    def test_untested_build_config_is_rejected(self):
        changes = {
            "left": [("STACK_SIZE=4096", "STACK_SIZE=768"),
                     ("START_DELAY_MS=3000", "START_DELAY_MS=0"),
                     ("DIAGNOSTICS=y", "DIAGNOSTICS=n"),
                     ("CI_TEST_KEYS=n", "CI_TEST_KEYS=y"),
                     ("PERIPHERAL_ID=1", "PERIPHERAL_ID=2"),
                     (TEST_LEFT.hex(), TEST_RIGHT.hex())],
            "dongle": [("VDB_SIZE=25", "VDB_SIZE=50"), ("DOUBLE_VDB=y", "DOUBLE_VDB=n"),
                       ("ROLE_CENTRAL=y", "ROLE_CENTRAL=n")],
        }
        for role, mutations in changes.items():
            build = self.build(role)
            for before, after in mutations:
                with self.subTest(role=role, mutation=before):
                    (build / "zephyr/.config").write_text(config_for(role).replace(before, after))
                    self.rejected(role, build)

    def test_duplicate_config_and_wrong_role_key_are_rejected(self):
        build = self.build("left")
        for extra in ("CONFIG_ZMK_SPLIT_ESB_PERIPHERAL_ID=1\n", key_line(verifier.RIGHT, TEST_RIGHT)):
            (build / "zephyr/.config").write_text(config_for("left") + extra)
            self.rejected("left", build)

    def test_header_mismatch_missing_and_duplicate(self):
        build = self.build("left")
        header = build / "modules/totem-esb-compat/generated/totem/esb_v3_keys.h"
        header.write_text(header_for("right"))
        self.rejected("left", build)
        header.unlink()
        self.rejected("left", build)
        header.write_text(header_for("left"))
        second = build / "old/generated/totem/esb_v3_keys.h"
        second.parent.mkdir(parents=True)
        second.write_text(header_for("left"))
        self.rejected("left", build)

    def test_uf2_role_key_leaks_and_public_keys_are_rejected(self):
        for role in ("left", "right", "dongle"):
            build = self.build(role)
            own = TEST_LEFT if role != "right" else TEST_RIGHT
            variants = [payload_for(role).replace(own, b"X" * 16),
                        payload_for(role) + verifier.PUBLIC_CI_KEYS[0],
                        payload_for(role) + b"ESB_DIAG USB_START"]
            if role != "dongle":
                variants.append(payload_for(role) + (TEST_RIGHT if role == "left" else TEST_LEFT))
            else:
                variants.append(payload_for(role) + STARTUP_FIXTURE[0])
            for payload in variants:
                (build / "zephyr/zmk.uf2").write_bytes(make_uf2(payload))
                self.rejected(role, build)

    def test_all_uf2_fields_and_block_boundaries_are_checked(self):
        build = self.build("left")
        original = make_uf2(payload_for("left"))
        variants = [b"", original[:-1]]
        for offset, replacement in ((0, 0), (4, 0), (8, 0), (12, 0), (16, 128),
                                    (20, 1), (24, 999), (28, 0), (508, 0), (512 + 12, 0x27000)):
            changed = bytearray(original)
            struct.pack_into("<I", changed, offset, replacement)
            variants.append(bytes(changed))
        for changed in variants:
            (build / "zephyr/zmk.uf2").write_bytes(changed)
            self.rejected("left", build)

    def test_raw_binary_must_match_uf2(self):
        build = self.build("left")
        (build / "zephyr/zmk.bin").write_bytes(payload_for("left") + b"wrong")
        self.rejected("left", build)

    def test_cli_pairs_and_errors_never_echo_inputs(self):
        for args in (["--key-dir", str(self.keys), "--role", "left"],
                     ["--key-dir", str(self.keys), "--role", TEST_LEFT.hex()],
                     ["--key-dir", str(self.root / TEST_LEFT.hex())]):
            output, errors = io.StringIO(), io.StringIO()
            with contextlib.redirect_stdout(output), contextlib.redirect_stderr(errors):
                status = verifier.main(args)
            self.assertEqual(status, 2)
            self.assertNotIn(TEST_LEFT.hex(), output.getvalue() + errors.getvalue())
            self.assertNotIn("Traceback", errors.getvalue())

    def test_cli_success_emits_only_summary_and_decode_errors_are_safe(self):
        build = self.build("left")
        output, errors = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(output), contextlib.redirect_stderr(errors):
            status = verifier.main(["--key-dir", str(self.keys), "--role", "left", "--build-dir", str(build)])
        self.assertEqual(status, 0)
        self.assertTrue(json.loads(output.getvalue())["verified"])
        self.assertEqual(errors.getvalue(), "")
        self.assertNotIn(str(self.keys), output.getvalue())
        for key in TEST_KEYS.values():
            self.assertNotIn(key.hex(), output.getvalue())
        (self.keys / "left.keyconf").write_bytes(b"\xff" + TEST_LEFT.hex().encode())
        self.rejected()


if __name__ == "__main__":
    unittest.main()
