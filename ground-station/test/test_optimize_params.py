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
    ALL_PARAM_NAMES,
    PARAM_REGISTRY,
    PARAM_SECTION,
    AdapterOptimizer,
    ParameterSpace,
    _expand_param_tokens,
    _read_skip_set,
    _resolve_selection,
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

# Mock handshake for testing
class _MockHandshake:
    def get_fps(self): return 60
    def correct_timestamp(self, ts): return ts
    def is_synced(self): return False


def _make_tick(ts=1000, rssi=-40, snr=25.0, rssi_min=-45, ant=2,
               pkt_all=100, pkt_lost=1, pkt_fec=0, fec_k=8, fec_n=12,
               loss_rate=0.01, fec_pressure=0.1,
               snr_ema=25.0, snr_slope=0.0,
               margin_cur=5.0, margin_tgt=5.0, emergency=False,
               changed=False, adapter='test-adapter',
               mcs=4, gi='short', sel_fec_k=8, sel_fec_n=12,
               bitrate=15000, power=30, **overrides):
    record = {
        'ts': ts, 'rssi': rssi, 'snr': snr, 'rssi_min': rssi_min,
        'ant': ant, 'pkt_all': pkt_all, 'pkt_lost': pkt_lost,
        'pkt_fec': pkt_fec, 'fec_k': fec_k, 'fec_n': fec_n,
        'loss_rate': loss_rate, 'fec_pressure': fec_pressure,
        'snr_ema': snr_ema, 'snr_slope': snr_slope,
        'margin_cur': margin_cur, 'margin_tgt': margin_tgt,
        'emergency': emergency,
        'changed': changed, 'adapter': adapter,
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

    def test_gate_params_in_range(self):
        """Gate parameters must land inside their declared search bounds."""
        space = ParameterSpace(_BASE_CONFIG_STR)
        study = optuna.create_study()

        for _ in range(10):
            trial = study.ask()
            config = space.define_trial(trial)
            study.tell(trial, 0.0)

            assert 0.5 <= config.getfloat('gate', 'hysteresis_up_db') <= 6.0
            assert 0.0 <= config.getfloat('gate', 'hysteresis_down_db') <= 4.0
            assert 0.05 <= config.getfloat('gate', 'snr_slope_alpha') <= 0.8
            assert 0.0 <= config.getfloat('gate', 'snr_predict_horizon_ticks') <= 10.0
            assert 0.05 <= config.getfloat('gate', 'emergency_loss_rate') <= 0.35
            assert 0.4 <= config.getfloat('gate', 'emergency_fec_pressure') <= 0.95

    def test_config_creates_valid_profile_selector(self):
        """Generated config should create a valid ProfileSelector."""
        space = ParameterSpace(_BASE_CONFIG_STR)
        study = optuna.create_study()
        trial = study.ask()
        config = space.define_trial(trial)
        study.tell(trial, 0.0)

        # Should not raise
        ps = ProfileSelector(config, _MockHandshake())

    def test_adapter_max_mcs_constraint(self):
        """max_mcs should be bounded by max_mcs_bound parameter."""
        space = ParameterSpace(_BASE_CONFIG_STR, max_mcs_bound=3)
        study = optuna.create_study()

        for _ in range(10):
            trial = study.ask()
            config = space.define_trial(trial)
            study.tell(trial, 0.0)

            max_mcs = config.getint('gate', 'max_mcs')
            assert max_mcs <= 3, f"max_mcs={max_mcs} exceeds bound of 3"

    def test_all_sections_present(self):
        """Generated config should have all expected sections."""
        space = ParameterSpace(_BASE_CONFIG_STR)
        study = optuna.create_study()
        trial = study.ask()
        config = space.define_trial(trial)
        study.tell(trial, 0.0)

        expected = ['profile selection', 'gate', 'dynamic', 'hardware']
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

_BASELINE_LINES = _BASE_CONFIG_STR.splitlines(keepends=True)


class TestConfigOutput:

    def test_write_valid_ini(self, tmp_path):
        """Written config should be valid INI parseable by configparser."""
        output_path = str(tmp_path / 'test.conf')
        write_optimized_config(
            _BASELINE_LINES, {}, output_path, 'test', 10, 0.3, 0.5,
            [], [],
        )

        loaded = configparser.ConfigParser()
        loaded.read(output_path)
        assert loaded.has_section('gate')
        assert loaded.has_section('dynamic')
        assert loaded.has_section('hardware')

    def test_metadata_header(self, tmp_path):
        """Output should contain metadata in header comments."""
        output_path = str(tmp_path / 'test.conf')
        write_optimized_config(
            _BASELINE_LINES, {}, output_path, 'my-adapter', 100, 0.3, 0.5,
            ['hysteresis_up_db'], [],
        )

        with open(output_path) as f:
            content = f.read()

        assert 'my-adapter' in content
        assert '100 trials' in content
        assert 'default=0.3000' in content
        assert 'optimized=0.5000' in content

    def test_creates_output_directory(self, tmp_path):
        """Should create output directory if it doesn't exist."""
        output_path = str(tmp_path / 'subdir' / 'test.conf')
        write_optimized_config(
            _BASELINE_LINES, {}, output_path, 'test', 5, 0.3, 0.4,
            [], [],
        )
        assert os.path.exists(output_path)


# =============================================================
# Tests for selected_params in ParameterSpace
# =============================================================

class TestSelectedParams:

    def test_selected_params_only_samples_subset(self):
        """Unselected params must keep their baseline values across trials."""
        baseline = configparser.ConfigParser()
        baseline.read_string(_BASE_CONFIG_STR)
        base_hyst_down = baseline.getfloat('gate', 'hysteresis_down_db')
        base_max_mcs = baseline.getint('gate', 'max_mcs')

        space = ParameterSpace(
            _BASE_CONFIG_STR, selected_params={'hysteresis_up_db'}
        )
        study = optuna.create_study()
        for _ in range(5):
            trial = study.ask()
            config = space.define_trial(trial)
            study.tell(trial, 0.0)
            # Selected param should be inside its bounds.
            assert 0.5 <= config.getfloat('gate', 'hysteresis_up_db') <= 6.0
            # Unselected params should equal the baseline, not sampled values.
            assert config.getfloat('gate', 'hysteresis_down_db') == base_hyst_down
            assert config.getint('gate', 'max_mcs') == base_max_mcs

    def test_none_selected_samples_everything(self):
        """selected_params=None preserves legacy 'sample every param' behavior."""
        space = ParameterSpace(_BASE_CONFIG_STR, selected_params=None)
        study = optuna.create_study()
        trial = study.ask()
        space.define_trial(trial)
        study.tell(trial, 0.0)
        # Every registry param should have been suggested on this trial.
        assert set(trial.params.keys()) == ALL_PARAM_NAMES


# =============================================================
# Tests for skip_optimize_params ([optimizer] section)
# =============================================================

class TestSkipOptimizeParams:

    def test_read_skip_set_empty_when_missing(self):
        cfg = "[gate]\nhysteresis_up_db = 2.5\n"
        assert _read_skip_set(cfg) == set()

    def test_read_skip_set_parses_list(self):
        cfg = (
            "[optimizer]\n"
            "skip_optimize_params = hysteresis_up_db, max_mcs\n"
        )
        assert _read_skip_set(cfg) == {'hysteresis_up_db', 'max_mcs'}

    def test_read_skip_set_drops_unknown(self, capsys):
        cfg = (
            "[optimizer]\n"
            "skip_optimize_params = hysteresis_up_db, bogus_param\n"
        )
        result = _read_skip_set(cfg)
        assert result == {'hysteresis_up_db'}
        captured = capsys.readouterr()
        assert 'bogus_param' in captured.out

    def test_read_skip_set_empty_string(self):
        cfg = "[optimizer]\nskip_optimize_params =\n"
        assert _read_skip_set(cfg) == set()


# =============================================================
# Tests for _expand_param_tokens and _resolve_selection
# =============================================================

class TestSelectionResolution:

    def test_expand_individual_names(self):
        result = _expand_param_tokens('hysteresis_up_db,max_mcs')
        assert result == {'hysteresis_up_db', 'max_mcs'}

    def test_expand_section_shorthand(self):
        gate_names = {p[0] for p in PARAM_REGISTRY if p[1] == 'gate'}
        assert _expand_param_tokens('gate') == gate_names

    def test_expand_mixed(self):
        gate_names = {p[0] for p in PARAM_REGISTRY if p[1] == 'gate'}
        result = _expand_param_tokens('gate,max_mcs')
        assert result == gate_names | {'max_mcs'}

    def test_expand_profile_selection_alias(self):
        ps_names = {p[0] for p in PARAM_REGISTRY if p[1] == 'profile selection'}
        assert _expand_param_tokens('profile_selection') == ps_names
        assert _expand_param_tokens('profile-selection') == ps_names

    def test_expand_unknown_raises(self):
        with pytest.raises(SystemExit, match='Unknown parameter'):
            _expand_param_tokens('not_a_param')

    def test_resolve_default_is_all(self):
        assert _resolve_selection(None, None, set()) == ALL_PARAM_NAMES

    def test_resolve_params_flag(self):
        result = _resolve_selection('hysteresis_up_db', None, set())
        assert result == {'hysteresis_up_db'}

    def test_resolve_exclude_flag(self):
        gate_names = {p[0] for p in PARAM_REGISTRY if p[1] == 'gate'}
        result = _resolve_selection(None, 'gate', set())
        assert result == ALL_PARAM_NAMES - gate_names

    def test_resolve_skip_wins_over_params(self):
        """skip_set strips out names even if the user passed them via --params."""
        result = _resolve_selection(
            'hysteresis_up_db,max_mcs', None, {'hysteresis_up_db'}
        )
        assert result == {'max_mcs'}

    def test_resolve_empty_raises(self):
        """If selection is empty after applying the skip set, exit loudly."""
        with pytest.raises(SystemExit, match='No parameters left'):
            _resolve_selection('hysteresis_up_db', None, {'hysteresis_up_db'})


# =============================================================
# Tests for comment-preserving config output
# =============================================================

class TestConfigOutputPreservation:

    def test_tuned_only_lines_change(self, tmp_path):
        """Output differs from baseline only on tuned value lines + header."""
        output_path = str(tmp_path / 'out.conf')
        optimized_values = {('gate', 'hysteresis_up_db'): '3.7'}
        write_optimized_config(
            _BASELINE_LINES, optimized_values, output_path,
            'test', 10, 0.3, 0.5,
            ['hysteresis_up_db'], [],
        )

        with open(output_path) as f:
            out_lines = f.readlines()

        # Strip header (starts with '#', ends at first blank line).
        header_end = out_lines.index('\n') + 1
        body = out_lines[header_end:]

        # Baseline length must match body length (line-for-line preservation).
        assert len(body) == len(_BASELINE_LINES)

        # Exactly one line changed: the hysteresis_up_db value line.
        diffs = [
            (i, a, b)
            for i, (a, b) in enumerate(zip(_BASELINE_LINES, body))
            if a != b
        ]
        assert len(diffs) == 1
        _, base_line, new_line = diffs[0]
        assert 'hysteresis_up_db' in base_line
        assert new_line.strip() == 'hysteresis_up_db = 3.7'

    def test_doc_comments_preserved(self, tmp_path):
        """The [gate] documentation block survives byte-for-byte."""
        output_path = str(tmp_path / 'out.conf')
        write_optimized_config(
            _BASELINE_LINES, {}, output_path, 'test', 1, 0.0, 0.0, [], [],
        )
        with open(output_path) as f:
            content = f.read()
        # Lines from the gate doc block in config/alink_gs.conf.
        assert '# Two-channel gate tuning (physical units, dB of SNR margin).' in content
        assert '# Channel A (SNR margin, slow/symmetric):' in content
        assert '#   emergency_loss_rate' in content

    def test_optimizer_section_round_trips(self, tmp_path):
        """A baseline with skip_optimize_params preserves that line verbatim."""
        baseline_with_skip = _BASE_CONFIG_STR.replace(
            'skip_optimize_params =',
            'skip_optimize_params = hysteresis_up_db, max_mcs',
        )
        lines = baseline_with_skip.splitlines(keepends=True)
        output_path = str(tmp_path / 'out.conf')
        write_optimized_config(
            lines, {}, output_path, 'test', 1, 0.0, 0.0, [], [],
        )
        with open(output_path) as f:
            content = f.read()
        assert 'skip_optimize_params = hysteresis_up_db, max_mcs' in content

    def test_metadata_lists_tuned_and_skipped(self, tmp_path):
        output_path = str(tmp_path / 'out.conf')
        write_optimized_config(
            _BASELINE_LINES, {}, output_path, 'test', 1, 0.0, 0.0,
            ['hysteresis_up_db', 'max_mcs'],
            ['fec_redundancy_ratio'],
        )
        with open(output_path) as f:
            content = f.read()
        assert '# Tuned parameters: hysteresis_up_db, max_mcs' in content
        assert '# Skipped (skip_optimize_params): fec_redundancy_ratio' in content

    def test_missing_tuned_key_raises(self, tmp_path):
        """If an optimized key is absent from the baseline, surface the error."""
        output_path = str(tmp_path / 'out.conf')
        # Use a key that exists in PARAM_REGISTRY but strip it from the baseline.
        stripped = [l for l in _BASELINE_LINES if not l.startswith('hysteresis_up_db')]
        with pytest.raises(RuntimeError, match='Could not locate tuned keys'):
            write_optimized_config(
                stripped, {('gate', 'hysteresis_up_db'): '3.0'},
                output_path, 'test', 1, 0.0, 0.0,
                ['hysteresis_up_db'], [],
            )


# =============================================================
# Tests for max_mcs_bound parameter
# =============================================================

class TestMaxMcsBound:

    def test_default_max_mcs_bound(self):
        """Default max_mcs_bound should be 7."""
        space = ParameterSpace(_BASE_CONFIG_STR)
        assert space.max_mcs_bound == 7

    def test_custom_max_mcs_bound(self):
        """Custom max_mcs_bound should be respected."""
        space = ParameterSpace(_BASE_CONFIG_STR, max_mcs_bound=4)
        assert space.max_mcs_bound == 4

    def test_max_mcs_samples_within_bound(self):
        """max_mcs should only sample values within the bound."""
        space = ParameterSpace(_BASE_CONFIG_STR, selected_params={'max_mcs'}, max_mcs_bound=4)
        study = optuna.create_study()

        for _ in range(10):
            trial = study.ask()
            config = space.define_trial(trial)
            study.tell(trial, 0.0)

            max_mcs = config.getint('gate', 'max_mcs')
            assert 0 <= max_mcs <= 4, f"max_mcs={max_mcs} outside expected range [0, 4]"
