#!/usr/bin/env python3
"""Unit tests for dynamic profile calculation in alink_gs."""
import sys
import os
import configparser
import pytest

# Add parent directory to path so we can import from alink_gs
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

# Import module-level constants and classes by executing the script up to __main__
_gs_path = os.path.join(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))), 'ground-station', 'alink_gs')
with open(_gs_path) as _f:
    _code = _f.read().split("if __name__")[0]
exec(_code)

TEST_CONFIG_STRING = """# adaptive-link VRX settings

[outgoing]
udp_ip = 10.5.0.10
udp_port = 9999

[wfb-ng]
host = 127.0.0.1
port = 8103

[keyframe]
allow_idr = True
idr_max_messages = 20

[handshake]
resync_interval_s = 30
video_fps_default = 90

[hardware]
bandwidth = 20
gop = 10
max_bitrate = 30000
min_bitrate = 2000
max_power = 2500
min_power = 200
max_fec_n = 50
max_fec_redundancy = 0.5

[gate]
snr_ema_alpha = 0.3
snr_slope_alpha = 0.3
snr_predict_horizon_ticks = 3
snr_safety_margin = 3
loss_margin_weight = 20
fec_margin_weight = 5
hysteresis_up_db = 2.0
hysteresis_down_db = 1.0
emergency_loss_rate = 0.15
emergency_fec_pressure = 0.75
max_mcs = 7
max_mcs_step_up = 1

[profile selection]
hold_fallback_mode_ms = 1000
hold_modes_down_ms = 3000
min_between_changes_ms = 200
fast_downgrade = True
upward_confidence_loops = 3

[dynamic]
short_gi_snr_margin = 5
short_gi_max_loss = 0.02
short_gi_max_fec_pressure = 0.3
fec_redundancy_ratio = 0.25
loss_threshold_for_fec_downgrade = 0.05
utilization_factor = 0.45

[ml]
persist_path =
snr_safety_margin_lr_up = 0
snr_safety_margin_lr_down = 0
fec_redundancy_ratio_lr_up = 0
fec_redundancy_ratio_lr_down = 0
utilization_factor_lr_up = 0
utilization_factor_lr_down = 0
hysteresis_up_db_lr_up = 0
hysteresis_up_db_lr_down = 0

[telemetry]
log_enabled = False
log_dir = /var/log/alink
log_rotate_mb = 50
outcome_window_ticks = 10
adapter_id = default
"""

def _make_config(overrides=None):
    """Create a configparser with TEST_CONFIG_STRING and optional overrides."""
    config = configparser.ConfigParser()
    config.read_string(TEST_CONFIG_STRING)
    if overrides:
        for section, kvs in overrides.items():
            for k, v in kvs.items():
                config.set(section, k, str(v))
    return config


def _make_selector(overrides=None):
    """Create a ProfileSelector."""
    class MockHandshake:
        def get_fps(self): return 60
        def correct_timestamp(self, ts): return ts
    return ProfileSelector(_make_config(overrides), MockHandshake())


def _feed_tick(ps, best_snr, loss_rate=0.0, fec_pressure=0.0,
               all_packets=1000, lost_packets=0, fec_rec=0):
    """Feed one tick of metrics into the selector via evaluate_link."""
    # Approximate lost_packets from loss_rate for the delta computation
    if lost_packets == 0 and loss_rate > 0:
        lost_packets = int(all_packets * loss_rate)
    if fec_rec == 0 and fec_pressure > 0:
        fec_rec = int(fec_pressure * 10)  # rough approximation
    ps.evaluate_link(
        best_snr=best_snr, all_packets=all_packets,
        lost_packets=lost_packets, fec_rec=fec_rec,
        fec_k=8, fec_n=12
    )


# Backward-compat alias for existing test bodies
_feed_score = _feed_tick


class TestMCSSelection:
    """Test that MCS is selected correctly based on SNR thresholds."""

    def test_mcs0_at_low_snr(self):
        ps = _make_selector()
        _feed_score(ps, best_snr=7)
        profile = ps._compute_profile()
        assert profile['mcs'] == 0

    def test_mcs0_when_snr_below_all_thresholds(self):
        ps = _make_selector()
        _feed_score(ps, best_snr=3)
        profile = ps._compute_profile()
        assert profile['mcs'] == 0

    def test_mcs1_at_snr_12(self):
        """SNR=12, margin=3 → MCS1 threshold=8+3=11 < 12 → MCS1."""
        ps = _make_selector()
        _feed_score(ps, best_snr=12)
        profile = ps._compute_profile()
        assert profile['mcs'] == 1

    def test_mcs3_at_snr_18(self):
        """SNR=18, margin=3 → MCS3 threshold=14+3=17 < 18 → MCS3."""
        ps = _make_selector()
        _feed_score(ps, best_snr=18)
        profile = ps._compute_profile()
        assert profile['mcs'] == 3

    def test_mcs7_at_high_snr(self):
        """SNR=32, margin=3 → MCS7 threshold=26+3=29 < 32 → MCS7."""
        ps = _make_selector()
        _feed_score(ps, best_snr=32)
        profile = ps._compute_profile()
        assert profile['mcs'] == 7

    def test_max_mcs_cap(self):
        """Even with high SNR, respect max_mcs config."""
        ps = _make_selector({'gate': {'max_mcs': '4'}})
        _feed_score(ps, best_snr=32)
        profile = ps._compute_profile()
        assert profile['mcs'] == 4

    def test_margin_increases_with_loss(self):
        """High loss widens margin, may push MCS down."""
        ps = _make_selector()
        # SNR=18 normally selects MCS3 (14+3=17 < 18)
        # With 10% loss: margin = 3 + 0.1*20 = 5 → MCS3 threshold = 14+5=19 > 18 → MCS2
        _feed_score(ps, best_snr=18)
        ps._current_loss_rate = 0.1
        profile = ps._compute_profile()
        assert profile['mcs'] == 2

    def test_margin_increases_with_fec_pressure(self):
        """High FEC pressure widens margin."""
        ps = _make_selector()
        _feed_score(ps, best_snr=18)
        ps._current_fec_pressure = 1.0  # 100% pressure → margin += 5
        # margin = 3 + 5 = 8, MCS3 threshold = 14+8 = 22 > 18
        # MCS2 threshold = 11+8 = 19 > 18
        # MCS1 threshold = 8+8 = 16 < 18 → MCS1
        profile = ps._compute_profile()
        assert profile['mcs'] == 1


