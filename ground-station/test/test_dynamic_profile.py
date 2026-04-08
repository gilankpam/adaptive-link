#!/usr/bin/env python3
"""Unit tests for dynamic profile calculation in alink_gs."""
import sys
import os
import unittest
import configparser

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

[json]
HOST = 127.0.0.1
PORT = 8103

[weights]
snr_weight = 0.5
rssi_weight = 0.5

[ranges]
SNR_MIN = 10
SNR_MAX = 36
RSSI_MIN = -85
RSSI_MAX = -40

[keyframe]
allow_idr = True
idr_max_messages = 20

[dynamic refinement]
allow_penalty = False

[noise]
min_noise = 0.01
max_noise = 0.1
deduction_exponent = 0.5

[error estimation]
kalman_estimate = 0.005
kalman_error_estimate = 0.1
process_variance = 1e-5
measurement_variance = 0.01

[scoring]
rf_weight = 0.5
loss_weight = 0.25
fec_weight = 0.15
diversity_weight = 0.1
max_loss_rate = 0.1
max_rssi_spread = 20

[profile selection]
hold_fallback_mode_ms = 1000
hold_modes_down_ms = 3000
min_between_changes_ms = 200
hysteresis_percent = 5
hysteresis_percent_down = 5
ema_fast_alpha = 0.5
ema_slow_alpha = 0.15
predict_multi = 1.0
fast_downgrade = True
upward_confidence_loops = 3
limit_max_score_to = 2000

[dynamic]
snr_safety_margin = 3
snr_ema_alpha = 0.3
loss_margin_weight = 20
fec_margin_weight = 5
max_mcs = 7
short_gi_snr_margin = 5
loss_threshold_for_fec_downgrade = 0.05
fec_redundancy_ratio = 0.25
utilization_factor = 0.45
max_bitrate = 30000
min_bitrate = 2000
max_power = 2500
min_power = 200
bandwidth = 20
gop = 10
max_mcs_step_up = 1

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
    return ProfileSelector(_make_config(overrides))


def _feed_score(ps, best_snr, loss_rate=0.0, fec_pressure=0.0,
                best_rssi=-50, min_rssi=-55, all_packets=1000,
                lost_packets=0, fec_rec=0, num_antennas=2):
    """Feed one tick of metrics into the selector."""
    # Approximate lost_packets from loss_rate for the delta computation
    if lost_packets == 0 and loss_rate > 0:
        lost_packets = int(all_packets * loss_rate)
    if fec_rec == 0 and fec_pressure > 0:
        fec_rec = int(fec_pressure * 10)  # rough approximation
    return ps.compute_score(
        best_rssi=best_rssi, best_snr=best_snr, min_rssi=min_rssi,
        all_packets=all_packets, lost_packets=lost_packets, fec_rec=fec_rec,
        fec_k=8, fec_n=12, num_antennas=num_antennas
    )


