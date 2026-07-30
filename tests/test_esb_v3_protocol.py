#!/usr/bin/env python3
# Copyright (c) 2026 asj9005
# SPDX-License-Identifier: MIT
"""Protocol-state model tests for Totem ESB secure v3.

This is deliberately a standard-library model, not a test of the firmware
implementation or its PSA AES-CCM backend. It checks nonce construction and
the intended handshake/replay invariants independently of the embedded code.
"""

from __future__ import annotations

from dataclasses import dataclass
import unittest


UINT32_MAX = (1 << 32) - 1
UINT64_MAX = (1 << 64) - 1

DOMAIN_UPLINK_LEFT = 0xA1
DOMAIN_UPLINK_RIGHT = 0xA2
DOMAIN_DOWNLINK_LEFT = 0xA5
DOMAIN_DOWNLINK_RIGHT = 0xA6
DOMAIN_ROOT_UPLINK_LEFT = 0xA9
DOMAIN_ROOT_UPLINK_RIGHT = 0xAA
DOMAIN_ROOT_DOWNLINK_LEFT = 0xAD
DOMAIN_ROOT_DOWNLINK_RIGHT = 0xAE
VALID_DOMAINS = {
    DOMAIN_UPLINK_LEFT,
    DOMAIN_UPLINK_RIGHT,
    DOMAIN_DOWNLINK_LEFT,
    DOMAIN_DOWNLINK_RIGHT,
    DOMAIN_ROOT_UPLINK_LEFT,
    DOMAIN_ROOT_UPLINK_RIGHT,
    DOMAIN_ROOT_DOWNLINK_LEFT,
    DOMAIN_ROOT_DOWNLINK_RIGHT,
}


def traffic_nonce(domain: int, session_nonce: int, sequence: int) -> bytes:
    """Build domain || session64-LE || sequence32-LE (13 bytes)."""
    if not 0 <= domain <= 0xFF:
        raise ValueError("domain must fit in one byte")
    if not 0 <= session_nonce <= UINT64_MAX:
        raise ValueError("session nonce must fit in 64 bits")
    if not 0 <= sequence <= UINT32_MAX:
        raise ValueError("sequence must fit in 32 bits")
    return (
        bytes((domain,))
        + session_nonce.to_bytes(8, "little")
        + sequence.to_bytes(4, "little")
    )


@dataclass(frozen=True)
class Session:
    peripheral_boot_nonce: int
    central_challenge: int

    @property
    def nonce(self) -> int:
        return self.peripheral_boot_nonce ^ self.central_challenge


class HandshakeReplayModel:
    """Small fail-closed model of the dongle's per-peer session state.

    The ``authenticated`` arguments stand in for successful firmware tag
    verification. No cryptographic operation is performed in this model.
    """

    def __init__(self) -> None:
        self.active: Session | None = None
        self.pending: Session | None = None
        self._seen_boot_nonces: set[int] = set()
        self._reserved_session_nonces: set[int] = set()
        self._rx_highwater: dict[int, int] = {}
        self.fail_closed = False

    def receive_hello(
        self,
        peripheral_boot_nonce: int,
        central_challenge: int,
        *,
        authenticated: bool = True,
    ) -> bool:
        if not authenticated:
            return False
        if not 0 < peripheral_boot_nonce <= UINT64_MAX:
            return False
        if not 0 < central_challenge <= UINT64_MAX:
            return False
        if peripheral_boot_nonce in self._seen_boot_nonces:
            return False

        candidate = Session(peripheral_boot_nonce, central_challenge)
        if candidate.nonce == 0 or candidate.nonce in self._reserved_session_nonces:
            return False

        self._seen_boot_nonces.add(peripheral_boot_nonce)
        self._reserved_session_nonces.add(candidate.nonce)
        self.pending = candidate
        # An authenticated HELLO may prepare a replacement, but must never
        # replace or clear a currently active session.
        return True

    def receive_ready(self, session_nonce: int, *, authenticated: bool = True) -> bool:
        if (
            not authenticated
            or self.pending is None
            or self.pending.nonce != session_nonce
        ):
            return False

        self.active = self.pending
        self.pending = None
        self._rx_highwater.clear()
        self.fail_closed = False
        return True

    def accept_traffic(self, session_nonce: int, domain: int, sequence: int) -> bool:
        if (
            self.fail_closed
            or self.active is None
            or self.active.nonce != session_nonce
            or domain not in VALID_DOMAINS
            or not 1 <= sequence <= UINT32_MAX
        ):
            return False

        # UINT32_MAX is reserved as the fail-closed re-handshake boundary so
        # the counter can never wrap and reuse a nonce under the same session.
        if sequence == UINT32_MAX:
            self.fail_closed = True
            return False

        previous = self._rx_highwater.get(domain)
        if previous is not None and sequence <= previous:
            return False

        self._rx_highwater[domain] = sequence
        return True