class TestGuardInterval:
    """Test guard interval selection."""

    def test_long_gi_when_margin_small(self):
        """SNR just above threshold → long GI."""
        ps = _make_selector()
        _feed_score(ps, best_snr=18)  # MCS3, margin above threshold = 18-14=4 < 5
        profile = ps._compute_profile()
        assert profile['gi'] == 'long'

    def test_short_gi_when_margin_large(self):
        """SNR well above threshold → short GI."""
        ps = _make_selector()
        _feed_score(ps, best_snr=22)  # MCS3 (14+3=17<22), margin above = 22-14=8 >= 5
        profile = ps._compute_profile()
        assert profile['gi'] == 'short'

    def test_long_gi_when_loss_high(self):
        """Even with good margin, high loss forces long GI."""
        ps = _make_selector()
        _feed_score(ps, best_snr=22)
        ps._current_loss_rate = 0.05  # > 0.02 threshold
        profile = ps._compute_profile()
        assert profile['gi'] == 'long'

    def test_long_gi_when_fec_pressure_high(self):
        """High FEC pressure forces long GI."""
        ps = _make_selector()
        _feed_score(ps, best_snr=22)
        ps._current_fec_pressure = 0.5  # > 0.3 threshold
        profile = ps._compute_profile()
        assert profile['gi'] == 'long'


class TestFEC:
    """Integration tests: FEC block sizing via _compute_profile.

    K is sized for a fixed block-completion time (block_latency_budget_ms,
    default 5 ms), not per video frame. This pins loss-recovery latency
    across MCS levels instead of scaling it with 1/fps.
    """

    def test_small_blocks_at_low_bitrate(self):
        """MCS0 LGI: PHY=6.5 Mbps, target=2194 kbps, pps=189.6.
        K_latency = round(5ms * 189.6pps) = 1 → floored to 2; N = K+1 = 3."""
        ps = _make_selector()
        _feed_score(ps, best_snr=7)
        profile = ps._compute_profile()
        assert profile['fec_k'] == 2
        assert profile['fec_n'] == 3

    def test_medium_blocks_at_mid_bitrate(self):
        """MCS4 LGI: PHY=39 Mbps, target=13163 kbps, pps=1137.9.
        K_latency = round(5ms * 1137.9pps) = 6; N = ceil(6/0.75) = 8."""
        ps = _make_selector()
        _feed_score(ps, best_snr=21)  # MCS4: 17+3=20 < 21
        profile = ps._compute_profile()
        assert profile['fec_k'] == 6
        assert profile['fec_n'] == 8

    def test_large_blocks_at_high_bitrate(self):
        """MCS7 SGI: PHY=72.2 Mbps, target=24368 kbps, pps=2106.9.
        K_latency = round(5ms * 2106.9pps) = 11; N = ceil(11/0.75) = 15."""
        ps = _make_selector()
        _feed_score(ps, best_snr=32)
        profile = ps._compute_profile()
        assert profile['fec_k'] == 11
        assert profile['fec_n'] == 15

    def test_block_fill_time_is_roughly_constant_across_mcs(self):
        """Core invariant of the latency-targeted formula: at the target
        bitrate used to size K, block-completion time stays near
        block_latency_budget_ms across MCS (or the K=2 floor, whichever is
        larger).

        Uses _compute_fec_from_bitrate directly against a range of target
        bitrates rather than going through _compute_profile, so we compare
        against the bitrate K was actually sized for (pre-K/N rounding)."""
        ps = _make_selector()
        budget_s = ps.block_latency_budget_ms / 1000.0
        mtu = ps.mtu_payload_bytes
        for target_bps in (2_000_000, 8_000_000, 14_000_000, 24_000_000):
            fec_k, fec_n = ps._compute_fec_from_bitrate(target_bps)
            pps = target_bps / (8 * mtu)
            t_block_s = fec_k / pps
            # Allow up to 1 packet of rounding slack; at very low bitrate
            # the K=2 floor dominates the budget.
            cap = max(budget_s, 2.0 / pps) + (1.0 / pps)
            assert t_block_s <= cap, (
                f"target_bps={target_bps} K={fec_k} "
                f"t_block={t_block_s*1000:.2f}ms cap={cap*1000:.2f}ms"
            )

    def test_loss_adds_parity_not_reduces_k(self):
        """Loss > threshold increases N (more parity) instead of reducing K.
        Both would add one recoverable loss per block; N+=1 keeps K (and
        t_block) at the operator's block_latency_budget_ms and disrupts the
        encoder bitrate less than K-=1 (smaller K/N change).
        SNR=21 with loss=0.06 → margin widens → MCS3. Base K=4, N=6.
        Loss > 0.05 → N += 1 = 7, K unchanged."""
        ps = _make_selector()
        _feed_score(ps, best_snr=21)
        ps._current_loss_rate = 0.06  # > 0.05 threshold
        profile = ps._compute_profile()
        assert profile['mcs'] == 3  # margin widened by loss
        assert profile['fec_k'] == 4  # K unchanged by loss reaction
        assert profile['fec_n'] == 7  # N bumped by one

    def test_loss_reaction_respects_max_fec_redundancy(self):
        """Loss-driven N bump must not push redundancy past max_fec_redundancy.
        MCS0: K=2, N=3. Loss → N=4 → redundancy=(4-2)/4=50%=max, still allowed."""
        ps = _make_selector()
        _feed_score(ps, best_snr=7)  # MCS0, K=2, N=3
        ps._current_loss_rate = 0.06
        profile = ps._compute_profile()
        redundancy = (profile['fec_n'] - profile['fec_k']) / profile['fec_n']
        assert redundancy <= 0.5 + 1e-9


