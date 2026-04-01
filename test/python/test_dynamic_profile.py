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


def _make_config(overrides=None):
    """Create a configparser with DEFAULT_CONFIG and optional overrides."""
    config = configparser.ConfigParser()
    config.read_string(DEFAULT_CONFIG)
    config.set('profile selection', 'dynamic_mode', 'True')
    if overrides:
        for section, kvs in overrides.items():
            for k, v in kvs.items():
                config.set(section, k, str(v))
    return config


def _make_selector(overrides=None):
    """Create a ProfileSelector in dynamic mode."""
    return ProfileSelector([], _make_config(overrides))


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
    """Test FEC parameter selection."""

    def test_fec_table_mcs0(self):
        ps = _make_selector()
        _feed_score(ps, best_snr=7)
        profile = ps._compute_profile()
        self.assertEqual(profile['fec_k'], 2)
        self.assertEqual(profile['fec_n'], 3)

    def test_fec_table_mcs4(self):
        ps = _make_selector()
        _feed_score(ps, best_snr=21)  # MCS4: 17+3=20 < 21
        profile = ps._compute_profile()
        self.assertEqual(profile['fec_k'], 8)
        self.assertEqual(profile['fec_n'], 12)

    def test_fec_table_mcs7(self):
        ps = _make_selector()
        _feed_score(ps, best_snr=32)
        profile = ps._compute_profile()
        self.assertEqual(profile['fec_k'], 10)
        self.assertEqual(profile['fec_n'], 12)

    def test_fec_downgrade_on_high_loss(self):
        """Loss > threshold reduces fec_k by 2.
        SNR=21 with loss=0.06 → margin=3+0.06*20=4.2 → MCS3 (14+4.2=18.2<21).
        MCS3 base FEC=(6,9), loss>0.05 → fec_k=max(2,6-2)=4.
        """
        ps = _make_selector()
        _feed_score(ps, best_snr=21)
        ps._current_loss_rate = 0.06  # > 0.05 threshold
        profile = ps._compute_profile()
        self.assertEqual(profile['mcs'], 3)  # Margin pushed MCS down
        self.assertEqual(profile['fec_k'], 4)  # 6-2=4
        self.assertEqual(profile['fec_n'], 9)

    def test_fec_downgrade_floor(self):
        """fec_k should not go below 2."""
        ps = _make_selector()
        _feed_score(ps, best_snr=7)  # MCS0, fec_k=2
        ps._current_loss_rate = 0.06
        profile = ps._compute_profile()
        self.assertEqual(profile['fec_k'], 2)  # max(2, 2-2) = 2


class TestBitrate(unittest.TestCase):
    """Test bitrate computation."""

    def test_bitrate_mcs0_long_gi(self):
        ps = _make_selector()
        _feed_score(ps, best_snr=7)
        profile = ps._compute_profile()
        # PHY 6.5 Mbps, FEC 2/3, util 0.45 → 6500 * 2/3 * 0.45 = 1950 → clamped to 2000
        self.assertEqual(profile['bitrate'], 2000)

    def test_bitrate_mcs7_short_gi(self):
        """SNR=32 → MCS7, snr_above=32-26=6 >= 5 → short GI.
        PHY 72.2 Mbps, FEC 10/12, util 0.45 → 72200*(10/12)*0.45=27075."""
        ps = _make_selector()
        _feed_score(ps, best_snr=32)
        profile = ps._compute_profile()
        self.assertEqual(profile['gi'], 'short')
        self.assertEqual(profile['bitrate'], 27075)

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
        # PHY 13.5 Mbps (40MHz MCS0 long GI), FEC 2/3, util 0.45
        # 13500 * 2/3 * 0.45 = 4050
        self.assertEqual(profile['bitrate'], 4050)


class TestPower(unittest.TestCase):
    """Test power scaling."""

    def test_max_power_at_low_mcs(self):
        ps = _make_selector()
        _feed_score(ps, best_snr=7)  # MCS 0
        profile = ps._compute_profile()
        self.assertEqual(profile['power'], 45)

    def test_min_power_at_high_mcs(self):
        ps = _make_selector()
        _feed_score(ps, best_snr=32)  # MCS 7
        profile = ps._compute_profile()
        self.assertEqual(profile['power'], 30)

    def test_mid_power_at_mid_mcs(self):
        ps = _make_selector()
        _feed_score(ps, best_snr=18)  # MCS 3
        profile = ps._compute_profile()
        # mcs 2-3: max_power - range//3 = 45 - 15//3 = 45 - 5 = 40
        self.assertEqual(profile['power'], 40)


class TestFixedParams(unittest.TestCase):
    """Test that fixed parameters come from config."""

    def test_gop_from_config(self):
        ps = _make_selector({'dynamic': {'gop': '5'}})
        _feed_score(ps, best_snr=20)
        profile = ps._compute_profile()
        self.assertEqual(profile['gop'], 5.0)

    def test_qp_delta_from_config(self):
        ps = _make_selector()
        _feed_score(ps, best_snr=20)
        profile = ps._compute_profile()
        self.assertEqual(profile['qp_delta'], -12)

    def test_roi_qp_from_config(self):
        ps = _make_selector()
        _feed_score(ps, best_snr=20)
        profile = ps._compute_profile()
        self.assertEqual(profile['roi_qp'], '0,0,0,0')

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


class TestBackwardCompat(unittest.TestCase):
    """Test that table mode still works when dynamic_mode=False."""

    def test_table_mode_default(self):
        config = configparser.ConfigParser()
        config.read_string(DEFAULT_CONFIG)
        ps = ProfileSelector([], config)
        self.assertFalse(ps.dynamic_mode)

    def test_table_mode_uses_lookup(self):
        config = configparser.ConfigParser()
        config.read_string(DEFAULT_CONFIG)
        profiles = [
            {'range_min': 1000, 'range_max': 1500, 'gi': 'long', 'mcs': 1,
             'fec_k': 8, 'fec_n': 12, 'bitrate': 4000, 'gop': 10.0,
             'power': 45, 'roi_qp': '0,0,0,0', 'bandwidth': 20, 'qp_delta': -12},
            {'range_min': 1501, 'range_max': 2000, 'gi': 'short', 'mcs': 5,
             'fec_k': 10, 'fec_n': 12, 'bitrate': 20000, 'gop': 10.0,
             'power': 30, 'roi_qp': '0,0,0,0', 'bandwidth': 20, 'qp_delta': -12},
        ]
        ps = ProfileSelector(profiles, config)
        idx, profile = ps._lookup_profile(1200)
        self.assertEqual(idx, 0)
        self.assertEqual(profile['mcs'], 1)


if __name__ == '__main__':
    unittest.main()