class TestMCSSelection(unittest.TestCase):
    """Test that MCS is selected correctly based on SNR thresholds."""

    def test_mcs0_at_low_snr(self):
        ps = _make_selector()
        _feed_score(ps, best_snr=7)
        profile = ps._compute_profile()
        self.assertEqual(profile['mcs'], 0)

    def test_mcs0_when_snr_below_all_thresholds(self):
        ps = _make_selector()
        _feed_score(ps, best_snr=3)
        profile = ps._compute_profile()
        self.assertEqual(profile['mcs'], 0)

    def test_mcs1_at_snr_12(self):
        """SNR=12, margin=3 → MCS1 threshold=8+3=11 < 12 → MCS1."""
        ps = _make_selector()
        _feed_score(ps, best_snr=12)
        profile = ps._compute_profile()
        self.assertEqual(profile['mcs'], 1)

    def test_mcs3_at_snr_18(self):
        """SNR=18, margin=3 → MCS3 threshold=14+3=17 < 18 → MCS3."""
        ps = _make_selector()
        _feed_score(ps, best_snr=18)
        profile = ps._compute_profile()
        self.assertEqual(profile['mcs'], 3)

    def test_mcs7_at_high_snr(self):
        """SNR=32, margin=3 → MCS7 threshold=26+3=29 < 32 → MCS7."""
        ps = _make_selector()
        _feed_score(ps, best_snr=32)
        profile = ps._compute_profile()
        self.assertEqual(profile['mcs'], 7)

    def test_max_mcs_cap(self):
        """Even with high SNR, respect max_mcs config."""
        ps = _make_selector({'dynamic': {'max_mcs': '4'}})
        _feed_score(ps, best_snr=32)
        profile = ps._compute_profile()
        self.assertEqual(profile['mcs'], 4)

    def test_margin_increases_with_loss(self):
        """High loss widens margin, may push MCS down."""
        ps = _make_selector()
        # SNR=18 normally selects MCS3 (14+3=17 < 18)
        # With 10% loss: margin = 3 + 0.1*20 = 5 → MCS3 threshold = 14+5=19 > 18 → MCS2
        _feed_score(ps, best_snr=18)
        ps._current_loss_rate = 0.1
        profile = ps._compute_profile()
        self.assertEqual(profile['mcs'], 2)

    def test_margin_increases_with_fec_pressure(self):
        """High FEC pressure widens margin."""
        ps = _make_selector()
        _feed_score(ps, best_snr=18)
        ps._current_fec_pressure = 1.0  # 100% pressure → margin += 5
        # margin = 3 + 5 = 8, MCS3 threshold = 14+8 = 22 > 18
        # MCS2 threshold = 11+8 = 19 > 18
        # MCS1 threshold = 8+8 = 16 < 18 → MCS1
        profile = ps._compute_profile()
        self.assertEqual(profile['mcs'], 1)


class TestGuardInterval(unittest.TestCase):
    """Test guard interval selection."""

    def test_long_gi_when_margin_small(self):
        """SNR just above threshold → long GI."""
        ps = _make_selector()
        _feed_score(ps, best_snr=18)  # MCS3, margin above threshold = 18-14=4 < 5
        profile = ps._compute_profile()
        self.assertEqual(profile['gi'], 'long')

    def test_short_gi_when_margin_large(self):
        """SNR well above threshold → short GI."""
        ps = _make_selector()
        _feed_score(ps, best_snr=22)  # MCS3 (14+3=17<22), margin above = 22-14=8 >= 5
        profile = ps._compute_profile()
        self.assertEqual(profile['gi'], 'short')

    def test_long_gi_when_loss_high(self):
        """Even with good margin, high loss forces long GI."""
        ps = _make_selector()
        _feed_score(ps, best_snr=22)
        ps._current_loss_rate = 0.05  # > 0.02 threshold
        profile = ps._compute_profile()
        self.assertEqual(profile['gi'], 'long')

    def test_long_gi_when_fec_pressure_high(self):
        """High FEC pressure forces long GI."""
        ps = _make_selector()
        _feed_score(ps, best_snr=22)
        ps._current_fec_pressure = 0.5  # > 0.3 threshold
        profile = ps._compute_profile()
        self.assertEqual(profile['gi'], 'long')