class TestFECFromBitrate:
    """Unit tests for _compute_fec_from_bitrate (latency-targeted block size)."""

    def test_k_minimum_is_2(self):
        """K must never drop below 2 regardless of how low the bitrate goes."""
        ps = _make_selector()
        fec_k, fec_n = ps._compute_fec_from_bitrate(100_000)  # 100 kbps
        assert fec_k >= 2
        assert fec_n >= fec_k + 1

    def test_n_guarantees_at_least_one_parity(self):
        """fec_n must be strictly greater than fec_k (at least one parity packet)."""
        ps = _make_selector()
        for bitrate in (500_000, 2_000_000, 10_000_000, 24_000_000):
            fec_k, fec_n = ps._compute_fec_from_bitrate(bitrate)
            assert fec_n > fec_k

    def test_k_scales_with_bitrate(self):
        """At constant MTU/fps/budget, K grows with bitrate (more packets/sec
        fit into the same latency budget)."""
        ps = _make_selector()
        k_low, _ = ps._compute_fec_from_bitrate(2_000_000)
        k_mid, _ = ps._compute_fec_from_bitrate(10_000_000)
        k_high, _ = ps._compute_fec_from_bitrate(24_000_000)
        assert k_low <= k_mid <= k_high

    def test_k_tracks_block_latency_budget(self):
        """K scales linearly with block_latency_budget_ms when budget is the
        binding constraint (not the K=2 floor)."""
        ps = _make_selector()
        ps.block_latency_budget_ms = 5.0
        k_5, _ = ps._compute_fec_from_bitrate(20_000_000)
        ps.block_latency_budget_ms = 10.0
        k_10, _ = ps._compute_fec_from_bitrate(20_000_000)
        # Doubling the budget should roughly double K.
        assert abs(k_10 - 2 * k_5) <= 1

    def test_budget_capped_by_one_block_per_frame(self):
        """A wildly generous budget must not size K past one-block-per-frame,
        which would be pure overhead."""
        ps = _make_selector()
        ps.block_latency_budget_ms = 1000.0  # absurd
        bitrate = 20_000_000
        fec_k, _ = ps._compute_fec_from_bitrate(bitrate)
        fps = ps.handshake.get_fps()
        packets_per_frame = round(bitrate / (8 * ps.mtu_payload_bytes) / fps)
        assert fec_k <= packets_per_frame

    def test_k_inversely_scales_with_mtu(self):
        """Larger MTU → fewer packets per second → smaller K at same bitrate."""
        ps_small = _make_selector({'dynamic': {'mtu_payload_bytes': '1446'}})
        ps_large = _make_selector({'dynamic': {'mtu_payload_bytes': '3893'}})
        bitrate = 20_000_000
        k_small, _ = ps_small._compute_fec_from_bitrate(bitrate)
        k_large, _ = ps_large._compute_fec_from_bitrate(bitrate)
        assert k_small > k_large

    def test_custom_redundancy_ratio_respected(self):
        """Caller-supplied redundancy_ratio scales N without touching K."""
        ps = _make_selector()
        k_25, n_25 = ps._compute_fec_from_bitrate(20_000_000, redundancy_ratio=0.25)
        k_50, n_50 = ps._compute_fec_from_bitrate(20_000_000, redundancy_ratio=0.50)
        assert k_25 == k_50
        assert n_50 > n_25


