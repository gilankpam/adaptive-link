#!/usr/bin/env python3
"""Unit tests for handshake protocol in alink_gs.

Tests the two-stage HandshakeClient: long resync + fast retry on reply loss,
with t1 echo freshness gating to drop stale or duplicate replies.
"""
import sys
import os

# Add parent directory to path so we can import from alink_gs
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

# Import module-level constants and classes by executing the script up to __main__
_gs_path = os.path.join(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))), 'ground-station', 'alink_gs')
with open(_gs_path) as _f:
    _code = _f.read().split("if __name__")[0]
# Prepend verbose_mode definition to the code so it's available when classes are defined
_code = "verbose_mode = False\n" + _code
exec(_code, globals())


class FakeSocket:
    def __init__(self):
        self.sent = []

    def sendto(self, data, addr):
        self.sent.append((data, addr))


def _make_hs(long_resync_ms=30000, fast_retry_ms=500, max_fast_retries=3,
             default_fps=90, rtt_ema_alpha=0.2):
    """Helper: build a HandshakeClient with a FakeSocket."""
    return HandshakeClient(
        FakeSocket(), '10.5.0.10', 9999,
        default_fps=default_fps,
        long_resync_ms=long_resync_ms,
        fast_retry_ms=fast_retry_ms,
        max_fast_retries=max_fast_retries,
        rtt_ema_alpha=rtt_ema_alpha,
    )


def _last_t1_sent(hs):
    """Extract the t1 from the most recently sent H: frame."""
    msg = hs.sock.sent[-1][0]
    payload = msg[4:].decode("ascii")
    assert payload.startswith("H:")
    return int(payload[2:])


# ─── Initialization ─────────────────────────────────────────────────────────


def test_handshake_basic_initialization():
    """Test handshake client initializes with default FPS."""
    hs = _make_hs()
    assert hs.drone_fps is None
    assert hs.drone_xres is None
    assert hs.drone_yres is None
    assert hs.get_fps() == 90  # Returns default
    assert hs.unmatched_replies == 0


def test_get_fps_returns_default_when_not_synced():
    """Test that get_fps() returns default when no reply received."""
    hs = _make_hs(default_fps=120)
    assert hs.get_fps() == 120
    assert hs.drone_fps is None


# ─── Long resync cadence (happy path) ───────────────────────────────────────


def test_first_tick_sends_immediately():
    """A freshly-constructed client should send on the first tick."""
    hs = _make_hs(long_resync_ms=30000)
    hs.tick(0)
    assert len(hs.sock.sent) == 1


def test_long_resync_cadence_after_successful_reply():
    """After a matching reply, the next hello waits the full long interval."""
    hs = _make_hs(long_resync_ms=30000)

    # t=0: send first hello
    hs.tick(0)
    assert len(hs.sock.sent) == 1
    t1 = _last_t1_sent(hs)

    # Reply arrives — clears pending and schedules next at sent_at + 30000
    assert hs.handle_reply(f"I:{t1}:5:10:60:1920:1080") is True

    # Ticks before the long interval should not send
    hs.tick(15000)
    hs.tick(29999)
    assert len(hs.sock.sent) == 1

    # At the long interval boundary, the next hello fires
    hs.tick(30000)
    assert len(hs.sock.sent) == 2


# ─── Fast retry on reply loss ───────────────────────────────────────────────


def test_fast_retry_fires_after_fast_window():
    """No reply within fast_retry_ms triggers a retry."""
    hs = _make_hs(long_resync_ms=30000, fast_retry_ms=500, max_fast_retries=3)

    hs.tick(0)
    assert len(hs.sock.sent) == 1

    # Inside the fast window — no retry
    hs.tick(100)
    hs.tick(499)
    assert len(hs.sock.sent) == 1

    # Fast window elapsed — retry fires
    hs.tick(500)
    assert len(hs.sock.sent) == 2

    # Another fast window elapsed — second retry
    hs.tick(1000)
    assert len(hs.sock.sent) == 3

    # Third retry
    hs.tick(1500)
    assert len(hs.sock.sent) == 4  # Original + 3 retries


def test_fast_retry_burst_exhaustion_backs_off_to_long_interval():
    """After max_fast_retries, give up and wait for the long resync."""
    hs = _make_hs(long_resync_ms=30000, fast_retry_ms=500, max_fast_retries=3)

    hs.tick(0)        # Send #1
    hs.tick(500)      # Retry #1
    hs.tick(1000)     # Retry #2
    hs.tick(1500)     # Retry #3
    assert len(hs.sock.sent) == 4

    # Burst exhausted on next fast-window expiry — no more sends
    hs.tick(2000)
    assert len(hs.sock.sent) == 4
    assert hs._pending_t1 is None  # State cleared

    # Within long interval — still no send
    hs.tick(15000)
    assert len(hs.sock.sent) == 4

    # After long interval from the give-up point — send resumes
    hs.tick(2000 + 30000)
    assert len(hs.sock.sent) == 5