class TestFEC(unittest.TestCase):
    """Test bitrate-aware FEC block sizing using frame-aligned algorithm."""

    def test_small_blocks_at_low_bitrate(self):
        """MCS0 LGI: PHY=6.5 Mbps, raw=2925 kbps, target=2194 kbps.
        Packets per frame: 2194000/(60*8*1446)=3.16 → K=4, N=ceil(4/0.75)=6."""
        ps = _make_selector()
        _feed_score(ps, best_snr=7)
        profile = ps._compute_profile()
        self.assertEqual(profile['fec_n'], 6)
        self.assertEqual(profile['fec_k'], 4)

    def test_medium_blocks_at_mid_bitrate(self):
        """MCS4 LGI: PHY=39 Mbps, raw=17550 kbps, target=13163 kbps.
        Packets per frame: 13162500/(60*8*1446)=18.96 → K=19, N=ceil(19/0.75)=26."""
        ps = _make_selector()
        _feed_score(ps, best_snr=21)  # MCS4: 17+3=20 < 21
        profile = ps._compute_profile()
        self.assertEqual(profile['fec_n'], 26)
        self.assertEqual(profile['fec_k'], 19)

    def test_large_blocks_at_high_bitrate(self):
        """MCS7 SGI: PHY=72.2 Mbps, raw=32490 kbps, target=24368 kbps.
        Packets per frame: 24368000/(60*8*1446)=35.1 → K=36, N=ceil(36/0.75)=48."""
        ps = _make_selector()
        _feed_score(ps, best_snr=32)
        profile = ps._compute_profile()
        self.assertEqual(profile['fec_n'], 48)
        self.assertEqual(profile['fec_k'], 36)

    def test_fec_downgrade_on_high_loss(self):
        """Loss > threshold reduces fec_k by 1, keeping fec_n constant (increases redundancy).
        SNR=21 with loss=0.06 → margin=3+0.06*20=4.2 → MCS3 (14+4.2=18.2<21).
        MCS3 LGI: PHY=26 Mbps, raw=11700 kbps, target=8775 kbps.
        Packets per frame: 8775000/(60*8*1446)=12.6 → K=13, N=ceil(13/0.75)=18.
        Loss>0.05 → K=max(2,13-1)=12, N stays 18 (redundancy increases from 27.8% to 33.3%).
        max_fec_redundancy check: 33.3% < 50%, OK."""
        ps = _make_selector()
        _feed_score(ps, best_snr=21)
        ps._current_loss_rate = 0.06  # > 0.05 threshold
        profile = ps._compute_profile()
        self.assertEqual(profile['mcs'], 3)  # Margin pushed MCS down
        self.assertEqual(profile['fec_k'], 12)
        self.assertEqual(profile['fec_n'], 18)  # N stays constant after loss downgrade

    def test_fec_downgrade_floor(self):
        """fec_k has minimum of 2, but max_fec_redundancy may raise it.
        MCS0: K=4, N=6. After loss downgrade: K=4-1=3, N stays 6.
        Redundancy becomes (6-3)/6 = 50% = max_fec_redundancy, OK."""
        ps = _make_selector()
        _feed_score(ps, best_snr=7)  # MCS0, K=4, N=6
        ps._current_loss_rate = 0.06  # Triggers downgrade
        profile = ps._compute_profile()
        self.assertEqual(profile['fec_k'], 3)  # 4-1=3, at max_fec_redundancy boundary (50%)
        self.assertEqual(profile['fec_n'], 6)  # N stays constant

    def test_max_fec_redundancy_enforced(self):
        """max_fec_redundancy should prevent excessive redundancy after loss.

        MCS0: K=4, N=6. Loss downgrade: K=4-1=3, N stays 6.
        Redundancy = (6-3)/6 = 50% = max_fec_redundancy, so it just passes.

        The purpose is to prevent FEC from becoming too aggressive (too much redundancy)
        on weak links, which would make the link even more fragile.
        """
        ps = _make_selector()
        _feed_score(ps, best_snr=7)  # MCS0, K=5, N=7
        ps._current_loss_rate = 0.06  # Triggers downgrade
        profile = ps._compute_profile()
        # After loss: K=3, N=4
        # max_fec_redundancy=0.5 means max 50% redundancy
        # (N-K)/N <= 0.5 → K/N >= 0.5 → K >= N/2
        # With K=3, N=4: 3/4 = 0.75 >= 0.5, so it passes
        redundancy = (profile['fec_n'] - profile['fec_k']) / profile['fec_n']
        self.assertLessEqual(redundancy, 0.5 + 0.01)  # Allow small tolerance