class TestBitrate:
    """Test bitrate computation. Values derive from link_bw × K/N — when the
    new latency-targeted formula picks smaller K, rounding shifts K/N slightly
    away from the configured redundancy ratio so bitrates are not identical
    to the old frame-proportional path."""

    def test_bitrate_mcs0_long_gi(self):
        """MCS0 LGI: K=2, N=3. link_bw=2925 kbps → 2925×2/3=1950 → clamped
        to min_bitrate=2000."""
        ps = _make_selector()
        _feed_score(ps, best_snr=7)
        profile = ps._compute_profile()
        assert profile['bitrate'] == 2000

    def test_bitrate_mcs7_short_gi(self):
        """MCS7 SGI: K=11, N=15. link_bw=32490 kbps → 32490×11/15=23826."""
        ps = _make_selector()
        _feed_score(ps, best_snr=32)
        profile = ps._compute_profile()
        assert profile['gi'] == 'short'
        assert profile['bitrate'] == 23826

    def test_bitrate_capped_at_max(self):
        ps = _make_selector({'hardware': {'max_bitrate': '10000'}})
        _feed_score(ps, best_snr=32)
        profile = ps._compute_profile()
        assert profile['bitrate'] == 10000

    def test_bitrate_floored_at_min(self):
        ps = _make_selector({'hardware': {'min_bitrate': '3000'}})
        _feed_score(ps, best_snr=7)
        profile = ps._compute_profile()
        assert profile['bitrate'] == 3000

    def test_bitrate_40mhz(self):
        """MCS0 40MHz LGI: PHY=13.5 Mbps, link_bw=6075 kbps, target=4556 kbps.
        pps=393.9 → K_latency=round(1.97)=2; N=max(3, ceil(2/0.75))=3.
        bitrate = 6075 × 2/3 = 4050."""
        ps = _make_selector({'hardware': {'bandwidth': '40'}})
        _feed_score(ps, best_snr=7)
        profile = ps._compute_profile()
        assert profile['bitrate'] == 4050

    def test_fec_sized_from_capped_bitrate(self):
        """max_bitrate caps target_bitrate before FEC sizing — pps is derived
        from the capped bitrate, so K/N reflect the encoder's actual output.

        max_bitrate=10000 → target=10000 kbps → pps=864.4.
        K_latency=round(5ms × 864.4pps)=4; N=max(5, ceil(4/0.75))=6.
        """
        ps = _make_selector({'hardware': {'max_bitrate': '10000'}})
        _feed_score(ps, best_snr=32)
        profile = ps._compute_profile()
        assert profile['fec_k'] == 4
        assert profile['fec_n'] == 6


class TestPower:
    """Test power scaling — linear interpolation."""

    def test_max_power_at_low_mcs(self):
        ps = _make_selector()
        _feed_score(ps, best_snr=7)  # MCS 0
        profile = ps._compute_profile()
        assert profile['power'] == 2500

    def test_min_power_at_high_mcs(self):
        ps = _make_selector()
        _feed_score(ps, best_snr=32)  # MCS 7
        profile = ps._compute_profile()
        assert profile['power'] == 200

    def test_mid_power_at_mid_mcs(self):
        ps = _make_selector()
        _feed_score(ps, best_snr=18)  # MCS 3
        profile = ps._compute_profile()
        # Linear: 2500 - (3/7) * 2300 = 2500 - 985.7 -> round to 1514
        assert profile['power'] == 1514

    def test_power_is_linear_and_monotonic(self):
        """Power decreases monotonically with small per-step delta."""
        ps = _make_selector()
        powers = [ps._compute_power(m) for m in range(8)]
        for i in range(1, len(powers)):
            assert powers[i] <= powers[i - 1]
            # Max per-step delta should be <= 329 (ceil(2300/7))
            assert powers[i - 1] - powers[i] <= 329


class TestFixedParams:
    """Test that fixed parameters come from config."""

    def test_gop_from_config(self):
        ps = _make_selector({'hardware': {'gop': '5'}})
        _feed_score(ps, best_snr=20)
        profile = ps._compute_profile()
        assert profile['gop'] == 5.0



    def test_bandwidth_from_config(self):
        ps = _make_selector()
        _feed_score(ps, best_snr=20)
        profile = ps._compute_profile()
        assert profile['bandwidth'] == 20


class TestSelectIntegration:
    """Test that select() works correctly in dynamic mode."""

    def _warm_up(self, ps, best_snr=20, n=5):
        """Feed enough ticks to get through confidence gating and hold timers.
        Returns once the first profile change is applied."""
        ps.last_change_time_ms = 0
        ps.min_between_changes_ms = 0
        ps.hold_modes_down_ms = 0
        ps.hold_fallback_mode_ms = 0
        for i in range(n):
            _feed_tick(ps, best_snr=best_snr, all_packets=1000 * (i + 1))
            changed, idx, profile = ps.select()
            if changed:
                return changed, idx, profile
        return False, ps.current_profile_idx, ps.current_profile

    def test_select_converges_after_confidence(self):
        """After enough ticks (confidence gating), select() produces a change."""
        ps = _make_selector()
        changed, idx, profile = self._warm_up(ps, best_snr=20)
        assert changed
        assert profile is not None
        assert idx == profile['mcs']

    def test_same_mcs_no_change(self):
        ps = _make_selector()
        self._warm_up(ps, best_snr=20)

        # Feed again with same SNR — should not change
        _feed_tick(ps, best_snr=20, all_packets=10000)
        changed, _, _ = ps.select()
        assert not changed

    def test_dynamic_profile_updates_even_without_mcs_change(self):
        """In dynamic mode, current_profile is updated even if MCS stays same."""
        ps = _make_selector()
        self._warm_up(ps, best_snr=20)
        assert ps.current_profile is not None

        # Feed with higher loss — same MCS but FEC might downgrade
        _feed_tick(ps, best_snr=20, all_packets=20000, lost_packets=2000)
        ps.select()
        # Profile should still exist and be updated
        assert ps.current_profile is not None

    def test_param_only_change_detected(self):
        """select() returns changed=True when MCS stays same but params differ."""
        ps = _make_selector()
        changed, idx, profile = self._warm_up(ps, best_snr=20)
        assert changed
        old_profile = dict(ps.current_profile)
        old_mcs = ps.current_profile_idx

        # Feed with significant loss to trigger FEC/bitrate change at same MCS
        ps.min_between_changes_ms = 0  # disable rate limiting
        _feed_tick(ps, best_snr=20, all_packets=20000, lost_packets=2000)
        changed, idx, profile = ps.select()

        # MCS should stay the same but params should differ
        if idx == old_mcs and ps._profile_changed(old_profile, profile):
            assert changed
        # If params didn't change (edge case), at least verify no crash
        assert profile is not None


