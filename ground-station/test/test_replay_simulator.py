"""Tests for ground-station/ml/replay_simulator.py"""

import configparser
import json
import math
import os
import sys

import pandas as pd
import pytest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..'))
from ml.replay_simulator import LinkModel, ReplayResult, ReplaySimulator, MCS_SNR_THRESHOLDS, load_config_from_file

_CONFIG_PATH = os.path.join(os.path.dirname(__file__), '..', '..', 'config', 'alink_gs.conf')
_BASE_CONFIG_STR = load_config_from_file(_CONFIG_PATH)


def _make_config(overrides=None):
    """Create a configparser with DEFAULT_CONFIG and dynamic_mode=True."""
    config = configparser.ConfigParser()
    config.read_string(_BASE_CONFIG_STR)
    config.set('profile selection', 'dynamic_mode', 'True')
    if overrides:
        for section, kvs in overrides.items():
            for k, v in kvs.items():
                config.set(section, k, str(v))
    return config


def _make_tick(ts=1000, rssi=-40, snr=25.0, rssi_min=-45, ant=2,
               pkt_all=100, pkt_lost=1, pkt_fec=0, fec_k=8, fec_n=12,
               loss_rate=0.01, fec_pressure=0.1, rf_score=0.8,
               loss_score=0.9, fec_score=0.9, div_score=0.95,
               score=1850.0, ema_fast=1850.0, ema_slow=1840.0,
               snr_ema=25.0, changed=False, adapter='test-adapter',
               mcs=4, gi='short', sel_fec_k=8, sel_fec_n=12,
               bitrate=15000, power=30, **overrides):
    """Create a tick record with defaults."""
    record = {
        'ts': ts, 'rssi': rssi, 'snr': snr, 'rssi_min': rssi_min,
        'ant': ant, 'pkt_all': pkt_all, 'pkt_lost': pkt_lost,
        'pkt_fec': pkt_fec, 'fec_k': fec_k, 'fec_n': fec_n,
        'loss_rate': loss_rate, 'fec_pressure': fec_pressure,
        'rf_score': rf_score, 'loss_score': loss_score,
        'fec_score': fec_score, 'div_score': div_score,
        'score': score, 'ema_fast': ema_fast, 'ema_slow': ema_slow,
        'snr_ema': snr_ema, 'changed': changed, 'adapter': adapter,
        'mcs': mcs, 'gi': gi, 'sel_fec_k': sel_fec_k,
        'sel_fec_n': sel_fec_n, 'bitrate': bitrate, 'power': power,
    }
    record.update(overrides)
    return record


def _ticks_to_df(ticks):
    """Convert list of tick dicts to DataFrame."""
    return pd.DataFrame(ticks)


# =============================================================
# Tests for LinkModel
# =============================================================

class TestLinkModelLoss:
    """Test SNR-to-loss estimation."""

    def test_zero_loss_above_threshold(self):
        """Loss should be near-zero when SNR is well above MCS threshold."""
        model = LinkModel()
        for mcs in range(8):
            snr = MCS_SNR_THRESHOLDS[mcs] + 10
            loss = model.estimate_loss(snr, mcs)
            assert loss < 0.01, f"MCS {mcs}: loss={loss} at SNR={snr}"

    def test_high_loss_below_threshold(self):
        """Loss should be high when SNR is well below MCS threshold."""
        model = LinkModel()
        for mcs in range(8):
            snr = MCS_SNR_THRESHOLDS[mcs] - 5
            loss = model.estimate_loss(snr, mcs)
            assert loss > 0.5, f"MCS {mcs}: loss={loss} at SNR={snr}"

    def test_loss_monotonically_decreases_with_snr(self):
        """Lower SNR should never produce lower loss for same MCS."""
        model = LinkModel()
        for mcs in [0, 3, 7]:
            prev_loss = 1.0
            for snr in range(0, 40):
                loss = model.estimate_loss(snr, mcs)
                assert loss <= prev_loss + 1e-9, (
                    f"MCS {mcs}: loss increased from {prev_loss} to {loss} "
                    f"as SNR increased to {snr}")
                prev_loss = loss

    def test_higher_mcs_higher_loss_at_same_snr(self):
        """At same SNR, higher MCS should produce equal or higher loss."""
        model = LinkModel()
        for snr in [10, 15, 20, 25]:
            prev_loss = 0.0
            for mcs in range(8):
                loss = model.estimate_loss(snr, mcs)
                assert loss >= prev_loss - 1e-9, (
                    f"SNR {snr}: MCS {mcs} loss={loss} < MCS {mcs-1} loss={prev_loss}")
                prev_loss = loss

    def test_clamps_mcs_range(self):
        """MCS values outside 0-7 should be clamped."""
        model = LinkModel()
        loss_neg = model.estimate_loss(20, -1)
        loss_0 = model.estimate_loss(20, 0)
        assert abs(loss_neg - loss_0) < 1e-9

        loss_high = model.estimate_loss(20, 10)
        loss_7 = model.estimate_loss(20, 7)
        assert abs(loss_high - loss_7) < 1e-9


