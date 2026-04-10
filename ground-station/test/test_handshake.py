#!/usr/bin/env python3
"""Unit tests for handshake protocol in alink_gs.

Tests the simplified HandshakeClient that handles FPS/resolution detection
without clock synchronization.
"""
import pytest
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


def test_handshake_basic_initialization():
    """Test handshake client initializes with default FPS."""
    hs = HandshakeClient(None, '127.0.0.1', 1234, default_fps=90, retry_interval_ms=500)
    
    assert hs.drone_fps is None
    assert hs.drone_xres is None
    assert hs.drone_yres is None
    assert hs.get_fps() == 90  # Returns default


def test_handshake_tick_sends_hello():
    """Test that tick() sends hello messages at retry_interval."""
    class FakeSocket:
        def __init__(self):
            self.sent = []
        def sendto(self, data, addr):
            self.sent.append((data, addr))
            
    hs = HandshakeClient(FakeSocket(), '10.5.0.10', 9999, default_fps=90, retry_interval_ms=500)
    
    # First tick should send hello (0 - 0 > 500 is false, but _last_hello_ms starts at 0)
    # Actually: 100 - 0 = 100 which is < 500, so no send
    hs.tick(100)
    assert len(hs.sock.sent) == 0  # Not enough time passed from initial 0
    
    # After retry_interval has passed, should send
    hs.tick(600)  # 600 - 0 = 600 > 500
    assert len(hs.sock.sent) == 1
    assert hs.sock.sent[0][1] == ('10.5.0.10', 9999)
    # Message format is \x00\x00\x00<length>H:<timestamp>
    msg = hs.sock.sent[0][0]
    assert msg[4:5] == b'H'  # Check H: prefix after length field
    assert b':600' in msg  # Check timestamp is included
    
    # Second tick within retry_interval should not send
    hs.tick(700)  # 700 - 600 = 100 < 500
    assert len(hs.sock.sent) == 1
    
    # After retry_interval, should send again
    hs.tick(1200)  # 1200 - 600 = 600 > 500
    assert len(hs.sock.sent) == 2


def test_handle_reply_parses_fps_and_resolution():
    """Test that handle_reply() correctly parses I: messages."""
    hs = HandshakeClient(None, '1', 1, default_fps=90)
    
    # Parse a valid I: reply
    result = hs.handle_reply("I:1000:1005:1010:60:1920:1080")
    
    assert result is True
    assert hs.drone_fps == 60
    assert hs.drone_xres == 1920
    assert hs.drone_yres == 1080
    assert hs.get_fps() == 60


def test_handle_reply_invalid_message():
    """Test that handle_reply() rejects invalid messages."""
    hs = HandshakeClient(None, '1', 1, default_fps=90)
    
    # Invalid: not I: prefix
    assert hs.handle_reply("P:0:long:5:10:20:5000:10:2500:20:1000") is False
    assert hs.get_fps() == 90  # Still using default
    
    # Invalid: too few fields
    assert hs.handle_reply("I:1000:1005") is False
    assert hs.get_fps() == 90
    
    # Invalid: non-numeric fields
    assert hs.handle_reply("I:1000:1005:1010:abc:1920:1080") is False
    assert hs.get_fps() == 90


def test_handle_reply_updates_on_new_info():
    """Test that handle_reply() updates FPS/resolution on new replies."""
    hs = HandshakeClient(None, '1', 1, default_fps=90)
    
    # First reply
    hs.handle_reply("I:1000:1005:1010:60:1920:1080")
    assert hs.get_fps() == 60
    assert hs.drone_xres == 1920
    assert hs.drone_yres == 1080
    
    # Second reply with different values
    hs.handle_reply("I:2000:2005:2010:30:1280:720")
    assert hs.get_fps() == 30
    assert hs.drone_xres == 1280
    assert hs.drone_yres == 720


def test_get_fps_returns_default_when_not_synced():
    """Test that get_fps() returns default when no reply received."""
    hs = HandshakeClient(None, '1', 1, default_fps=120)
    
    assert hs.get_fps() == 120
    assert hs.drone_fps is None


def test_handshake_retry_interval_config():
    """Test that retry_interval_ms is configurable."""
    hs = HandshakeClient(None, '1', 1, default_fps=90, retry_interval_ms=1000)
    assert hs._retry_interval_ms == 1000
    
    hs2 = HandshakeClient(None, '1', 1, default_fps=90, retry_interval_ms=30000)
    assert hs2._retry_interval_ms == 30000