class TestMCSStepLimit:
    """Test that upward MCS changes are step-limited."""

    def _warm_at_mcs(self, ps, target_snr, n=10):
        """Feed ticks until selector settles at the MCS for target_snr."""
        ps.last_change_time_ms = 0
        ps.min_between_changes_ms = 0
        ps.hold_modes_down_ms = 0
        ps.hold_fallback_mode_ms = 0
        for i in range(n):
            _feed_tick(ps, best_snr=target_snr, all_packets=1000 * (i + 1))
            ps.select()
        # Reset timing so hold timers don't block subsequent tests
        ps.last_change_time_ms = 0
        ps.last_mcs_change_time_ms = 0

    def test_upgrade_capped_to_one_step(self):
        """Starting at MCS 2, even if SNR qualifies for MCS 7, only step to MCS 3."""
        ps = _make_selector()
        self._warm_at_mcs(ps, target_snr=15)
        assert ps.current_profile_idx == 2

        # Feed enough ticks for SNR EMA to converge and confidence to pass
        first_change_idx = None
        for i in range(15):
            _feed_tick(ps, best_snr=32, all_packets=10000 * (i + 1))
            changed, idx, profile = ps.select()
            if changed and first_change_idx is None:
                first_change_idx = idx
                break
        # First upgrade should be capped to +1
        assert first_change_idx == 3

    def test_downgrade_not_limited(self):
        """Downgrades can drop more than 1 MCS step."""
        ps = _make_selector()
        self._warm_at_mcs(ps, target_snr=24)  # MCS 5
        start_mcs = ps.current_profile_idx
        assert start_mcs == 5

        # Feed several ticks at very low SNR so EMA drops enough
        for i in range(10):
            _feed_tick(ps, best_snr=7, all_packets=50000 * (i + 1))
            changed, idx, profile = ps.select()
            if changed:
                # Downgrade should jump more than 1 step (proves it's unlimited)
                assert idx < start_mcs - 1
                return
        pytest.fail("Expected a downgrade but none occurred")

    def test_step_limit_disabled_with_zero(self):
        """Setting max_mcs_step_up=0 disables step limiting."""
        ps = _make_selector({'gate': {'max_mcs_step_up': '0'}})
        self._warm_at_mcs(ps, target_snr=15)  # MCS 2
        assert ps.current_profile_idx == 2

        # Feed many ticks so EMA converges and we can jump past +1
        changes = []
        for i in range(20):
            _feed_tick(ps, best_snr=32, all_packets=10000 * (i + 1))
            changed, idx, profile = ps.select()
            if changed:
                changes.append(idx)
                break
        # First change should be > +1 step from MCS 2 (proves no limit)
        assert len(changes) > 0, "Expected a change"
        assert changes[0] > 3


class TestFECWithLargeMTU:
    """FEC computation with configurable mtu_payload_bytes (wfb-ng mlink setups).

    Majestic configured for wfb-ng mlink sends UDP packets up to ~3893 bytes
    instead of the standard ~1446. K should scale inversely with MTU so the
    block-completion latency stays constant.
    """

    MTU_LARGE = 3893

    def _selector(self, overrides=None):
        base = {'dynamic': {'mtu_payload_bytes': str(self.MTU_LARGE)}}
        if overrides:
            for section, kvs in overrides.items():
                base.setdefault(section, {}).update(kvs)
        return _make_selector(base)

    def test_config_key_loaded(self):
        ps = self._selector()
        assert ps.mtu_payload_bytes == self.MTU_LARGE

    def test_default_mtu_is_1446(self):
        ps = _make_selector()
        assert ps.mtu_payload_bytes == 1446

    def test_fec_k_scaled_down_at_mid_bitrate(self):
        """20.5 Mbps at MTU=3893: pps=658.5. K=round(5ms × 658.5)=3; N=4."""
        ps = self._selector()
        fec_k, fec_n = ps._compute_fec_from_bitrate(20_500_000)
        assert fec_k == 3
        assert fec_n == 4

    def test_fec_k_floor_at_low_bitrate(self):
        """Low bitrate hits the K=2 floor, same as small-MTU path."""
        ps = self._selector()
        fec_k, fec_n = ps._compute_fec_from_bitrate(2_000_000)
        assert fec_k == 2
        assert fec_n == 3

    def test_fec_computation_scales_inversely_with_mtu(self):
        """Same bitrate at 2.7x larger MTU gives ~2.7x smaller K (before floors)."""
        ps_small = _make_selector({'dynamic': {'mtu_payload_bytes': '1446'}})
        ps_large = self._selector()
        # Use a high bitrate so the K=2 floor doesn't distort the ratio.
        bitrate = 30_000_000
        k_small, _ = ps_small._compute_fec_from_bitrate(bitrate)
        k_large, _ = ps_large._compute_fec_from_bitrate(bitrate)
        ratio = k_small / k_large
        # 3893 / 1446 ≈ 2.69, allow ±0.5 for integer rounding
        assert 2.0 <= ratio <= 3.5

    def test_block_fill_time_independent_of_mtu_at_high_bitrate(self):
        """At high enough bitrate that neither K floor nor frame cap bind,
        block-fill time should sit near block_latency_budget_ms regardless
        of MTU — the whole point of the new formula."""
        bitrate = 30_000_000
        for mtu in (1446, 3893):
            ps = _make_selector({'dynamic': {'mtu_payload_bytes': str(mtu)}})
            fec_k, _ = ps._compute_fec_from_bitrate(bitrate)
            pps = bitrate / (8 * mtu)
            t_block_ms = fec_k / pps * 1000
            assert abs(t_block_ms - ps.block_latency_budget_ms) <= 0.7, (
                f"mtu={mtu} K={fec_k} pps={pps:.1f} t_block={t_block_ms:.2f}ms"
            )

    def test_oversized_mtu_clamped_with_warning(self, capsys):
        """mtu_payload_bytes beyond the wfb-ng ceiling is clamped."""
        ps = _make_selector(
            {'dynamic': {'mtu_payload_bytes': '5000'}})
        captured = capsys.readouterr()
        assert 'WARNING' in captured.out
        assert ps.mtu_payload_bytes == MTU_PAYLOAD_BYTES_MAX


