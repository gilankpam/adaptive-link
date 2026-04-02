"""Tests for ground-station/ml/feature_engineering.py"""

import json
import os
import sys
import tempfile

import numpy as np
import pandas as pd
import pytest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..', '..', 'ground-station'))
from ml.feature_engineering import (
    MCS_SNR_THRESHOLDS,
    compute_all_features,
    compute_fec_saturation,
    compute_link_budget_margin,
    compute_loss_accel,
    compute_score_volatility,
    compute_snr_roc,
    compute_time_since_change,
    join_outcomes,
    load_telemetry,
)


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


def _make_outcome(change_ts=1000, avg_loss=0.01, max_loss=0.03, label='good', ticks=10):
    return {
        'type': 'outcome', 'change_ts': change_ts,
        'avg_loss': avg_loss, 'max_loss': max_loss,
        'label': label, 'ticks': ticks,
    }


def _write_jsonl(directory, records, filename='telemetry_0.jsonl'):
    path = os.path.join(directory, filename)
    with open(path, 'w') as f:
        for r in records:
            f.write(json.dumps(r) + '\n')
    return path


# =============================================================
# Tests for load_telemetry
# =============================================================

class TestLoadTelemetry:

    def test_load_valid_data(self, tmp_path):
        ticks = [_make_tick(ts=i * 100) for i in range(5)]
        _write_jsonl(str(tmp_path), ticks)

        ticks_df, outcomes_df = load_telemetry(str(tmp_path))
        assert len(ticks_df) == 5
        assert outcomes_df.empty

    def test_separates_outcome_records(self, tmp_path):
        records = [
            _make_tick(ts=100),
            _make_tick(ts=200, changed=True),
            _make_outcome(change_ts=200),
            _make_tick(ts=300),
        ]
        _write_jsonl(str(tmp_path), records)

        ticks_df, outcomes_df = load_telemetry(str(tmp_path))
        assert len(ticks_df) == 3
        assert len(outcomes_df) == 1
        assert outcomes_df.iloc[0]['label'] == 'good'

    def test_loads_multiple_files(self, tmp_path):
        _write_jsonl(str(tmp_path), [_make_tick(ts=100)], 'telemetry_0.jsonl')
        _write_jsonl(str(tmp_path), [_make_tick(ts=200)], 'telemetry_1.jsonl')

        ticks_df, _ = load_telemetry(str(tmp_path))
        assert len(ticks_df) == 2

    def test_missing_directory_raises(self):
        with pytest.raises(FileNotFoundError):
            load_telemetry('/nonexistent/path')

    def test_empty_directory_raises(self, tmp_path):
        with pytest.raises(FileNotFoundError, match='No telemetry'):
            load_telemetry(str(tmp_path))

    def test_missing_fields_raises(self, tmp_path):
        # Write a record missing required fields
        bad_record = {'ts': 100, 'rssi': -40}
        _write_jsonl(str(tmp_path), [bad_record])

        with pytest.raises(ValueError, match='Missing required fields'):
            load_telemetry(str(tmp_path))


# =============================================================
# Tests for join_outcomes
# =============================================================

class TestJoinOutcomes:

    def test_joins_outcome_to_tick(self):
        ticks_df = pd.DataFrame([
            _make_tick(ts=100),
            _make_tick(ts=200, changed=True),
            _make_tick(ts=300),
        ])
        outcomes_df = pd.DataFrame([
            _make_outcome(change_ts=200, label='bad', avg_loss=0.06, max_loss=0.1),
        ])

        result = join_outcomes(ticks_df, outcomes_df)
        assert result.loc[1, 'outcome_label'] == 'bad'
        assert result.loc[1, 'outcome_avg_loss'] == 0.06
        assert result.loc[0, 'outcome_label'] is None
        assert result.loc[2, 'outcome_label'] is None

    def test_empty_outcomes(self):
        ticks_df = pd.DataFrame([_make_tick(ts=100)])
        outcomes_df = pd.DataFrame(
            columns=['type', 'change_ts', 'avg_loss', 'max_loss', 'label', 'ticks']
        )
        result = join_outcomes(ticks_df, outcomes_df)
        assert result.loc[0, 'outcome_label'] is None


# =============================================================
# Tests for derived feature functions
# =============================================================

class TestComputeSnrRoc:

    def test_consecutive_ticks(self):
        df = pd.DataFrame([
            _make_tick(snr_ema=25.0),
            _make_tick(snr_ema=23.0),
            _make_tick(snr_ema=26.0),
        ])
        result = compute_snr_roc(df)
        assert result.iloc[0] == 0.0  # first tick
        assert result.iloc[1] == -2.0
        assert result.iloc[2] == 3.0

    def test_single_tick(self):
        df = pd.DataFrame([_make_tick(snr_ema=20.0)])
        result = compute_snr_roc(df)
        assert result.iloc[0] == 0.0


class TestComputeLossAccel:

    def test_accelerating_loss(self):
        df = pd.DataFrame([
            _make_tick(loss_rate=0.01),
            _make_tick(loss_rate=0.03),
            _make_tick(loss_rate=0.07),
        ])
        result = compute_loss_accel(df)
        assert result.iloc[0] == 0.0
        assert result.iloc[1] == 0.0
        # First deriv: [0, 0.02, 0.04], second deriv: [0, 0.02, 0.02]
        assert abs(result.iloc[2] - 0.02) < 1e-9

    def test_single_tick(self):
        df = pd.DataFrame([_make_tick(loss_rate=0.05)])
        result = compute_loss_accel(df)
        assert result.iloc[0] == 0.0

    def test_two_ticks(self):
        df = pd.DataFrame([
            _make_tick(loss_rate=0.01),
            _make_tick(loss_rate=0.03),
        ])
        result = compute_loss_accel(df)
        assert result.iloc[0] == 0.0
        assert result.iloc[1] == 0.0  # needs 3 ticks for 2nd derivative


