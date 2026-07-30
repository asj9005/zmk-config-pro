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
        self._failed_session_nonce: int | None = None
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

    def encrypted_link_authentication_failure(self) -> None:
        """Model BLE-style connection loss after an encrypted MIC failure."""
        self._failed_session_nonce = (
            None if self.active is None else self.active.nonce
        )
        self.active = None
        self.pending = None
        self._rx_highwater.clear()
        self.fail_closed = True

    def prepare_auth_failure_rekey(
        self,
        failed_session_nonce: int,
        peripheral_boot_nonce: int,
        central_challenge: int,
        *,
        authenticated: bool = True,
    ) -> bool:
        """Accept a root-authenticated rekey tied to the failed session."""
        if (
            not authenticated
            or not self.fail_closed
            or self._failed_session_nonce != failed_session_nonce
            or not 0 < peripheral_boot_nonce <= UINT64_MAX
            or not 0 < central_challenge <= UINT64_MAX
        ):
            return False

        candidate = Session(peripheral_boot_nonce, central_challenge)
        if (
            candidate.nonce == 0
            or candidate.nonce in self._reserved_session_nonces
        ):
            return False

        self._reserved_session_nonces.add(candidate.nonce)
        self.pending = candidate
        self._failed_session_nonce = None
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


class RecoveryHighwaterModel:
    """Model root RECOVERY replay state independently of traffic sessions."""

    def __init__(
        self,
        *,
        active_boot_nonce: int | None = None,
        active_session: int | None = None,
        highwater: int = 0,
    ) -> None:
        self.root_boot_nonce = active_boot_nonce
        self.root_highwater = highwater
        self.active_boot_nonce = active_boot_nonce
        self.active_session = active_session
        self.pending_boot_nonce: int | None = None
        self.pending_session: int | None = None
        self.pending_request = 0
        self.pending_reset_session = 0
        self.challenge_generation = 0

    def receive_hello(self, boot_nonce: int, pending_session: int) -> str:
        if (
            boot_nonce == 0
            or pending_session == 0
            or (
                self.active_session is not None
                and self.active_boot_nonce == boot_nonce
            )
        ):
            return "ignored"
        if self.pending_reset_session != 0:
            return (
                "challenge_retry"
                if self.pending_boot_nonce == boot_nonce
                else "reset_pinned"
            )
        self.pending_boot_nonce = boot_nonce
        self.pending_session = pending_session
        self.pending_request = 0
        self.pending_reset_session = 0
        self.challenge_generation += 1
        return "challenge"

    def begin_mic_reset(
        self,
        *,
        boot_nonce: int,
        failed_session: int,
        pending_session: int,
        request_sequence: int,
    ) -> None:
        self.active_boot_nonce = None
        self.active_session = None
        self.pending_boot_nonce = boot_nonce
        self.pending_session = pending_session
        self.pending_request = request_sequence
        self.pending_reset_session = failed_session
        self.challenge_generation += 1

    def expire_pending(self) -> bool:
        if self.pending_session is None:
            return False
        reset_pending = self.pending_reset_session != 0
        self.pending_boot_nonce = None
        self.pending_session = None
        self.pending_request = 0
        self.pending_reset_session = 0
        if self.active_session is None and not reset_pending:
            self.root_boot_nonce = None
            self.root_highwater = 0
        return True

    def timeout(self) -> None:
        """Discard traffic state while preserving the root replay epoch."""
        self.active_boot_nonce = None
        self.active_session = None
        self.pending_boot_nonce = None
        self.pending_session = None
        self.pending_request = 0
        self.pending_reset_session = 0

    def receive_recovery(
        self,
        boot_nonce: int,
        traffic_session: int,
        sequence: int,
        *,
        authenticated: bool = True,
    ) -> str:
        if (
            not authenticated
            or boot_nonce == 0
            or sequence == 0
        ):
            return "invalid"
        if self.root_boot_nonce is None:
            if self.active_session is not None or self.pending_session is not None:
                return "stale"
            self.root_boot_nonce = boot_nonce
            self.root_highwater = 0
        if boot_nonce != self.root_boot_nonce:
            return "stale"
        if sequence <= self.root_highwater:
            return "replay"

        # Authenticated root nonces are consumed even when their traffic
        # session has already been retired.
        self.root_highwater = sequence
        if self.active_session is not None:
            if (
                self.active_boot_nonce == boot_nonce
                and self.active_session == traffic_session
            ):
                return "active"
            return "stale_session"

        if self.pending_reset_session != 0:
            if (
                self.pending_boot_nonce == boot_nonce
                and self.pending_reset_session == traffic_session
            ):
                return "reset_retry"
            if self.pending_boot_nonce == boot_nonce:
                return "stale_session"

        if (
            self.pending_session is not None
            and self.pending_reset_session == 0
        ):
            return (
                "challenge_retry"
                if self.pending_boot_nonce == boot_nonce
                else "stale"
            )

        # Once queued, a normal challenge transcript remains fixed until
        # READY. Re-sealing the same authenticated content is safe; changing
        # the pending key while the old ACK payload is in flight is not.
        self.pending_boot_nonce = boot_nonce
        self.pending_session = (self.pending_session or 0x1000) + 1
        self.pending_request = sequence
        self.pending_reset_session = 0
        self.challenge_generation += 1
        return "challenge"

    def promote_pending(self) -> None:
        assert self.pending_boot_nonce is not None
        assert self.pending_session is not None
        if self.root_boot_nonce != self.pending_boot_nonce:
            self.root_boot_nonce = self.pending_boot_nonce
            self.root_highwater = self.pending_request
        self.active_boot_nonce = self.pending_boot_nonce
        self.active_session = self.pending_session
        self.pending_boot_nonce = None
        self.pending_session = None
        self.pending_request = 0
        self.pending_reset_session = 0