class TestLinkModelFEC:
    """Test FEC recovery estimation."""

    def test_recovers_within_capacity(self):
        """When losses are within FEC capacity, all should be recovered."""
        model = LinkModel()
        # fec_k=8, fec_n=12 -> redundancy=4 per block
        # With 120 packets (10 blocks) and 2% loss = ~2.4 lost per block < 4
        recovered = model.estimate_fec_recoveries(0.02, 8, 12, 120)
        assert recovered > 0
        # Can't recover more than total lost
        total_lost = int(0.02 * 120)
        assert recovered <= total_lost + 1  # +1 for rounding

    def test_fec_saturated(self):
        """When losses exceed capacity, recovery is capped."""
        model = LinkModel()
        # fec_k=8, fec_n=12 -> redundancy=4 per block
        # With 60% loss: 7.2 lost per block > 4 capacity
        recovered = model.estimate_fec_recoveries(0.6, 8, 12, 120)
        # Max recovery = 4 per block * 10 blocks = 40
        assert recovered <= 40

    def test_zero_loss_zero_recovery(self):
        """No loss means no FEC recovery needed."""
        model = LinkModel()
        recovered = model.estimate_fec_recoveries(0.0, 8, 12, 120)
        assert recovered == 0

    def test_invalid_fec_params(self):
        """Invalid FEC parameters should return 0."""
        model = LinkModel()
        assert model.estimate_fec_recoveries(0.1, 0, 0, 100) == 0
        assert model.estimate_fec_recoveries(0.1, 12, 8, 100) == 0  # k > n
        assert model.estimate_fec_recoveries(0.1, 8, 8, 100) == 0  # k == n


# =============================================================
# Tests for ReplayResult
# =============================================================

class TestReplayResult:

    def test_transition_rate_empty(self):
        r = ReplayResult()
        assert r.transition_rate == 0.0
        assert r.stability_score == 1.0

    def test_transition_rate_calculation(self):
        r = ReplayResult()
        r.total_ticks = 100  # 10 seconds at 10Hz
        r.transition_count = 5  # 5 changes in 10 seconds = 0.5/sec
        assert abs(r.transition_rate - 0.5) < 0.01
        assert abs(r.stability_score - 0.5) < 0.01

    def test_high_transition_rate_caps_stability(self):
        r = ReplayResult()
        r.total_ticks = 10  # 1 second
        r.transition_count = 10  # 10 changes/sec
        assert r.stability_score == 0.0


# =============================================================
# Tests for ReplaySimulator
# =============================================================

class TestReplaySimulatorDeterminism:
    """Replay must be deterministic: same inputs = same outputs."""

    def test_identical_runs(self):
        ticks = [_make_tick(ts=i * 100, snr=25.0) for i in range(20)]
        df = _ticks_to_df(ticks)
        config = _make_config()

        sim1 = ReplaySimulator(df.copy(), config)
        r1 = sim1.run()

        sim2 = ReplaySimulator(df.copy(), config)
        r2 = sim2.run()

        assert r1.total_fitness == r2.total_fitness
        assert r1.mean_bitrate == r2.mean_bitrate
        assert r1.mean_loss == r2.mean_loss
        assert r1.transition_count == r2.transition_count
        assert r1.crash_events == r2.crash_events

    def test_different_params_different_results(self):
        """Changing a parameter should change the result."""
        # Use enough ticks with fast timers to ensure profile selection settles
        ticks = [_make_tick(ts=i * 100, snr=25.0) for i in range(100)]
        df = _ticks_to_df(ticks)

        overrides = {
            'profile selection': {
                'hold_modes_down_ms': '100',
                'hold_fallback_mode_ms': '100',
                'min_between_changes_ms': '50',
                'upward_confidence_loops': '1',
            }
        }
        config1 = _make_config({**overrides, 'dynamic': {'snr_safety_margin': '3'}})
        config2 = _make_config({**overrides, 'dynamic': {'snr_safety_margin': '12'}})

        r1 = ReplaySimulator(df.copy(), config1).run()
        r2 = ReplaySimulator(df.copy(), config2).run()

        # With higher safety margin, MCS is lower -> different bitrate
        assert r1.mean_bitrate != r2.mean_bitrate