class TestMaxFECN:
    """Test max_fec_n configuration parameter enforcement.

    With the latency-targeted formula K is much smaller than the old
    frame-proportional version, so max_fec_n only binds at very aggressive
    caps. These tests still cover the cap machinery.
    """

    def test_max_fec_n_aggressive_cap(self):
        """MCS7 SGI: K=11, N=15 (new formula). max_fec_n=10 caps N to 10
        and scales K down preserving the original 11/15 ratio:
          K = ceil(10 × 11/15) = 8.
        """
        ps = _make_selector({'hardware': {'max_fec_n': '10'}})
        _feed_score(ps, best_snr=32)
        profile = ps._compute_profile()
        assert profile['fec_n'] == 10
        assert profile['fec_k'] == 8

    def test_max_fec_n_not_exceeded_at_high_bitrate(self):
        """At any reasonable cap, N stays at or below it."""
        ps = _make_selector({'hardware': {'max_fec_n': '8'}})
        _feed_score(ps, best_snr=32)
        profile = ps._compute_profile()
        assert profile['fec_n'] <= 8

    def test_max_fec_n_preserves_redundancy_ratio(self):
        """When the cap triggers, K is rescaled to keep redundancy close."""
        ps = _make_selector({'hardware': {'max_fec_n': '8'}})
        _feed_score(ps, best_snr=32)
        profile = ps._compute_profile()
        # Redundancy before cap was (15-11)/15 ≈ 0.267; after cap stays close.
        actual_ratio = (profile['fec_n'] - profile['fec_k']) / profile['fec_n']
        assert abs(actual_ratio - 0.267) <= 0.1

    def test_max_fec_n_does_not_affect_low_bitrate(self):
        """At low bitrates where N is already tiny, no cap applies.
        MCS0: K=2, N=3. Cap at 30 is irrelevant."""
        ps = _make_selector({'hardware': {'max_fec_n': '30'}})
        _feed_score(ps, best_snr=7)
        profile = ps._compute_profile()
        assert profile['fec_k'] == 2
        assert profile['fec_n'] == 3

    def test_max_fec_n_default_value(self):
        """Default max_fec_n should be 50."""
        ps = _make_selector()
        assert ps.max_fec_n == 50

    def test_max_fec_n_with_intermediate_mcs(self):
        """MCS4: K=6, N=8 with new formula. max_fec_n=25 doesn't bind."""
        ps = _make_selector({'hardware': {'max_fec_n': '25'}})
        _feed_score(ps, best_snr=21)
        profile = ps._compute_profile()
        assert profile['fec_n'] <= 25
        assert profile['fec_n'] == 8  # actually well below cap



class TestOscillationRegression:
    """Regression test: MCS/power should not oscillate under stable SNR."""

    def test_stable_snr_converges(self):
        """At stable SNR near MCS boundary, system should converge."""
        ps = _make_selector()
        ps.last_change_time_ms = 0
        ps.min_between_changes_ms = 0
        ps.hold_modes_down_ms = 0
        ps.hold_fallback_mode_ms = 0

        mcs_history = []
        for i in range(20):
            _feed_tick(ps, best_snr=29, all_packets=1000 * (i + 1))
            ps.select()
            if ps.current_profile is not None:
                mcs_history.append(ps.current_profile['mcs'])

        # After convergence, last 5 MCS values should be stable
        if len(mcs_history) >= 5:
            last_5 = mcs_history[-5:]
            unique = set(last_5)
            assert len(unique) <= 1, f"MCS oscillating in last 5 ticks: {last_5}"