class PeripheralChallengeModel:
    """Model the half's normal (non-reset) RECOVERY challenge window."""

    def __init__(
        self,
        *,
        accepted_request: int = 0,
        central_nonce: int = 0xAAAA,
    ) -> None:
        self.state = "established"
        self.accepted_request = accepted_request
        self.last_probe = accepted_request
        self.central_nonce = central_nonce

    def expire_session_ok_wait(self) -> bool:
        if self.state != "wait_session_ok":
            return False
        self.state = "wait_challenge"
        self.accepted_request = UINT32_MAX
        self.last_probe = 0
        self.central_nonce = 0
        return True

    def send_recovery(self) -> int:
        self.last_probe += 1
        return self.last_probe

    def receive_challenge(self, request: int, central_nonce: int) -> str:
        if self.state == "established":
            if request == self.accepted_request:
                return (
                    "duplicate"
                    if central_nonce == self.central_nonce
                    else "rejected"
                )
            if (
                request < self.accepted_request
                or request > self.last_probe
            ):
                return "rejected"
        elif self.state == "wait_session_ok":
            if request == self.accepted_request:
                return (
                    "ready"
                    if central_nonce == self.central_nonce
                    else "rejected"
                )
            if (
                request < self.accepted_request
                or request > self.last_probe
            ):
                return "rejected"
        else:
            return "rejected"

        self.state = "wait_session_ok"
        self.accepted_request = request
        self.central_nonce = central_nonce
        return "ready"


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

    def test_bad_root_hello_does_not_terminate_active_link(self) -> None:
        model = HandshakeReplayModel()
        active = establish(model, 0x1111, 0xAAAA)

        self.assertFalse(
            model.receive_hello(0x2222, 0xBBBB, authenticated=False)
        )
        self.assertEqual(model.active, active)
        self.assertIsNone(model.pending)
        self.assertTrue(
            model.accept_traffic(active.nonce, DOMAIN_UPLINK_LEFT, 1)
        )

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

    def test_mic_failure_terminates_link_until_fresh_handshake(self) -> None:
        model = HandshakeReplayModel()
        old = establish(model, 0x1111, 0xAAAA)
        self.assertTrue(
            model.accept_traffic(old.nonce, DOMAIN_UPLINK_LEFT, 1)
        )

        # Even an already prepared replacement must not survive a MIC failure.
        self.assertTrue(model.receive_hello(0x2222, 0xBBBB))
        pending = model.pending
        self.assertIsNotNone(pending)
        model.encrypted_link_authentication_failure()

        self.assertTrue(model.fail_closed)
        self.assertIsNone(model.active)
        self.assertIsNone(model.pending)
        self.assertFalse(
            model.accept_traffic(old.nonce, DOMAIN_UPLINK_LEFT, 2)
        )
        self.assertFalse(model.receive_ready(pending.nonce))

        self.assertFalse(
            model.prepare_auth_failure_rekey(
                old.nonce ^ 1,
                old.peripheral_boot_nonce,
                0xCCCC,
            )
        )
        self.assertTrue(
            model.prepare_auth_failure_rekey(
                old.nonce,
                old.peripheral_boot_nonce,
                0xCCCC,
            )
        )
        replacement = model.pending
        self.assertIsNotNone(replacement)
        self.assertTrue(model.receive_ready(replacement.nonce))
        self.assertFalse(model.fail_closed)
        self.assertTrue(
            model.accept_traffic(
                replacement.nonce,
                DOMAIN_UPLINK_LEFT,
                1,
            )
        )
        self.assertFalse(
            model.prepare_auth_failure_rekey(
                old.nonce,
                old.peripheral_boot_nonce,
                0xCCCC,
            )
        )