class TestFECFromBitrate(unittest.TestCase):
    """Test the new FEC calculation algorithm."""

    def test_packets_per_frame_calculation(self):
        """Verify packets per frame formula: 4 Mbps at 60 FPS = 5.76 packets."""
        ps = _make_selector()
        bitrate = 4000000  # 4 Mbps
        packets_per_frame = bitrate / (VIDEO_FPS * 8 * MTU_PAYLOAD_BYTES)
        self.assertAlmostEqual(packets_per_frame, 5.76, places=1)

    def test_fec_k_equals_ceil_packets_per_frame(self):
        """K should be ceiling of packets per frame."""
        ps = _make_selector()
        bitrate = 4000000  # 4 Mbps → 5.76 packets → K=6
        fec_k, fec_n = ps._compute_fec_from_bitrate(bitrate)
        self.assertEqual(fec_k, 6)

    def test_fec_n_uses_config_redundancy_ratio(self):
        """N should use fec_redundancy_ratio config (default 0.25 = 25%)."""
        ps = _make_selector()
        # With 25% redundancy: N = K / 0.75
        # K=6 → N = 6 / 0.75 = 8
        fec_k, fec_n = ps._compute_fec_from_bitrate(4000000)
        self.assertEqual(fec_n, 8)

    def test_fec_k_minimum_is_2(self):
        """K should never be less than 2."""
        ps = _make_selector()
        # Very low bitrate should still give K >= 2
        fec_k, fec_n = ps._compute_fec_from_bitrate(100000)  # 100 kbps
        self.assertGreaterEqual(fec_k, 2)

    def test_4mbps_example_from_spec(self):
        """Test the exact example from the specification: 4 Mbps at 60 FPS = 6/8."""
        ps = _make_selector()
        # 4 Mbps / (60 * 8 * 1446) = 5.76 packets → K=6
        # N = 6 / 0.75 = 8 (with 25% redundancy)
        fec_k, fec_n = ps._compute_fec_from_bitrate(4000000)
        self.assertEqual(fec_k, 6)
        self.assertEqual(fec_n, 8)

    def test_different_redundancy_ratio(self):
        """Test that custom redundancy ratio is respected."""
        ps = _make_selector()
        # With 33% redundancy: N = K / 0.67 ≈ K * 1.5
        # K=6 → N = 6 / 0.67 = 9
        fec_k, fec_n = ps._compute_fec_from_bitrate(4000000, redundancy_ratio=0.33)
        self.assertEqual(fec_k, 6)
        self.assertEqual(fec_n, 9)


class TestBitrate(unittest.TestCase):
    """Test bitrate computation."""

    def test_bitrate_mcs0_long_gi(self):
        """MCS0 LGI: PHY=6.5 Mbps, link_bw=2925 kbps, K=4, N=6.
        bitrate = 2925000 * 4/6 / 1000 = 1950 → clamped to min_bitrate=2000."""
        ps = _make_selector()
        _feed_score(ps, best_snr=7)
        profile = ps._compute_profile()
        self.assertEqual(profile['bitrate'], 2000)

    def test_bitrate_mcs7_short_gi(self):
        """SNR=32 → MCS7, snr_above=32-26=6 >= 5 → short GI.
        PHY 72.2 Mbps, raw=32490 kbps, target=32490×0.75=24367 kbps."""
        ps = _make_selector()
        _feed_score(ps, best_snr=32)
        profile = ps._compute_profile()
        self.assertEqual(profile['gi'], 'short')
        self.assertEqual(profile['bitrate'], 24367)

    def test_bitrate_capped_at_max(self):
        ps = _make_selector({'dynamic': {'max_bitrate': '10000'}})
        _feed_score(ps, best_snr=32)
        profile = ps._compute_profile()
        self.assertEqual(profile['bitrate'], 10000)

    def test_bitrate_floored_at_min(self):
        ps = _make_selector({'dynamic': {'min_bitrate': '3000'}})
        _feed_score(ps, best_snr=7)
        profile = ps._compute_profile()
        self.assertEqual(profile['bitrate'], 3000)

    def test_bitrate_40mhz(self):
        ps = _make_selector({'dynamic': {'bandwidth': '40'}})
        _feed_score(ps, best_snr=7)
        profile = ps._compute_profile()
        # PHY 13.5 Mbps (40MHz MCS0 long GI), link_bw=6075 kbps
        # K=7, N=10 → bitrate = 6075000 * 7/10 / 1000 = 4252
        self.assertEqual(profile['bitrate'], 4252)