class TestComputeFecSaturation:

    def test_moderate_pressure(self):
        df = pd.DataFrame([_make_tick(fec_pressure=0.6)])
        result = compute_fec_saturation(df)
        assert result.iloc[0] == 0.6

    def test_no_pressure(self):
        df = pd.DataFrame([_make_tick(fec_pressure=0.0)])
        result = compute_fec_saturation(df)
        assert result.iloc[0] == 0.0

    def test_clamps_above_one(self):
        df = pd.DataFrame([_make_tick(fec_pressure=1.5)])
        result = compute_fec_saturation(df)
        assert result.iloc[0] == 1.0


class TestComputeScoreVolatility:

    def test_stable_link(self):
        df = pd.DataFrame([_make_tick(score=1500 + i * 0.5) for i in range(20)])
        result = compute_score_volatility(df)
        assert result.iloc[-1] < 5.0

    def test_unstable_link(self):
        scores = [1200 if i % 2 == 0 else 1800 for i in range(20)]
        df = pd.DataFrame([_make_tick(score=s) for s in scores])
        result = compute_score_volatility(df)
        assert result.iloc[-1] > 100.0

    def test_fewer_ticks_than_window(self):
        df = pd.DataFrame([_make_tick(score=1500 + i * 10) for i in range(5)])
        result = compute_score_volatility(df, window=20)
        # Should still compute over available ticks
        assert result.iloc[-1] > 0


class TestComputeLinkBudgetMargin:

    def test_comfortable_margin(self):
        # MCS 4 threshold = 17 dB
        df = pd.DataFrame([_make_tick(snr_ema=30.0, mcs=4)])
        result = compute_link_budget_margin(df)
        assert result.iloc[0] == 30.0 - MCS_SNR_THRESHOLDS[4]  # 30 - 17 = 13

    def test_marginal_link(self):
        df = pd.DataFrame([_make_tick(snr_ema=18.0, mcs=4)])
        result = compute_link_budget_margin(df)
        assert result.iloc[0] == 18.0 - MCS_SNR_THRESHOLDS[4]  # 18 - 17 = 1

    def test_below_threshold(self):
        df = pd.DataFrame([_make_tick(snr_ema=15.0, mcs=4)])
        result = compute_link_budget_margin(df)
        assert result.iloc[0] == 15.0 - MCS_SNR_THRESHOLDS[4]  # 15 - 17 = -2

    def test_no_mcs_column(self):
        df = pd.DataFrame([_make_tick()])
        df = df.drop(columns=['mcs'])
        result = compute_link_budget_margin(df)
        assert pd.isna(result.iloc[0])


class TestComputeTimeSinceChange:

    def test_recent_change(self):
        df = pd.DataFrame([
            _make_tick(ts=1000, changed=True),
            _make_tick(ts=1100, changed=False),
            _make_tick(ts=1500, changed=False),
        ])
        result = compute_time_since_change(df)
        assert result.iloc[0] == 0  # change tick itself
        assert result.iloc[1] == 100
        assert result.iloc[2] == 500

    def test_no_prior_change(self):
        df = pd.DataFrame([
            _make_tick(ts=1000, changed=False),
            _make_tick(ts=1100, changed=False),
        ])
        result = compute_time_since_change(df)
        assert pd.isna(result.iloc[0])
        assert pd.isna(result.iloc[1])

    def test_multiple_changes(self):
        df = pd.DataFrame([
            _make_tick(ts=1000, changed=True),
            _make_tick(ts=1100, changed=False),
            _make_tick(ts=1200, changed=True),
            _make_tick(ts=1500, changed=False),
        ])
        result = compute_time_since_change(df)
        assert result.iloc[1] == 100  # from first change
        assert result.iloc[2] == 0    # second change itself
        assert result.iloc[3] == 300  # from second change


# =============================================================
# Tests for compute_all_features
# =============================================================

class TestComputeAllFeatures:

    def test_all_columns_present(self):
        df = pd.DataFrame([
            _make_tick(ts=100, snr_ema=25.0, loss_rate=0.01, fec_pressure=0.1,
                       score=1800, mcs=4, changed=True),
            _make_tick(ts=200, snr_ema=23.0, loss_rate=0.02, fec_pressure=0.2,
                       score=1750, mcs=4, changed=False),
            _make_tick(ts=300, snr_ema=24.0, loss_rate=0.015, fec_pressure=0.15,
                       score=1780, mcs=4, changed=False),
        ])
        result = compute_all_features(df)

        expected_columns = [
            'snr_roc', 'loss_accel', 'fec_saturation',
            'score_volatility', 'link_budget_margin', 'time_since_change',
        ]
        for col in expected_columns:
            assert col in result.columns, f"Missing column: {col}"

    def test_does_not_modify_original(self):
        df = pd.DataFrame([_make_tick(ts=100), _make_tick(ts=200)])
        original_cols = set(df.columns)
        compute_all_features(df)
        assert set(df.columns) == original_cols