class RecoveryReplayTests(unittest.TestCase):
    def test_mic_reset_preserves_root_highwater_across_ready(self) -> None:
        model = RecoveryHighwaterModel(
            active_boot_nonce=0x1111,
            active_session=0xAAAA,
            highwater=10,
        )
        model.begin_mic_reset(
            boot_nonce=0x1111,
            failed_session=0xAAAA,
            pending_session=0xBBBB,
            request_sequence=10,
        )
        transcript = (
            model.pending_session,
            model.pending_request,
            model.pending_reset_session,
            model.challenge_generation,
        )

        self.assertEqual(
            model.receive_recovery(0x1111, 0xAAAA, 11),
            "reset_retry",
        )
        self.assertEqual(
            (
                model.pending_session,
                model.pending_request,
                model.pending_reset_session,
                model.challenge_generation,
            ),
            transcript,
        )
        self.assertEqual(model.root_highwater, 11)

        model.promote_pending()
        self.assertEqual(model.root_highwater, 11)
        self.assertEqual(
            model.receive_recovery(0x1111, 0xAAAA, 10),
            "replay",
        )
        self.assertEqual(
            model.receive_recovery(0x1111, 0xAAAA, 11),
            "replay",
        )
        self.assertIsNone(model.pending_session)

    def test_fresh_delayed_old_session_is_consumed_without_rekey(self) -> None:
        model = RecoveryHighwaterModel(
            active_boot_nonce=0x1111,
            active_session=0xBBBB,
            highwater=11,
        )
        self.assertEqual(
            model.receive_recovery(0x1111, 0xAAAA, 12),
            "stale_session",
        )
        self.assertEqual(model.root_highwater, 12)
        self.assertEqual(model.active_session, 0xBBBB)
        self.assertIsNone(model.pending_session)
        self.assertEqual(
            model.receive_recovery(0x1111, 0xBBBB, 13),
            "active",
        )

    def test_timeout_keeps_root_epoch_and_requires_fresh_sequence(self) -> None:
        model = RecoveryHighwaterModel(
            active_boot_nonce=0x1111,
            active_session=0xAAAA,
            highwater=20,
        )
        model.timeout()
        self.assertEqual(
            model.receive_recovery(0x1111, 0xAAAA, 20),
            "replay",
        )
        self.assertEqual(
            model.receive_recovery(0x1111, 0xAAAA, 21),
            "challenge",
        )
        self.assertEqual(model.pending_request, 21)

    def test_new_hello_switches_root_epoch_only_when_ready_promotes(self) -> None:
        model = RecoveryHighwaterModel(
            active_boot_nonce=0x1111,
            active_session=0xAAAA,
            highwater=7,
        )
        self.assertEqual(model.receive_hello(0x2222, 0xBBBB), "challenge")
        self.assertEqual(
            model.receive_recovery(0x1111, 0xAAAA, 8),
            "active",
        )
        self.assertEqual(model.root_boot_nonce, 0x1111)

        model.promote_pending()
        self.assertEqual(model.root_boot_nonce, 0x2222)
        self.assertEqual(model.root_highwater, 0)
        self.assertEqual(
            model.receive_recovery(0x1111, 0xAAAA, 9),
            "stale",
        )
        self.assertEqual(model.root_boot_nonce, 0x2222)
        self.assertEqual(
            model.receive_recovery(0x2222, 0xBBBB, 1),
            "active",
        )

    def test_regular_pending_is_pinned_across_new_recovery_sequence(self) -> None:
        model = RecoveryHighwaterModel()
        self.assertEqual(
            model.receive_recovery(0x1111, 0xAAAA, 1),
            "challenge",
        )
        first_generation = model.challenge_generation
        first_pending = model.pending_session
        self.assertEqual(
            model.receive_recovery(0x1111, 0xAAAA, 2),
            "challenge_retry",
        )
        self.assertEqual(model.challenge_generation, first_generation)
        self.assertEqual(model.pending_session, first_pending)
        self.assertEqual(model.pending_request, 1)
        self.assertEqual(model.root_highwater, 2)

    def test_reset_pending_rejects_other_hello_until_bounded_expiry(self) -> None:
        model = RecoveryHighwaterModel(
            active_boot_nonce=0x1111,
            active_session=0xAAAA,
            highwater=4,
        )
        model.begin_mic_reset(
            boot_nonce=0x1111,
            failed_session=0xAAAA,
            pending_session=0xBBBB,
            request_sequence=4,
        )
        transcript = (
            model.pending_boot_nonce,
            model.pending_session,
            model.pending_reset_session,
        )
        self.assertEqual(
            model.receive_hello(0x2222, 0xCCCC),
            "reset_pinned",
        )
        self.assertEqual(
            (
                model.pending_boot_nonce,
                model.pending_session,
                model.pending_reset_session,
            ),
            transcript,
        )
        self.assertTrue(model.expire_pending())
        self.assertEqual(model.root_boot_nonce, 0x1111)
        self.assertEqual(
            model.receive_hello(0x2222, 0xCCCC),
            "challenge",
        )

    def test_cold_boot_replay_candidate_expires_for_live_recovery(self) -> None:
        model = RecoveryHighwaterModel()
        self.assertEqual(
            model.receive_recovery(0x1111, 0xAAAA, 1),
            "challenge",
        )
        self.assertEqual(
            model.receive_recovery(0x2222, 0xBBBB, 1),
            "stale",
        )
        self.assertTrue(model.expire_pending())
        self.assertIsNone(model.root_boot_nonce)
        self.assertEqual(
            model.receive_recovery(0x2222, 0xBBBB, 2),
            "challenge",
        )


