"""Tests for ground-station/ml/optimize_params.py"""

import configparser
import json
import os
import sys
import tempfile

import optuna
import pandas as pd
import pytest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..'))
from ml.optimize_params import (
    AdapterOptimizer,
    ParameterSpace,
    _load_adapter_constraints,
    write_optimized_config,
)
from ml.replay_simulator import load_config_from_file

_CONFIG_PATH = os.path.join(os.path.dirname(__file__), '..', '..', 'config', 'alink_gs.conf')
_BASE_CONFIG_STR = load_config_from_file(_CONFIG_PATH)

# Import ProfileSelector for config validation
_gs_path = os.path.join(os.path.dirname(__file__), '..', 'alink_gs')
with open(_gs_path) as _f:
    _code = _f.read().split("if __name__")[0]
exec(_code)


def _make_tick(ts=1000, rssi=-40, snr=25.0, rssi_min=-45, ant=2,
               pkt_all=100, pkt_lost=1, pkt_fec=0, fec_k=8, fec_n=12,
               loss_rate=0.01, fec_pressure=0.1, rf_score=0.8,
               loss_score=0.9, fec_score=0.9, div_score=0.95,
               score=1850.0, ema_fast=1850.0, ema_slow=1840.0,
               snr_ema=25.0, changed=False, adapter='test-adapter',
               mcs=4, gi='short', sel_fec_k=8, sel_fec_n=12,
               bitrate=15000, power=30, **overrides):
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


def _make_ticks_df(n=50, adapter='test-adapter', snr=25.0):
    ticks = [_make_tick(ts=i * 100, adapter=adapter, snr=snr) for i in range(n)]
    return pd.DataFrame(ticks)


# =============================================================
# Tests for ParameterSpace
# =============================================================

class TestParameterSpace:

    def test_scoring_weights_sum_to_one(self):
        """Scoring weights must sum to approximately 1.0."""
        space = ParameterSpace(_BASE_CONFIG_STR)
        study = optuna.create_study()

        for _ in range(10):
            trial = study.ask()
            config = space.define_trial(trial)
            study.tell(trial, 0.0)

            rf = config.getfloat('scoring', 'rf_weight')
            loss = config.getfloat('scoring', 'loss_weight')
            fec = config.getfloat('scoring', 'fec_weight')
            div = config.getfloat('scoring', 'diversity_weight')
            total = rf + loss + fec + div
            assert 0.1 <= total <= 1.7, f"Weights sum to {total}"

    def test_rf_weights_sum_to_one(self):
        """snr_weight + rssi_weight must equal 1.0."""
        space = ParameterSpace(_BASE_CONFIG_STR)
        study = optuna.create_study()

        for _ in range(5):
            trial = study.ask()
            config = space.define_trial(trial)
            study.tell(trial, 0.0)

            snr = config.getfloat('weights', 'snr_weight')
            rssi = config.getfloat('weights', 'rssi_weight')
            assert abs(snr + rssi - 1.0) < 1e-9

    def test_config_creates_valid_profile_selector(self):
        """Generated config should create a valid ProfileSelector."""
        space = ParameterSpace(_BASE_CONFIG_STR)
        study = optuna.create_study()
        trial = study.ask()
        config = space.define_trial(trial)
        study.tell(trial, 0.0)

        # Should not raise
        ps = ProfileSelector(config)

    def test_adapter_max_mcs_constraint(self):
        """max_mcs should be bounded by adapter capability."""
        space = ParameterSpace(_BASE_CONFIG_STR, {'max_mcs': 3, 'bandwidths': [20]})
        study = optuna.create_study()

        for _ in range(10):
            trial = study.ask()
            config = space.define_trial(trial)
            study.tell(trial, 0.0)

            max_mcs = config.getint('dynamic', 'max_mcs')
            assert max_mcs <= 3, f"max_mcs={max_mcs} exceeds adapter limit of 3"

    def test_all_sections_present(self):
        """Generated config should have all expected sections."""
        space = ParameterSpace(_BASE_CONFIG_STR)
        study = optuna.create_study()
        trial = study.ask()
        config = space.define_trial(trial)
        study.tell(trial, 0.0)

        expected = ['scoring', 'weights', 'ranges', 'profile selection',
                    'dynamic', 'noise', 'error estimation']
        for section in expected:
            assert config.has_section(section), f"Missing section: {section}"


# =============================================================
# Tests for AdapterOptimizer
# =============================================================