def establish(
    model: HandshakeReplayModel,
    peripheral_boot_nonce: int,
    central_challenge: int,
) -> Session:
    assert model.receive_hello(peripheral_boot_nonce, central_challenge)
    assert model.pending is not None
    session = model.pending
    assert model.receive_ready(session.nonce)
    return session


class NonceTests(unittest.TestCase):
    def test_nonce_layout_is_13_bytes_and_little_endian(self) -> None:
        nonce = traffic_nonce(
            0xA5,
            0x0102030405060708,
            0x0A0B0C0D,
        )
        self.assertEqual(
            nonce,
            bytes.fromhex("a5 08 07 06 05 04 03 02 01 0d 0c 0b 0a"),
        )
        self.assertEqual(len(nonce), 13)

    def test_domain_session_and_sequence_combinations_are_unique(self) -> None:
        domains = sorted(VALID_DOMAINS)
        sessions = (1, 2, 0x0123456789ABCDEF, UINT64_MAX)
        sequences = (0, 1, 2, UINT32_MAX - 1)

        nonces = {
            traffic_nonce(domain, session, sequence)
            for domain in domains
            for session in sessions
            for sequence in sequences
        }
        self.assertEqual(len(nonces), len(domains) * len(sessions) * len(sequences))

    def test_out_of_range_nonce_fields_are_rejected(self) -> None:
        for args in (
            (-1, 1, 1),
            (0x100, 1, 1),
            (1, -1, 1),
            (1, UINT64_MAX + 1, 1),
            (1, 1, -1),
            (1, 1, UINT32_MAX + 1),
        ):
            with self.subTest(args=args):
                with self.assertRaises(ValueError):
                    traffic_nonce(*args)


