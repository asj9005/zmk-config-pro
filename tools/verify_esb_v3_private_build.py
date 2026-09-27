#!/usr/bin/env python3
# Copyright (c) 2026 asj9005
# SPDX-License-Identifier: MIT
"""Read-only verification of a private ESB v3 key set and an optional local build.

Run with --key-dir first. Add --role left|right|dongle --build-dir after a
pristine build of the tested lowprioprobe/ram25 configuration. Never redirect
private build logs to a public location. This tool emits only fixed labels,
booleans, fingerprints and file hashes; it never emits keys or file contents.

UF2 framing checks are shared in design with the existing CI artifact verifier.
They verify the XIAO nRF52840 application region, not device installation or
runtime RF behavior. The supplied source checkout/toolchain must be verified
separately. No files are created or changed by this tool.
"""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import re
import struct
import sys

LOCAL = "CONFIG_TOTEM_ESB_V3_LOCAL_KEY_HEX"
LEFT = "CONFIG_TOTEM_ESB_V3_LEFT_KEY_HEX"
RIGHT = "CONFIG_TOTEM_ESB_V3_RIGHT_KEY_HEX"
KEY_SYMBOLS = {LOCAL, LEFT, RIGHT}
PUBLIC_CI_KEYS = (
    bytes.fromhex("4d4f5d2cd993c6be0db2495606d229a1"),
    bytes.fromhex("ed812b6ff48a2e17328b623983cb776c"),
)
FLASH_START = 0x27000
FLASH_LIMIT = 0xEC000
FAMILY_ID = 0xADA52840
STARTUP_MARKERS = (
    b"ESB_DIAG DELAY_START delay_ms=",
    b"ESB_DIAG AUTO_START transport_init",
    b"ESB_DIAG AUTO_START transport_return",
)


class VerificationError(Exception):
    """Messages contain only verifier-authored text, never input data."""


def require(condition: bool, message: str) -> None:
    if not condition:
        raise VerificationError(message)


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def read_bytes(path: Path, maximum: int, label: str) -> bytes:
    # label is always a fixed string supplied by this module, not a file name.
    try:
        require(path.is_file(), label + " is missing or is not a regular file")
        require(path.stat().st_size <= maximum, label + " exceeds the size limit")
        data = path.read_bytes()
        require(len(data) <= maximum, label + " exceeds the size limit")
        return data
    except OSError:
        raise VerificationError(label + " could not be read") from None


def decode(data: bytes, label: str) -> str:
    try:
        return data.decode("utf-8-sig")
    except UnicodeError:
        raise VerificationError(label + " has invalid text encoding") from None


def parse_config(text: str, label: str) -> dict[str, str]:
    values: dict[str, str] = {}
    for raw in text.splitlines():
        line = raw.strip()
        if not line:
            continue
        disabled = re.fullmatch(r"# (CONFIG_[A-Z0-9_]+) is not set", line)
        if disabled:
            symbol, value = disabled[1], "n"
        elif line.startswith("#"):
            continue
        else:
            match = re.fullmatch(r"(CONFIG_[A-Z0-9_]+)=(.*)", line)
            require(match is not None, label + " contains an invalid assignment")
            symbol, value = match[1], match[2]
        require(symbol not in values, label + " contains a duplicate symbol")
        values[symbol] = value
    return values


def parse_key(value: str) -> bytes:
    require(re.fullmatch(r'"[0-9a-fA-F]{32}"', value) is not None,
            "A key must be one quoted 32-digit hexadecimal value")
    return bytes.fromhex(value[1:-1])


def read_key_set(key_dir: Path) -> dict[str, bytes]:
    expected = {"left": {LOCAL}, "right": {LOCAL}, "dongle": {LEFT, RIGHT}}
    parsed = {}
    for role, symbols in expected.items():
        label = role + " key configuration"
        data = read_bytes(key_dir / (role + ".keyconf"), 4096, label)
        values = parse_config(decode(data, label), label)
        require(set(values) == symbols, label + " has unexpected or missing symbols")
        parsed[role] = {symbol: parse_key(value) for symbol, value in values.items()}
    left, right = parsed["left"][LOCAL], parsed["right"][LOCAL]
    require(left == parsed["dongle"][LEFT] and right == parsed["dongle"][RIGHT],
            "Peripheral and dongle key configurations do not match")
    require(left != right, "Left and right link keys must differ")
    require(all(key not in PUBLIC_CI_KEYS for key in (left, right)),
            "A public CI link key is present")
    require(all(key != bytes(16) for key in (left, right)), "An all-zero link key is present")
    return {"left": left, "right": right}