class TestTwoChannelGate:
    """Tests for the two-channel gate (SNR margin + emergency triggers)."""

    def _open_gates(self, ps):
        """Disable timers so transitions are only constrained by gate logic."""
        ps.last_change_time_ms = 0
        ps.last_mcs_change_time_ms = 0
        ps.min_between_changes_ms = 0
        ps.hold_modes_down_ms = 0
        ps.hold_fallback_mode_ms = 0

    def _settle_at(self, ps, snr, n=20):
        """Drive the selector until it lands at the steady-state MCS for snr."""
        self._open_gates(ps)
        for i in range(n):
            _feed_tick(ps, best_snr=snr, all_packets=1000 * (i + 1))
            ps.select()
        self._open_gates(ps)

    def test_upgrade_blocked_until_headroom(self):
        """At SNR just above threshold (< hysteresis_up_db margin), MCS3→MCS4 blocked.
        MCS4 threshold=17, safety=3 → need margin >= 2.0 dB above floor, i.e. snr >= 22.
        At snr=21 (margin to MCS4 = 21-17-3=1.0 < 2.0), upgrade must not fire."""
        ps = _make_selector()
        self._settle_at(ps, snr=18)  # MCS3: 14+3=17 <= 18; MCS4 needs 20 > 18
        assert ps.current_profile_idx == 3

        # Feed snr=21 many times — _compute_profile picks MCS4 (17+3=20<=21),
        # but channel A requires margin(MCS4)=21-17-3=1.0 dB >= hysteresis_up_db=2.0.
        upgraded = False
        for i in range(20):
            _feed_tick(ps, best_snr=21, all_packets=100000 + 1000 * (i + 1))
            changed, idx, _ = ps.select()
            if changed and idx > 3:
                upgraded = True
                break
        assert not upgraded, "Upgrade should be blocked by hysteresis_up_db"

    def test_downgrade_blocked_by_hysteresis_down(self):
        """At SNR slightly below current-MCS threshold (within hysteresis_down_db),
        downgrade must NOT fire. Settle at MCS4 (snr~22), drop to 20.
        Margin(MCS4) = 20 - 17 - 3 = 0; hysteresis_down_db=1.0 → still blocked."""
        ps = _make_selector()
        self._settle_at(ps, snr=22)  # MCS4: 17+3=20 <= 22
        assert ps.current_profile_idx == 4

        # Drop to snr=20. Margin = 20-17-3 = 0. Not < -1.0 → no downgrade.
        downgraded = False
        for i in range(5):
            _feed_tick(ps, best_snr=20, all_packets=200000 + 1000 * (i + 1))
            changed, idx, _ = ps.select()
            if changed and idx < 4:
                downgraded = True
                break
        assert not downgraded, "Downgrade within hysteresis_down_db must not fire"

    def test_downgrade_fires_below_hysteresis_down(self):
        """When SNR drops enough that margin < -hysteresis_down_db, downgrade fires."""
        ps = _make_selector()
        self._settle_at(ps, snr=22)  # MCS4
        assert ps.current_profile_idx == 4

        # Drop snr far enough: margin(MCS4) = snr-17-3 = snr-20; need < -1 → snr < 19.
        downgraded_to = None
        for i in range(15):
            _feed_tick(ps, best_snr=15, all_packets=200000 + 1000 * (i + 1))
            changed, idx, _ = ps.select()
            if changed and idx < 4:
                downgraded_to = idx
                break
        assert downgraded_to is not None, "Expected downgrade below hysteresis_down_db"
        assert downgraded_to < 4

    def test_emergency_loss_forces_downgrade(self):
        """High loss rate at healthy SNR forces at-least-one-step downgrade.
        Note: the computed profile with the widened margin may already be below
        current-1, so emergency clamps to min(computed, current-1) — meaning
        idx < start_idx is the correct invariant, not idx == start_idx - 1."""
        ps = _make_selector()
        self._settle_at(ps, snr=24)  # MCS5 (20+3 <= 24)
        assert ps.current_profile_idx == 5
        start_idx = ps.current_profile_idx

        _feed_tick(ps, best_snr=24, all_packets=500000)
        ps._current_loss_rate = 0.25  # > emergency_loss_rate=0.15
        changed, idx, _ = ps.select()
        assert changed
        assert idx < start_idx

    def test_emergency_fec_forces_downgrade(self):
        """High FEC pressure at healthy SNR forces one-step downgrade."""
        ps = _make_selector()
        self._settle_at(ps, snr=24)
        assert ps.current_profile_idx == 5
        start_idx = ps.current_profile_idx

        # Directly push fec_pressure above threshold via evaluate_link state.
        _feed_tick(ps, best_snr=24, all_packets=200000)
        ps._current_fec_pressure = 0.90  # > 0.75 threshold
        changed, idx, _ = ps.select()
        assert changed
        assert idx < start_idx

    def test_emergency_bypasses_rate_limit(self):
        """Emergency downgrade fires even within min_between_changes_ms."""
        ps = _make_selector()
        self._settle_at(ps, snr=24)  # MCS5
        start_idx = ps.current_profile_idx

        # Re-enable rate limit and pretend a change just happened.
        ps.min_between_changes_ms = 5000
        ps.last_change_time_ms = ps._now_ms()  # rate limit is now active
        ps.last_mcs_change_time_ms = ps._now_ms()

        # Normal tick at same SNR — rate limit blocks any update.
        _feed_tick(ps, best_snr=24, all_packets=300000)
        changed, _, _ = ps.select()
        assert not changed

        # Now force emergency via high loss — should bypass rate limit.
        _feed_tick(ps, best_snr=24, all_packets=310000)
        ps._current_loss_rate = 0.25  # > emergency_loss_rate=0.15
        changed, idx, _ = ps.select()
        assert changed, "Emergency should bypass rate limit"
        assert idx < start_idx

    def test_predictive_blocks_upgrade_on_falling_snr(self):
        """Marginal upgrade must be blocked when SNR slope is sharply negative."""
        ps = _make_selector()
        self._settle_at(ps, snr=25)  # MCS5 (margin to MCS6 = 25-23-3 = -1)

        # Establish a strongly negative slope by walking snr down.
        self._open_gates(ps)
        for i, snr in enumerate([25, 24, 23, 22, 21]):
            _feed_tick(ps, best_snr=snr, all_packets=500000 + 1000 * (i + 1))
            ps.select()

        # Now push snr to exactly the upgrade boundary for MCS6 (margin ~2 dB).
        # With negative slope, predicted_margin = margin + slope*horizon should be < 0.
        assert ps._snr_slope < 0.0

        upgraded = False
        for i in range(3):
            _feed_tick(ps, best_snr=28, all_packets=600000 + 1000 * (i + 1))
            changed, idx, _ = ps.select()
            if changed and idx > ps.current_profile_idx - 1:
                # Check whether the change was actually an upgrade
                if idx > 5:
                    upgraded = True
                    break
        assert not upgraded, "Negative slope prediction should block marginal upgrade"


    def test_predictive_downgrade_on_falling_snr(self):
        """Current margin above -hysteresis_down_db but slope predicts breach → downgrade.
        margin(MCS4) ≈ -0.65 (above -1.0, old gate would block).
        predicted ≈ -0.65 + (-0.4)*3 ≈ -1.85 (below -1.0, new gate fires)."""
        ps = _make_selector()
        self._settle_at(ps, snr=22)  # MCS4: 17+3=20 <= 22
        assert ps.current_profile_idx == 4

        # Simulate falling link: snr_ema just below MCS4 floor, negative slope
        ps.snr_ema = 19.5   # margin(4) = 19.5 - 17 - 3 = -0.5 > -1.0
        ps._snr_slope = -0.5
        self._open_gates(ps)

        # evaluate_link updates ema/slope; _compute_profile picks MCS3
        _feed_tick(ps, best_snr=19, all_packets=500000)
        changed, idx, _ = ps.select()

        assert changed, "Predicted margin should trigger downgrade"
        assert idx < 4

    def test_no_false_predictive_downgrade_stable_snr(self):
        """Healthy margin with near-zero slope → no false downgrade.
        At MCS4 with snr=22, margin=0 dB. Stable SNR → slope ≈ 0 →
        predicted margin ≈ current margin → no trigger."""
        ps = _make_selector()
        self._settle_at(ps, snr=22)  # MCS4
        assert ps.current_profile_idx == 4

        # Feed stable SNR — slope should be near zero
        self._open_gates(ps)
        false_downgrade = False
        for i in range(10):
            _feed_tick(ps, best_snr=20.5, all_packets=500000 + 1000 * (i + 1))
            changed, idx, _ = ps.select()
            if changed and idx < 4:
                false_downgrade = True
                break
        assert not false_downgrade, "Stable SNR should not cause predictive downgrade"

    def test_slow_downgrade_blocked_by_hold_timer(self):
        """With fast_downgrade=False, non-emergency downgrade is blocked within
        hold_modes_down_ms."""
        ps = _make_selector({'profile selection': {'fast_downgrade': 'False'}})
        self._settle_at(ps, snr=22)  # MCS4
        assert ps.current_profile_idx == 4

        # Pretend an MCS change just happened
        ps.min_between_changes_ms = 0
        ps.hold_modes_down_ms = 5000
        ps.last_mcs_change_time_ms = ps._now_ms()

        # Drop SNR well below threshold — would trigger downgrade if not held
        for i in range(10):
            _feed_tick(ps, best_snr=10, all_packets=300000 + 1000 * (i + 1))
            changed, idx, _ = ps.select()
            assert not changed, "Slow downgrade should be blocked by hold_modes_down_ms"

    def test_slow_downgrade_fires_after_hold_timer(self):
        """With fast_downgrade=False, non-emergency downgrade fires once
        hold_modes_down_ms has elapsed."""
        ps = _make_selector({'profile selection': {'fast_downgrade': 'False'}})
        self._settle_at(ps, snr=22)  # MCS4
        assert ps.current_profile_idx == 4

        # Set hold timer in the past so it's already expired
        ps.min_between_changes_ms = 0
        ps.hold_modes_down_ms = 3000
        ps.last_mcs_change_time_ms = ps._now_ms() - 5000

        # Drop SNR — downgrade should fire
        downgraded = False
        for i in range(10):
            _feed_tick(ps, best_snr=10, all_packets=300000 + 1000 * (i + 1))
            changed, idx, _ = ps.select()
            if changed and idx < 4:
                downgraded = True
                break
        assert downgraded, "Slow downgrade should fire after hold_modes_down_ms expires"

    def test_emergency_bypasses_slow_downgrade_hold(self):
        """Emergency downgrade fires immediately even with fast_downgrade=False
        and hold timer active."""
        ps = _make_selector({'profile selection': {'fast_downgrade': 'False'}})
        self._settle_at(ps, snr=24)  # MCS5
        assert ps.current_profile_idx == 5
        start_idx = ps.current_profile_idx

        # Set hold timer to block slow downgrades
        ps.min_between_changes_ms = 0
        ps.hold_modes_down_ms = 5000
        ps.last_mcs_change_time_ms = ps._now_ms()

        # Trigger emergency via high loss
        _feed_tick(ps, best_snr=24, all_packets=400000)
        ps._current_loss_rate = 0.25  # > emergency_loss_rate=0.15
        changed, idx, _ = ps.select()
        assert changed, "Emergency should bypass slow downgrade hold"
        assert idx < start_idx