class TestReplaySimulatorTimeInjection:
    """Hold timers must use tick timestamps, not wall clock."""

    def test_rate_limiting_respects_tick_time(self):
        """min_between_changes_ms should be enforced using tick timestamps."""
        # Create ticks with rapidly changing SNR, spaced 50ms apart
        # With min_between_changes_ms=200, changes should be rate-limited
        ticks = []
        for i in range(40):
            # Alternate between high and low SNR every tick
            snr = 30 if i % 2 == 0 else 8
            ticks.append(_make_tick(ts=i * 50, snr=snr))

        df = _ticks_to_df(ticks)
        config = _make_config({
            'profile selection': {
                'min_between_changes_ms': '200',
                'fast_downgrade': 'True',
                'hysteresis_percent': '1',
                'hysteresis_percent_down': '1',
            }
        })

        result = ReplaySimulator(df, config).run()
        # With 50ms spacing and 200ms rate limit, at most 1 change per 4 ticks
        # 40 ticks = max ~10 changes (including initial)
        assert result.transition_count < 20  # Sanity: less than every-other-tick

    def test_no_wall_clock_dependency(self):
        """Results must be the same regardless of tick spacing (same order)."""
        ticks_fast = [_make_tick(ts=i * 100, snr=25.0) for i in range(20)]
        ticks_slow = [_make_tick(ts=i * 1000, snr=25.0) for i in range(20)]

        config = _make_config({
            'profile selection': {
                'min_between_changes_ms': '50',
                'hold_modes_down_ms': '50',
            }
        })

        r_fast = ReplaySimulator(_ticks_to_df(ticks_fast), config).run()
        r_slow = ReplaySimulator(_ticks_to_df(ticks_slow), config).run()

        # Both should produce the same MCS decisions since
        # hold timers are short enough that all changes are allowed
        assert r_fast.mean_bitrate == r_slow.mean_bitrate


class TestReplaySimulatorFitness:
    """Fitness function behavior."""

    def test_stable_high_snr_good_fitness(self):
        """Stable, high-SNR conditions should produce good fitness."""
        ticks = [_make_tick(ts=i * 100, snr=35.0, rssi=-30) for i in range(50)]
        df = _ticks_to_df(ticks)
        config = _make_config()

        result = ReplaySimulator(df, config).run()
        assert result.total_fitness > 0.3  # Should be positive and reasonable
        assert result.crash_events == 0
        assert result.mean_bitrate > 0

    def test_low_snr_conservative_no_crashes(self):
        """Low SNR should produce conservative MCS, not crashes."""
        ticks = [_make_tick(ts=i * 100, snr=7.0, rssi=-75) for i in range(50)]
        df = _ticks_to_df(ticks)
        config = _make_config()

        result = ReplaySimulator(df, config).run()
        assert result.crash_events == 0  # Should not crash at low MCS
        assert result.total_fitness > 0  # Should still be positive

    def test_crash_penalty_dominates(self):
        """A parameter set causing crashes must score worse than conservative one."""
        ticks = [_make_tick(ts=i * 100, snr=12.0, rssi=-65) for i in range(50)]
        df = _ticks_to_df(ticks)

        # Conservative: high safety margin
        config_safe = _make_config({'dynamic': {'snr_safety_margin': '8'}})
        r_safe = ReplaySimulator(df.copy(), config_safe).run()

        # Aggressive: no safety margin, may select MCS too high for SNR
        config_aggr = _make_config({'dynamic': {'snr_safety_margin': '0'}})
        r_aggr = ReplaySimulator(df.copy(), config_aggr).run()

        # If aggressive causes crashes, it should score worse
        if r_aggr.crash_events > r_safe.crash_events:
            assert r_safe.total_fitness > r_aggr.total_fitness

    def test_empty_ticks(self):
        """Empty DataFrame should return zero-valued result."""
        df = pd.DataFrame()
        config = _make_config()
        result = ReplaySimulator(df, config).run()
        assert result.total_ticks == 0
        assert result.total_fitness == 0.0

    def test_single_tick(self):
        """Single tick should produce valid result."""
        ticks = [_make_tick(ts=1000, snr=20.0)]
        df = _ticks_to_df(ticks)
        config = _make_config()
        result = ReplaySimulator(df, config).run()
        assert result.total_ticks == 1
        assert not math.isnan(result.total_fitness)

    def test_throughput_reward(self):
        """Higher SNR should yield higher throughput and better fitness."""
        ticks_high = [_make_tick(ts=i * 100, snr=35.0, rssi=-25) for i in range(50)]
        ticks_low = [_make_tick(ts=i * 100, snr=10.0, rssi=-70) for i in range(50)]

        config = _make_config()

        r_high = ReplaySimulator(_ticks_to_df(ticks_high), config).run()
        r_low = ReplaySimulator(_ticks_to_df(ticks_low), config).run()

        assert r_high.mean_bitrate > r_low.mean_bitrate