def check_build_config(values: dict[str, str], role: str, keys: dict[str, bytes]) -> None:
    require(role in ("left", "right", "dongle"), "Unsupported firmware role")
    expected = {
        "CONFIG_BOARD_TARGET": '"xiao_ble/nrf52840/zmk"',
        "CONFIG_SOC": '"nrf52840"',
        "CONFIG_SHIELD_TOTEM_ESB_V3": "y",
        "CONFIG_TOTEM_ESB_COMPAT": "y",
        "CONFIG_TOTEM_ESB_V3": "y",
        "CONFIG_TOTEM_ESB_DIAGNOSTICS": "y",
        "CONFIG_TOTEM_ESB_V3_CI_TEST_KEYS": "n",
        "CONFIG_TOTEM_ESB_BENCHMARK": "n",
        "CONFIG_TOTEM_ESB_DIAGNOSTIC_USB_START": "n",
        "CONFIG_ZMK_SPLIT": "y",
        "CONFIG_ZMK_SPLIT_ESB": "y",
        "CONFIG_ZMK_BLE": "n",
        "CONFIG_ESB_MAX_PAYLOAD_LENGTH": "64",
        "CONFIG_SYSTEM_WORKQUEUE_STACK_SIZE": "4096",
        "CONFIG_INPUT_THREAD_STACK_SIZE": "4096",
        "CONFIG_ZMK_SPLIT_ROLE_CENTRAL": "y" if role == "dongle" else "n",
    }
    for candidate in ("left", "right", "dongle"):
        enabled = "y" if candidate == role else "n"
        expected["CONFIG_SHIELD_TOTEM_" + candidate.upper()] = enabled
        expected["CONFIG_SHIELD_TOTEM_ESB_" + candidate.upper()] = enabled
    if role == "dongle":
        expected.update({
            "CONFIG_ZMK_SPLIT_ESB_PERIPHERAL_COUNT": "2",
            "CONFIG_ZMK_SPLIT_ESB_PERIPHERAL_ID": "0",
            "CONFIG_TOTEM_ESB_PROSPECTOR": "y",
            "CONFIG_ZMK_LOW_PRIORITY_THREAD_STACK_SIZE": "768",
            "CONFIG_LV_Z_VDB_SIZE": "25",
            "CONFIG_LV_Z_DOUBLE_VDB": "y",
            "CONFIG_ZMK_USB": "y",
            "CONFIG_ZMK_STUDIO": "y",
            "CONFIG_USB_HID_POLL_INTERVAL_MS": "1",
        })
        required_keys = {LEFT: keys["left"], RIGHT: keys["right"]}
    else:
        expected.update({
            "CONFIG_ZMK_SPLIT_ESB_PERIPHERAL_COUNT": "0",
            "CONFIG_ZMK_SPLIT_ESB_PERIPHERAL_ID": "1" if role == "left" else "2",
            "CONFIG_ZMK_LOW_PRIORITY_THREAD_STACK_SIZE": "4096",
            "CONFIG_TOTEM_ESB_DIAGNOSTIC_START_DELAY_MS": "3000",
            "CONFIG_ZMK_USB": "n",
        })
        required_keys = {LOCAL: keys[role]}
    for symbol, wanted in expected.items():
        # Kconfig can omit an inactive boolean entirely, or emit '# ... not set'.
        actual = values.get(symbol, "n" if wanted == "n" else None)
        require(actual == wanted, "Build configuration does not match the tested role: " + symbol)
    present_keys = {symbol for symbol in KEY_SYMBOLS if symbol in values}
    require(present_keys == set(required_keys), "Build configuration has keys for the wrong role")
    for symbol, key in required_keys.items():
        require(parse_key(values[symbol]) == key, "Build configuration key differs from the supplied set")


def check_generated_header(data: bytes, role: str, keys: dict[str, bytes]) -> None:
    require(role in ("left", "right", "dongle"), "Unsupported firmware role")
    text = re.sub(r"/\*[\s\S]*?\*/", "", decode(data, "Generated key header"))
    values = {}
    for raw in text.splitlines():
        line = raw.strip()
        if not line or line == "#pragma once":
            continue
        match = re.fullmatch(r"#define TOTEM_ESB_V3_(LOCAL|LEFT|RIGHT)_KEY_BYTES\s+(.+)", line)
        require(match is not None, "Generated key header contains an unexpected definition")
        require(match[1] not in values, "Generated key header contains a duplicate definition")
        tokens = [part.strip() for part in match[2].split(",")]
        require(len(tokens) == 16 and all(re.fullmatch(r"0x[0-9a-fA-F]{2}", part) for part in tokens),
                "Generated key header contains an invalid initializer")
        values[match[1]] = bytes(int(part, 16) for part in tokens)
    expected = {"LOCAL": bytes(16), "LEFT": bytes(16), "RIGHT": bytes(16)}
    if role == "dongle":
        expected.update(LEFT=keys["left"], RIGHT=keys["right"])
    else:
        expected["LOCAL"] = keys[role]
    require(values == expected, "Generated key header does not match the requested role and key set")


