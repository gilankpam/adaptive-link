#!/usr/bin/env python3
"""Unit tests for handshake protocol in alink_gs."""
import pytest
import sys
import os

# Add parent directory to path so we can import from alink_gs
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

# Import module-level constants and classes by executing the script up to __main__
_gs_path = os.path.join(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))), 'ground-station', 'alink_gs')
with open(_gs_path) as _f:
    _code = _f.read().split("if __name__")[0]
exec(_code)

def test_handshake_state_transitions():
    hs = HandshakeClient(None, '127.0.0.1', 1234, resync_interval_s=1, retry_interval_ms=500)
    
    assert hs.state == HandshakeState.INIT
    
    class FakeSocket:
        def __init__(self):
            self.sent = []
        def sendto(self, data, addr):
            self.sent.append(data)
            
    hs.sock = FakeSocket()
    
    hs.tick(100)
    assert hs.state == HandshakeState.PENDING
    assert len(hs.sock.sent) == 1
    
    hs.tick(200)
    assert len(hs.sock.sent) == 1  # haven't waited retry_interval yet
    
    hs.tick(700)
    assert len(hs.sock.sent) == 2  # retry
    
    hs.handle_reply("I:700:710:720:60:1920:1080", 730)
    assert hs.state == HandshakeState.SYNCED
    assert hs.is_synced()
    assert hs.get_fps() == 60
    
    # Resync triggers after resync_interval_s
    hs.tick(730 + 1000 + 1)
    assert hs.state == HandshakeState.SYNCED  # Doesn't downgrade
    assert len(hs.sock.sent) == 3

def test_handshake_offset_math():
    hs = HandshakeClient(None, '1', 1)
    hs.pending_t1 = 100
    
    t1 = 100
    t2 = 105
    t3 = 110
    t4 = 115
    
    # offset = ((105 - 100) + (110 - 115)) / 2 = (5 - 5) / 2 = 0
    # rtt = (115 - 100) - (110 - 105) = 15 - 5 = 10
    hs.handle_reply(f"I:{t1}:{t2}:{t3}:90:1080:720", t4)
    assert hs.offset_ms == 0
    assert hs.rtt_ms == 10
    assert hs.is_synced()

    hs.pending_t1 = 200
    # Clock offset drone is +50ms ahead
    t1 = 200
    t2 = 255 # t1(GS) is 200, drone received at 255 (travel 5ms)
    t3 = 260
    t4 = 215 # GS receives at 215 (travel 5ms: 215 - 200 = 15ms rtt)
    # offset = ((255 - 200) + (260 - 215)) / 2  = (55 + 45)/2 = 50
    hs.handle_reply(f"I:{t1}:{t2}:{t3}:90:1080:720", t4)
    assert hs.offset_ms == 50

def test_handshake_rtt_filters():
    hs = HandshakeClient(None, '1', 1)
    hs.pending_t1 = 100

    # RTT too high (>100)
    # rtt = (205-100) - (110-105) = 105 - 5 = 100 (pass)
    hs.handle_reply("I:100:105:110:90:0:0", 205)
    assert hs.is_synced()
    
    hs.state = HandshakeState.PENDING
    hs.pending_t1 = 100
    # rtt = (206-100) - (110-105) = 101 (fail)
    hs.handle_reply("I:100:105:110:90:0:0", 206)
    assert not hs.is_synced()

    # RTT < 0
    hs.pending_t1 = 100
    hs.handle_reply("I:100:105:110:90:0:0", 100) # RTT = (100-100) - 5 = -5
    assert not hs.is_synced()

    # Drone too slow (t3 - t2 > 10)
    hs.pending_t1 = 100
    hs.handle_reply("I:100:105:120:90:0:0", 130) # t3-t2 = 15
    assert not hs.is_synced()

def test_handshake_stale_reply():
    hs = HandshakeClient(None, '1', 1)
    hs.state = HandshakeState.PENDING
    hs.pending_t1 = 200
    
    hs.handle_reply("I:100:105:110:90:0:0", 115)
    assert hs.state == HandshakeState.PENDING

def test_handshake_correct_timestamp():
    hs = HandshakeClient(None, '1', 1, default_fps=120)
    
    assert hs.get_fps() == 120
    assert hs.correct_timestamp(1000) == 1000
    
    hs.pending_t1 = 100
    hs.handle_reply("I:100:150:150:60:0:0", 100) # offset=50
    assert hs.is_synced()
    
    assert hs.get_fps() == 60
    assert hs.correct_timestamp(1000) == 1050