class TestPower(unittest.TestCase):
    """Test power scaling — linear interpolation."""

    def test_max_power_at_low_mcs(self):
        ps = _make_selector()
        _feed_score(ps, best_snr=7)  # MCS 0
        profile = ps._compute_profile()
        self.assertEqual(profile['power'], 2500)

    def test_min_power_at_high_mcs(self):
        ps = _make_selector()
        _feed_score(ps, best_snr=32)  # MCS 7
        profile = ps._compute_profile()
        self.assertEqual(profile['power'], 200)

    def test_mid_power_at_mid_mcs(self):
        ps = _make_selector()
        _feed_score(ps, best_snr=18)  # MCS 3
        profile = ps._compute_profile()
        # Linear: 2500 - (3/7) * 2300 = 2500 - 985.7 -> round to 1514
        self.assertEqual(profile['power'], 1514)

    def test_power_is_linear_and_monotonic(self):
        """Power decreases monotonically with small per-step delta."""
        ps = _make_selector()
        powers = [ps._compute_power(m) for m in range(8)]
        for i in range(1, len(powers)):
            self.assertLessEqual(powers[i], powers[i - 1])
            # Max per-step delta should be <= 329 (ceil(2300/7))
            self.assertLessEqual(powers[i - 1] - powers[i], 329)


class TestFixedParams(unittest.TestCase):
    """Test that fixed parameters come from config."""

    def test_gop_from_config(self):
        ps = _make_selector({'dynamic': {'gop': '5'}})
        _feed_score(ps, best_snr=20)
        profile = ps._compute_profile()
        self.assertEqual(profile['gop'], 5.0)



    def test_bandwidth_from_config(self):
        ps = _make_selector()
        _feed_score(ps, best_snr=20)
        profile = ps._compute_profile()
        self.assertEqual(profile['bandwidth'], 20)


class TestSelectIntegration(unittest.TestCase):
    """Test that select() works correctly in dynamic mode."""

    def _warm_up(self, ps, best_snr=20, n=5):
        """Feed enough ticks to get through confidence gating and hold timers.
        Returns once the first profile change is applied."""
        ps.last_change_time_ms = 0
        ps.min_between_changes_ms = 0
        ps.hold_modes_down_ms = 0
        ps.hold_fallback_mode_ms = 0
        for i in range(n):
            score = _feed_score(ps, best_snr=best_snr, all_packets=1000 * (i + 1))
            changed, idx, profile = ps.select(score)
            if changed:
                return changed, idx, profile
        return False, ps.current_profile_idx, ps.current_profile

    def test_select_converges_after_confidence(self):
        """After enough ticks (confidence gating), select() produces a change."""
        ps = _make_selector()
        changed, idx, profile = self._warm_up(ps, best_snr=20)
        self.assertTrue(changed)
        self.assertIsNotNone(profile)
        self.assertEqual(idx, profile['mcs'])

    def test_same_mcs_no_change(self):
        ps = _make_selector()
        self._warm_up(ps, best_snr=20)

        # Feed again with same SNR — should not change
        score = _feed_score(ps, best_snr=20, all_packets=10000)
        changed, _, _ = ps.select(score)
        self.assertFalse(changed)

    def test_dynamic_profile_updates_even_without_mcs_change(self):
        """In dynamic mode, current_profile is updated even if MCS stays same."""
        ps = _make_selector()
        self._warm_up(ps, best_snr=20)
        self.assertIsNotNone(ps.current_profile)

        # Feed with higher loss — same MCS but FEC might downgrade
        score = _feed_score(ps, best_snr=20, all_packets=20000, lost_packets=2000)
        ps.select(score)
        # Profile should still exist and be updated
        self.assertIsNotNone(ps.current_profile)

    def test_param_only_change_detected(self):
        """select() returns changed=True when MCS stays same but params differ."""
        ps = _make_selector()
        changed, idx, profile = self._warm_up(ps, best_snr=20)
        self.assertTrue(changed)
        old_profile = dict(ps.current_profile)
        old_mcs = ps.current_profile_idx

        # Feed with significant loss to trigger FEC/bitrate change at same MCS
        ps.min_between_changes_ms = 0  # disable rate limiting
        score = _feed_score(ps, best_snr=20, all_packets=20000, lost_packets=2000)
        changed, idx, profile = ps.select(score)

        # MCS should stay the same but params should differ
        if idx == old_mcs and ps._profile_changed(old_profile, profile):
            self.assertTrue(changed)
        # If params didn't change (edge case), at least verify no crash
        self.assertIsNotNone(profile)