def test_fast_retry_clears_on_matching_reply():
    """A matching reply during a retry burst returns to long-interval cadence."""
    hs = _make_hs(long_resync_ms=30000, fast_retry_ms=500, max_fast_retries=3)

    hs.tick(0)        # Send #1, t1=0
    hs.tick(500)      # Retry #1, t1=500
    hs.tick(1000)     # Retry #2, t1=1000
    assert len(hs.sock.sent) == 3
    pending = hs._pending_t1
    assert pending == 1000

    # Reply for the most recent hello arrives — clears the burst
    assert hs.handle_reply(f"I:{pending}:5:10:60:1920:1080") is True
    assert hs._pending_t1 is None
    assert hs._fast_retry_count == 0
    assert hs.drone_fps == 60

    # Next hello waits the full long interval, measured from sent_at (1000)
    hs.tick(2000)
    assert len(hs.sock.sent) == 3
    hs.tick(31000)
    assert len(hs.sock.sent) == 4


# ─── t1 echo freshness ──────────────────────────────────────────────────────


def test_handle_reply_drops_unmatched_t1():
    """A reply whose t1 doesn't match the pending hello is dropped."""
    hs = _make_hs()
    hs.tick(1000)
    pending = hs._pending_t1
    assert pending == 1000

    # Mismatch — older t1 from a previous burst still in the buffer
    result = hs.handle_reply("I:999:5:10:60:1920:1080")
    assert result is False
    assert hs.drone_fps is None
    assert hs.unmatched_replies == 1

    # Pending unchanged — still waiting on t1=1000
    assert hs._pending_t1 == 1000


def test_handle_reply_drops_when_no_pending():
    """A reply with no pending hello (e.g. before first send) is dropped."""
    hs = _make_hs()
    result = hs.handle_reply("I:1000:5:10:60:1920:1080")
    assert result is False
    assert hs.drone_fps is None
    assert hs.unmatched_replies == 1


def test_handle_reply_drops_duplicate_after_match():
    """A second reply with the same t1 (duplicate retransmission) is dropped."""
    hs = _make_hs()
    hs.tick(1000)
    t1 = hs._pending_t1

    assert hs.handle_reply(f"I:{t1}:5:10:60:1920:1080") is True
    assert hs.drone_fps == 60

    # Duplicate of the same reply — pending was already cleared
    result = hs.handle_reply(f"I:{t1}:5:10:30:1280:720")
    assert result is False
    assert hs.unmatched_replies == 1
    # State must NOT be overwritten by the duplicate
    assert hs.drone_fps == 60
    assert hs.drone_xres == 1920


def test_handle_reply_after_retry_only_matches_latest():
    """During a retry burst, only the latest t1 is accepted; earlier replies drop."""
    hs = _make_hs(fast_retry_ms=500, max_fast_retries=3)
    hs.tick(0)        # t1=0
    hs.tick(500)      # retry, t1=500
    hs.tick(1000)     # retry, t1=1000

    # A late reply for the original hello (t1=0) — drop
    assert hs.handle_reply("I:0:5:10:60:1920:1080") is False
    assert hs.drone_fps is None
    assert hs.unmatched_replies == 1

    # A reply for the latest pending — accept
    assert hs.handle_reply("I:1000:5:10:60:1920:1080") is True
    assert hs.drone_fps == 60


# ─── Reply parsing (preserved coverage) ─────────────────────────────────────


def test_handle_reply_parses_valid_message():
    """A valid I: reply for the pending hello updates fps/xres/yres."""
    hs = _make_hs()
    hs.tick(1000)
    assert hs.handle_reply("I:1000:1005:1010:60:1920:1080") is True
    assert hs.drone_fps == 60
    assert hs.drone_xres == 1920
    assert hs.drone_yres == 1080
    assert hs.get_fps() == 60


def test_handle_reply_invalid_message():
    """Test that handle_reply() rejects malformed messages."""
    hs = _make_hs()
    hs.tick(1000)

    # Wrong prefix
    assert hs.handle_reply("P:0:long:5:10:20:5000:10:2500:20:1000") is False
    assert hs.get_fps() == 90

    # Too few fields
    assert hs.handle_reply("I:1000:1005") is False
    assert hs.get_fps() == 90

    # Non-numeric fps
    assert hs.handle_reply("I:1000:1005:1010:abc:1920:1080") is False
    assert hs.get_fps() == 90

    # Pending should still be intact (parse failure ≠ unmatched-counter bump)
    assert hs._pending_t1 == 1000


# ─── RTT (clock-offset-cancelling) ──────────────────────────────────────────