class TestAdapterOptimizer:

    def test_single_trial_completes(self):
        """Objective function should complete without error for one trial."""
        df = _make_ticks_df(50)
        optimizer = AdapterOptimizer('test-adapter', df, _BASE_CONFIG_STR, n_trials=1, seed=42)
        best_config, fitness, study = optimizer.optimize()

        assert isinstance(best_config, configparser.ConfigParser)
        assert isinstance(fitness, float)
        assert len(study.trials) == 1

    def test_adapter_filtering(self):
        """Only ticks matching adapter_id should be used."""
        ticks_a = [_make_tick(ts=i * 100, adapter='adapter-a', snr=25.0)
                   for i in range(30)]
        ticks_b = [_make_tick(ts=i * 100, adapter='adapter-b', snr=10.0)
                   for i in range(30)]
        df = pd.DataFrame(ticks_a + ticks_b)

        opt = AdapterOptimizer('adapter-a', df, _BASE_CONFIG_STR, n_trials=1, seed=42)
        assert len(opt.ticks_df) == 30
        assert all(opt.ticks_df['adapter'] == 'adapter-a')

    def test_empty_adapter_raises(self):
        """Should raise ValueError when no ticks match the adapter."""
        df = _make_ticks_df(50, adapter='other-adapter')
        with pytest.raises(ValueError, match="No telemetry"):
            AdapterOptimizer('nonexistent', df, _BASE_CONFIG_STR)

    def test_default_fitness(self):
        """get_default_fitness should return valid results."""
        df = _make_ticks_df(50)
        opt = AdapterOptimizer('test-adapter', df, _BASE_CONFIG_STR, n_trials=1)
        fitness, result = opt.get_default_fitness()
        assert isinstance(fitness, float)
        assert result.total_ticks == 50

    def test_reproducible_with_same_seed(self):
        """Same seed should produce identical results."""
        df = _make_ticks_df(50)
        _, f1, _ = AdapterOptimizer('test-adapter', df, _BASE_CONFIG_STR, n_trials=3, seed=42).optimize()
        _, f2, _ = AdapterOptimizer('test-adapter', df, _BASE_CONFIG_STR, n_trials=3, seed=42).optimize()
        assert f1 == f2


# =============================================================
# Tests for config output
# =============================================================

class TestConfigOutput:

    def test_write_valid_ini(self, tmp_path):
        """Written config should be valid INI parseable by configparser."""
        config = configparser.ConfigParser()
        config.read_string(_BASE_CONFIG_STR)

        output_path = str(tmp_path / 'test.conf')
        write_optimized_config(config, output_path, 'test', 10, 0.3, 0.5)

        # Should parse without error
        loaded = configparser.ConfigParser()
        loaded.read(output_path)
        assert loaded.has_section('scoring')
        assert loaded.has_section('dynamic')

    def test_metadata_header(self, tmp_path):
        """Output should contain metadata in header comments."""
        config = configparser.ConfigParser()
        config.read_string(_BASE_CONFIG_STR)

        output_path = str(tmp_path / 'test.conf')
        write_optimized_config(config, output_path, 'my-adapter', 100, 0.3, 0.5)

        with open(output_path) as f:
            content = f.read()

        assert 'my-adapter' in content
        assert '100 trials' in content
        assert 'default=0.3000' in content
        assert 'optimized=0.5000' in content

    def test_creates_output_directory(self, tmp_path):
        """Should create output directory if it doesn't exist."""
        output_path = str(tmp_path / 'subdir' / 'test.conf')
        config = configparser.ConfigParser()
        config.read_string(_BASE_CONFIG_STR)

        write_optimized_config(config, output_path, 'test', 5, 0.3, 0.4)
        assert os.path.exists(output_path)


# =============================================================
# Tests for adapter constraints loading
# =============================================================

class TestLoadAdapterConstraints:

    def test_load_constraints(self, tmp_path):
        """Should parse adapter constraints from YAML."""
        yaml_content = {
            'profiles': {
                'test-adapter': {
                    'mcs': [0, 1, 2, 3],
                    'bw': [20],
                }
            }
        }
        yaml_path = str(tmp_path / 'adapters.yaml')
        import yaml
        with open(yaml_path, 'w') as f:
            yaml.dump(yaml_content, f)

        constraints = _load_adapter_constraints(yaml_path)
        assert constraints['test-adapter']['max_mcs'] == 3
        assert constraints['test-adapter']['bandwidths'] == [20]

    def test_missing_file(self):
        """Missing YAML file should return empty dict."""
        result = _load_adapter_constraints('/nonexistent/path.yaml')
        assert result == {}