def validate_uf2(data: bytes, role: str, keys: dict[str, bytes]) -> bytes:
    require(role in ("left", "right", "dongle"), "Unsupported firmware role")
    require(bool(data) and len(data) % 512 == 0, "Invalid UF2 file length")
    total = len(data) // 512
    require(total * 256 <= FLASH_LIMIT - FLASH_START, "UF2 exceeds application capacity")
    payload = bytearray()
    for index in range(total):
        block = data[index * 512:(index + 1) * 512]
        magic0, magic1, flags, address, size, number, count, family = struct.unpack_from("<8I", block)
        require((magic0, magic1) == (0x0A324655, 0x9E5D5157), "Invalid UF2 header magic")
        require(struct.unpack_from("<I", block, 508)[0] == 0x0AB16F30, "Invalid UF2 end magic")
        require((flags, size, number, count, family) == (0x2000, 256, index, total, FAMILY_ID),
                "Invalid UF2 flags, size, block sequence/count or family")
        require(address == FLASH_START + index * 256 and address + size <= FLASH_LIMIT,
                "Invalid UF2 application flash address")
        payload.extend(block[32:32 + size])
    require(not any(key in payload for key in PUBLIC_CI_KEYS), "UF2 contains a public CI link key")
    if role == "dongle":
        require(all(key in payload for key in keys.values()), "Dongle UF2 lacks an expected link key")
        require(b"dongle\x00" in payload, "Dongle diagnostic role marker is absent")
        require(not any(marker in payload for marker in STARTUP_MARKERS),
                "Dongle UF2 contains peripheral startup markers")
    else:
        other = "right" if role == "left" else "left"
        require(keys[role] in payload, "Peripheral UF2 lacks its expected link key")
        require(keys[other] not in payload, "Peripheral UF2 contains the other half's link key")
        require(all(marker in payload for marker in STARTUP_MARKERS),
                "Peripheral UF2 lacks the tested automatic delayed startup markers")
    require(b"ESB_DIAG USB_START" not in payload, "UF2 contains DTR-gated startup code")
    require(b"[esb-diag] role=%s" in payload and b"tx_steps=done:result" in payload,
            "UF2 lacks the tested diagnostic markers")
    return bytes(payload)


def verify(key_dir: Path, role: str | None = None, build_dir: Path | None = None) -> dict:
    require((role is None) == (build_dir is None), "Role and build directory must be supplied together")
    keys = read_key_set(key_dir)
    result = {
        "verified": True,
        "key_set_matches": True,
        "public_ci_keys_absent": True,
        "left_sha256_64": sha256(keys["left"])[:16],
        "right_sha256_64": sha256(keys["right"])[:16],
    }
    if build_dir is None:
        return result
    config_data = read_bytes(build_dir / "zephyr" / ".config", 2 * 1024 * 1024, "Build configuration")
    check_build_config(parse_config(decode(config_data, "Build configuration"), "Build configuration"), role, keys)
    try:
        headers = [path for path in build_dir.rglob("esb_v3_keys.h")
                   if path.parts[-3:] == ("generated", "totem", "esb_v3_keys.h")]
    except OSError:
        raise VerificationError("Generated key header could not be located") from None
    require(len(headers) == 1, "Expected exactly one generated key header in the build directory")
    header_data = read_bytes(headers[0], 65536, "Generated key header")
    check_generated_header(header_data, role, keys)
    uf2_data = read_bytes(build_dir / "zephyr" / "zmk.uf2", 2 * (FLASH_LIMIT - FLASH_START), "UF2")
    payload = validate_uf2(uf2_data, role, keys)
    result.update(role=role, build_config_matches=True, generated_header_matches=True,
                  uf2_role_keys_match=True, uf2_blocks=len(uf2_data) // 512,
                  config_sha256=sha256(config_data), header_sha256=sha256(header_data),
                  uf2_sha256=sha256(uf2_data), binary_checked=False)
    binary_path = build_dir / "zephyr" / "zmk.bin"
    if binary_path.exists():
        binary = read_bytes(binary_path, FLASH_LIMIT - FLASH_START, "Raw firmware binary")
        padding = payload[len(binary):]
        require(bool(binary) and 0 <= len(payload) - len(binary) < 256 and
                payload[:len(binary)] == binary and
                (not padding or set(padding) in ({0}, {255})),
                "UF2 payload differs from the raw firmware binary")
        result.update(binary_checked=True, binary_sha256=sha256(binary))
    return result


class SafeParser(argparse.ArgumentParser):
    def error(self, message: str) -> None:
        # argparse's default errors echo untrusted command-line values.
        raise VerificationError("Invalid command line; provide --key-dir and optional --role/--build-dir")


def main(argv: list[str] | None = None) -> int:
    try:
        parser = SafeParser(description=__doc__)
        parser.add_argument("--key-dir", required=True, type=Path)
        parser.add_argument("--role", choices=("left", "right", "dongle"))
        parser.add_argument("--build-dir", type=Path)
        args = parser.parse_args(argv)
        result = verify(args.key_dir, args.role, args.build_dir)
    except VerificationError as error:
        print(json.dumps({"verified": False, "error": str(error)}), file=sys.stderr)
        return 2
    except Exception:
        # Do not expose paths, decoded config lines or exception reprs on an
        # unexpected filesystem/decoder error involving private build inputs.
        print(json.dumps({"verified": False, "error": "Verification could not be completed"}), file=sys.stderr)
        return 2
    print(json.dumps(result, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