def test_rtt_basic_calculation():
    """rtt = (gs_recv - t1) - (t3 - t2). Drone-side processing time cancels."""
    hs = _make_hs(rtt_ema_alpha=0.5)
    hs.tick(1000)  # send hello with t1=1000
    # Drone receives at t2=5_000_000_000 (huge offset proves clocks differ),
    # processes for 3ms, replies at t3=5_000_000_003.
    # GS receives the reply at gs_recv=1010, so the on-wire round trip is
    # (1010 - 1000) - (5_000_000_003 - 5_000_000_000) = 10 - 3 = 7 ms.
    assert hs.handle_reply("I:1000:5000000000:5000000003:60:1920:1080",
                           gs_recv_ms=1010) is True
    assert hs.last_rtt_ms == 7
    assert hs.avg_rtt_ms == 7.0  # First sample seeds the EMA exactly


def test_rtt_ignores_clock_offset():
    """RTT must be the same regardless of how far GS↔drone clocks differ."""
    hs1 = _make_hs(rtt_ema_alpha=0.5)
    hs1.tick(1000)
    # Drone clock offset: small
    hs1.handle_reply("I:1000:2000:2003:60:1920:1080", gs_recv_ms=1015)

    hs2 = _make_hs(rtt_ema_alpha=0.5)
    hs2.tick(1000)
    # Same on-wire RTT, but drone clock is 5 billion ms ahead
    hs2.handle_reply("I:1000:5000002000:5000002003:60:1920:1080", gs_recv_ms=1015)

    # Both: (1015-1000) - (2003-2000) = 15 - 3 = 12
    assert hs1.last_rtt_ms == 12
    assert hs2.last_rtt_ms == 12


def test_rtt_ema_smoothing():
    """avg_rtt_ms tracks an EMA of the most recent samples."""
    hs = _make_hs(rtt_ema_alpha=0.5)

    hs.tick(0)
    hs.handle_reply("I:0:100:101:60:1920:1080", gs_recv_ms=11)  # rtt = 11 - 1 = 10
    assert hs.last_rtt_ms == 10
    assert hs.avg_rtt_ms == 10.0

    # Schedule next, send again
    hs.tick(30000)
    hs.handle_reply("I:30000:200:202:60:1920:1080", gs_recv_ms=30022)  # rtt = 22 - 2 = 20
    assert hs.last_rtt_ms == 20
    # EMA: 0.5 * 20 + 0.5 * 10 = 15
    assert hs.avg_rtt_ms == 15.0


def test_rtt_negative_sample_dropped():
    """A negative RTT (clock anomaly or bug) must not corrupt avg_rtt_ms."""
    hs = _make_hs(rtt_ema_alpha=0.5)

    hs.tick(1000)
    # Drone-side processing reported as longer than the round trip.
    # rtt = (1010 - 1000) - (2050 - 2000) = 10 - 50 = -40
    result = hs.handle_reply("I:1000:2000:2050:60:1920:1080", gs_recv_ms=1010)
    # Reply still accepted (FPS/res update), but RTT sample dropped.
    assert result is True
    assert hs.drone_fps == 60
    assert hs.last_rtt_ms is None
    assert hs.avg_rtt_ms is None
    assert hs.rtt_invalid_count == 1


def test_rtt_omitted_when_gs_recv_not_provided():
    """Backward-compat: handle_reply without gs_recv_ms still works."""
    hs = _make_hs()
    hs.tick(1000)
    assert hs.handle_reply("I:1000:2000:2003:60:1920:1080") is True
    assert hs.drone_fps == 60
    assert hs.last_rtt_ms is None
    assert hs.avg_rtt_ms is None


def test_rtt_unmatched_reply_doesnt_update():
    """An unmatched reply must not bump RTT state."""
    hs = _make_hs()
    hs.tick(1000)
    # Mismatched t1 — dropped before RTT computation
    assert hs.handle_reply("I:999:2000:2003:60:1920:1080", gs_recv_ms=1010) is False
    assert hs.last_rtt_ms is None
    assert hs.unmatched_replies == 1


# ─── force_resync ───────────────────────────────────────────────────────────


def test_force_resync_sends_immediately_on_next_tick():
    """force_resync() should clear scheduling so the next tick fires a hello."""
    hs = _make_hs(long_resync_ms=30000)

    # Establish steady state with a successful handshake
    hs.tick(0)
    t1 = hs._pending_t1
    hs.handle_reply(f"I:{t1}:5:10:60:1920:1080")
    assert len(hs.sock.sent) == 1

    # Without force, next hello waits ~30s
    hs.tick(1000)
    assert len(hs.sock.sent) == 1

    # Force resync — next tick should send right away
    hs.force_resync()
    hs.tick(1001)
    assert len(hs.sock.sent) == 2