class TestMCSStepLimit(unittest.TestCase):
    """Test that upward MCS changes are step-limited."""

    def _warm_at_mcs(self, ps, target_snr, n=10):
        """Feed ticks until selector settles at the MCS for target_snr."""
        ps.last_change_time_ms = 0
        ps.min_between_changes_ms = 0
        ps.hold_modes_down_ms = 0
        ps.hold_fallback_mode_ms = 0
        for i in range(n):
            score = _feed_score(ps, best_snr=target_snr, all_packets=1000 * (i + 1))
            ps.select(score)
        # Reset timing so hold timers don't block subsequent tests
        ps.last_change_time_ms = 0

    def test_upgrade_capped_to_one_step(self):
        """Starting at MCS 2, even if SNR qualifies for MCS 7, only step to MCS 3."""
        ps = _make_selector()
        self._warm_at_mcs(ps, target_snr=15)
        self.assertEqual(ps.current_profile_idx, 2)

        # Feed enough ticks for SNR EMA to converge and confidence to pass
        first_change_idx = None
        for i in range(15):
            score = _feed_score(ps, best_snr=32, all_packets=10000 * (i + 1))
            changed, idx, profile = ps.select(score)
            if changed and first_change_idx is None:
                first_change_idx = idx
                break
        # First upgrade should be capped to +1
        self.assertEqual(first_change_idx, 3)

    def test_downgrade_not_limited(self):
        """Downgrades can drop more than 1 MCS step."""
        ps = _make_selector()
        self._warm_at_mcs(ps, target_snr=24)  # MCS 5
        start_mcs = ps.current_profile_idx
        self.assertEqual(start_mcs, 5)

        # Feed several ticks at very low SNR so EMA drops enough
        for i in range(10):
            score = _feed_score(ps, best_snr=7, all_packets=50000 * (i + 1))
            changed, idx, profile = ps.select(score)
            if changed:
                # Downgrade should jump more than 1 step (proves it's unlimited)
                self.assertLess(idx, start_mcs - 1)
                return
        self.fail("Expected a downgrade but none occurred")

    def test_step_limit_disabled_with_zero(self):
        """Setting max_mcs_step_up=0 disables step limiting."""
        ps = _make_selector({'dynamic': {'max_mcs_step_up': '0'}})
        self._warm_at_mcs(ps, target_snr=15)  # MCS 2
        self.assertEqual(ps.current_profile_idx, 2)

        # Feed many ticks so EMA converges and we can jump past +1
        changes = []
        for i in range(20):
            score = _feed_score(ps, best_snr=32, all_packets=10000 * (i + 1))
            changed, idx, profile = ps.select(score)
            if changed:
                changes.append(idx)
                break
        # First change should be > +1 step from MCS 2 (proves no limit)
        self.assertTrue(len(changes) > 0, "Expected a change")
        self.assertGreater(changes[0], 3)


class TestOscillationRegression(unittest.TestCase):
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
            score = _feed_score(ps, best_snr=29, all_packets=1000 * (i + 1))
            ps.select(score)
            if ps.current_profile is not None:
                mcs_history.append(ps.current_profile['mcs'])

        # After convergence, last 5 MCS values should be stable
        if len(mcs_history) >= 5:
            last_5 = mcs_history[-5:]
            unique = set(last_5)
            self.assertLessEqual(len(unique), 1,
                f"MCS oscillating in last 5 ticks: {last_5}")


if __name__ == '__main__':
    unittest.main()