class PeripheralChallengeTests(unittest.TestCase):
    def test_one_packet_late_ack_challenge_is_accepted(self) -> None:
        model = PeripheralChallengeModel()
        self.assertEqual(model.send_recovery(), 1)
        self.assertEqual(model.send_recovery(), 2)
        self.assertEqual(
            model.receive_challenge(1, 0xBBBB),
            "ready",
        )
        self.assertEqual(model.accepted_request, 1)
        self.assertEqual(
            model.receive_challenge(1, 0xBBBB),
            "ready",
        )

    def test_newer_valid_challenge_can_replace_orphaned_pending(self) -> None:
        model = PeripheralChallengeModel()
        model.send_recovery()
        model.send_recovery()
        self.assertEqual(
            model.receive_challenge(1, 0xBBBB),
            "ready",
        )
        self.assertEqual(
            model.receive_challenge(2, 0xCCCC),
            "ready",
        )
        self.assertEqual(model.accepted_request, 2)
        self.assertEqual(model.central_nonce, 0xCCCC)
        self.assertEqual(
            model.receive_challenge(1, 0xBBBB),
            "rejected",
        )
        self.assertEqual(
            model.receive_challenge(3, 0xDDDD),
            "rejected",
        )

    def test_session_ok_timeout_returns_to_fresh_hello_state(self) -> None:
        model = PeripheralChallengeModel()
        model.send_recovery()
        self.assertEqual(
            model.receive_challenge(1, 0xBBBB),
            "ready",
        )
        self.assertTrue(model.expire_session_ok_wait())
        self.assertEqual(model.state, "wait_challenge")
        self.assertEqual(model.accepted_request, UINT32_MAX)
        self.assertEqual(model.last_probe, 0)
        self.assertFalse(model.expire_session_ok_wait())


if __name__ == "__main__":
    unittest.main()