class HandshakeAndReplayTests(unittest.TestCase):
    def test_ready_is_required_before_pending_session_becomes_active(self) -> None:
        model = HandshakeReplayModel()
        self.assertTrue(model.receive_hello(0x1111, 0xAAAA))
        self.assertIsNone(model.active)
        self.assertIsNotNone(model.pending)
        pending_nonce = model.pending.nonce

        self.assertFalse(
            model.accept_traffic(pending_nonce, DOMAIN_UPLINK_LEFT, 1)
        )
        self.assertFalse(model.receive_ready(pending_nonce, authenticated=False))
        self.assertIsNone(model.active)
        self.assertTrue(model.receive_ready(pending_nonce))
        self.assertEqual(model.active.nonce, pending_nonce)

    def test_replayed_old_hello_cannot_replace_active_session(self) -> None:
        model = HandshakeReplayModel()
        active = establish(model, 0x1111, 0xAAAA)

        self.assertFalse(model.receive_hello(0x1111, 0xBBBB))
        self.assertEqual(model.active, active)
        self.assertIsNone(model.pending)

    def test_fresh_hello_stays_pending_until_matching_ready(self) -> None:
        model = HandshakeReplayModel()
        old = establish(model, 0x1111, 0xAAAA)
        self.assertTrue(
            model.accept_traffic(old.nonce, DOMAIN_UPLINK_LEFT, 10)
        )

        self.assertTrue(model.receive_hello(0x2222, 0xBBBB))
        new = model.pending
        self.assertIsNotNone(new)
        self.assertEqual(model.active, old)
        self.assertTrue(
            model.accept_traffic(old.nonce, DOMAIN_UPLINK_LEFT, 11)
        )
        self.assertFalse(
            model.accept_traffic(new.nonce, DOMAIN_UPLINK_LEFT, 1)
        )

        self.assertFalse(model.receive_ready(old.nonce))
        self.assertEqual(model.active, old)
        self.assertTrue(model.receive_ready(new.nonce))
        self.assertEqual(model.active, new)
        self.assertFalse(
            model.accept_traffic(old.nonce, DOMAIN_UPLINK_LEFT, 12)
        )

    def test_duplicate_backward_and_old_session_traffic_are_rejected(self) -> None:
        model = HandshakeReplayModel()
        old = establish(model, 0x1111, 0xAAAA)

        self.assertTrue(
            model.accept_traffic(old.nonce, DOMAIN_UPLINK_LEFT, 10)
        )
        self.assertFalse(
            model.accept_traffic(old.nonce, DOMAIN_UPLINK_LEFT, 10)
        )
        self.assertFalse(
            model.accept_traffic(old.nonce, DOMAIN_UPLINK_LEFT, 9)
        )
        self.assertTrue(
            model.accept_traffic(old.nonce, DOMAIN_UPLINK_LEFT, 11)
        )

        self.assertTrue(model.receive_hello(0x2222, 0xBBBB))
        new_nonce = model.pending.nonce
        self.assertTrue(model.receive_ready(new_nonce))
        self.assertFalse(
            model.accept_traffic(old.nonce, DOMAIN_UPLINK_LEFT, 12)
        )
        self.assertTrue(
            model.accept_traffic(new_nonce, DOMAIN_UPLINK_LEFT, 1)
        )

    def test_direction_domains_keep_independent_highwater_marks(self) -> None:
        model = HandshakeReplayModel()
        session = establish(model, 0x1111, 0xAAAA)

        self.assertTrue(
            model.accept_traffic(session.nonce, DOMAIN_UPLINK_LEFT, 5)
        )
        self.assertTrue(
            model.accept_traffic(session.nonce, DOMAIN_DOWNLINK_LEFT, 5)
        )
        self.assertFalse(
            model.accept_traffic(session.nonce, DOMAIN_UPLINK_LEFT, 5)
        )
        self.assertFalse(
            model.accept_traffic(session.nonce, DOMAIN_DOWNLINK_LEFT, 5)
        )

    def test_sequence_wrap_fails_closed_until_new_ready(self) -> None:
        model = HandshakeReplayModel()
        old = establish(model, 0x1111, 0xAAAA)

        self.assertTrue(
            model.accept_traffic(
                old.nonce,
                DOMAIN_UPLINK_LEFT,
                UINT32_MAX - 1,
            )
        )
        self.assertFalse(
            model.accept_traffic(old.nonce, DOMAIN_UPLINK_LEFT, UINT32_MAX)
        )
        self.assertTrue(model.fail_closed)
        self.assertFalse(
            model.accept_traffic(old.nonce, DOMAIN_UPLINK_LEFT, 1)
        )
        self.assertFalse(
            model.accept_traffic(old.nonce, DOMAIN_DOWNLINK_LEFT, 1)
        )

        self.assertTrue(model.receive_hello(0x2222, 0xBBBB))
        replacement_nonce = model.pending.nonce
        self.assertTrue(model.receive_ready(replacement_nonce))
        self.assertFalse(model.fail_closed)
        self.assertTrue(
            model.accept_traffic(
                replacement_nonce,
                DOMAIN_UPLINK_LEFT,
                1,
            )
        )


if __name__ == "__main__":
    unittest.main()
