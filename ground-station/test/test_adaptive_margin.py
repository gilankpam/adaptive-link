#!/usr/bin/env python3
"""Unit tests for AdaptiveParam / AdaptiveParams online learning in alink_gs."""
import sys
import os
import json
import tempfile
import configparser

import pytest

# Import module-level constants and classes by executing the script up to __main__
_gs_path = os.path.join(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))), 'ground-station', 'alink_gs')
with open(_gs_path) as _f:
    _code = _f.read().split("if __name__")[0]
exec(_code)


def _make_params(params_dict=None, outcome_window=5, persist_path=''):
    """Create an AdaptiveParams with optional pre-registered params."""
    ap = AdaptiveParams(outcome_window=outcome_window,
                        persist_path=persist_path)
    if params_dict:
        for name, kwargs in params_dict.items():
            ap.add(name, AdaptiveParam(**kwargs))
    return ap


# ── AdaptiveParam unit tests ──────────────────────────────────────────────

class TestAdaptiveParamUpdate:
    """Test single-param update logic."""

    def test_increase_on_bad_true(self):
        p = AdaptiveParam(value=5.0, lr_up=1.0, lr_down=0.1,
                          bounds=(0, 100), increase_on_bad=True)
        p.update(had_loss=True)
        assert p.value == 6.0

    def test_decrease_on_good_when_increase_on_bad(self):
        p = AdaptiveParam(value=5.0, lr_up=1.0, lr_down=0.1,
                          bounds=(0, 100), increase_on_bad=True)
        p.update(had_loss=False)
        assert p.value == pytest.approx(4.9)

    def test_decrease_on_bad_when_increase_on_bad_false(self):
        p = AdaptiveParam(value=0.5, lr_up=0.1, lr_down=0.01,
                          bounds=(0, 1), increase_on_bad=False)
        p.update(had_loss=True)
        assert p.value == pytest.approx(0.4)

    def test_increase_on_good_when_increase_on_bad_false(self):
        p = AdaptiveParam(value=0.5, lr_up=0.1, lr_down=0.01,
                          bounds=(0, 1), increase_on_bad=False)
        p.update(had_loss=False)
        assert p.value == pytest.approx(0.51)

    def test_clamps_to_upper_bound(self):
        p = AdaptiveParam(value=9.5, lr_up=1.0, lr_down=0.1,
                          bounds=(1, 10), increase_on_bad=True)
        p.update(had_loss=True)
        assert p.value == 10.0

    def test_clamps_to_lower_bound(self):
        p = AdaptiveParam(value=1.1, lr_up=1.0, lr_down=2.0,
                          bounds=(1, 10), increase_on_bad=True)
        p.update(had_loss=False)
        assert p.value == 1.0

    def test_asymmetry(self):
        p_bad = AdaptiveParam(value=5.0, lr_up=1.0, lr_down=0.1,
                              bounds=(0, 100), increase_on_bad=True)
        p_good = AdaptiveParam(value=5.0, lr_up=1.0, lr_down=0.1,
                               bounds=(0, 100), increase_on_bad=True)
        p_bad.update(had_loss=True)
        p_good.update(had_loss=False)
        assert abs(p_bad.value - 5.0) > abs(p_good.value - 5.0)


# ── AdaptiveParams outcome tracking tests ─────────────────────────────────

class TestAdaptiveParamsOutcome:
    """Test shared outcome tracking in AdaptiveParams."""

    def test_good_outcome_updates_all(self):
        ap = _make_params({
            'a': dict(value=5.0, lr_up=1.0, lr_down=0.1,
                      bounds=(0, 100), increase_on_bad=True),
            'b': dict(value=0.5, lr_up=0.1, lr_down=0.01,
                      bounds=(0, 1), increase_on_bad=False),
        })
        ap.on_profile_changed()
        for _ in range(5):
            ap.tick(0.0)
        # 'a' should decrease (increase_on_bad=True, good outcome)
        assert ap.get('a') < 5.0
        # 'b' should increase (increase_on_bad=False, good outcome)
        assert ap.get('b') > 0.5

    def test_bad_outcome_updates_all(self):
        ap = _make_params({
            'a': dict(value=5.0, lr_up=1.0, lr_down=0.1,
                      bounds=(0, 100), increase_on_bad=True),
            'b': dict(value=0.5, lr_up=0.1, lr_down=0.01,
                      bounds=(0, 1), increase_on_bad=False),
        })
        ap.on_profile_changed()
        ap.tick(0.05)  # loss
        for _ in range(4):
            ap.tick(0.0)
        # 'a' should increase (increase_on_bad=True)
        assert ap.get('a') > 5.0
        # 'b' should decrease (increase_on_bad=False)
        assert ap.get('b') < 0.5

    def test_no_update_without_profile_change(self):
        ap = _make_params({
            'x': dict(value=5.0, lr_up=1.0, lr_down=0.5,
                      bounds=(0, 100), increase_on_bad=True),
        })
        for _ in range(20):
            ap.tick(0.0)
        assert ap.get('x') == 5.0

    def test_new_profile_change_finalizes_previous(self):
        ap = _make_params({
            'x': dict(value=5.0, lr_up=1.0, lr_down=0.5,
                      bounds=(0, 100), increase_on_bad=True),
        }, outcome_window=10)
        # First change: start tracking
        ap.on_profile_changed()
        ap.tick(0.0)  # 1 outcome tick (good so far)
        # Second change before window completes — should finalize first
        ap.on_profile_changed()
        # First outcome finalized as good (no loss in collected ticks)
        assert ap.get('x') < 5.0


# ── Last outcome snapshot tests ──────────────────────────────────────────

class TestLastOutcomeSnapshot:
    """Test that _last_outcome captures values and deltas after finalization."""

    def test_none_before_any_outcome(self):
        ap = _make_params({
            'x': dict(value=5.0, lr_up=1.0, lr_down=0.5,
                      bounds=(0, 100), increase_on_bad=True),
        })
        assert ap._last_outcome is None

    def test_good_outcome_snapshot(self):
        ap = _make_params({
            'x': dict(value=5.0, lr_up=1.0, lr_down=0.5,
                      bounds=(0, 100), increase_on_bad=True),
        })
        ap.on_profile_changed()
        for _ in range(5):
            ap.tick(0.0)
        assert ap._last_outcome is not None
        assert ap._last_outcome['had_loss'] is False
        assert 'x' in ap._last_outcome['params']
        assert ap._last_outcome['params']['x']['value'] == pytest.approx(4.5)
        assert ap._last_outcome['params']['x']['delta'] == pytest.approx(-0.5)

    def test_bad_outcome_snapshot(self):
        ap = _make_params({
            'x': dict(value=5.0, lr_up=1.0, lr_down=0.5,
                      bounds=(0, 100), increase_on_bad=True),
        })
        ap.on_profile_changed()
        ap.tick(0.05)  # loss
        for _ in range(4):
            ap.tick(0.0)
        assert ap._last_outcome['had_loss'] is True
        assert ap._last_outcome['params']['x']['value'] == pytest.approx(6.0)
        assert ap._last_outcome['params']['x']['delta'] == pytest.approx(1.0)

    def test_deltas_correct_for_decrease_on_bad(self):
        ap = _make_params({
            'u': dict(value=0.5, lr_up=0.1, lr_down=0.01,
                      bounds=(0, 1), increase_on_bad=False),
        })
        ap.on_profile_changed()
        ap.tick(0.05)  # loss
        for _ in range(4):
            ap.tick(0.0)
        # increase_on_bad=False, bad outcome → delta should be negative
        assert ap._last_outcome['params']['u']['delta'] == pytest.approx(-0.1)

    def test_snapshot_includes_all_params(self):
        ap = _make_params({
            'a': dict(value=5.0, lr_up=1.0, lr_down=0.1,
                      bounds=(0, 100), increase_on_bad=True),
            'b': dict(value=0.5, lr_up=0.1, lr_down=0.01,
                      bounds=(0, 1), increase_on_bad=False),
        })
        ap.on_profile_changed()
        for _ in range(5):
            ap.tick(0.0)
        assert 'a' in ap._last_outcome['params']
        assert 'b' in ap._last_outcome['params']

    def test_zero_deltas_when_lr_zero(self):
        ap = _make_params({
            'x': dict(value=5.0, lr_up=0, lr_down=0,
                      bounds=(0, 100), increase_on_bad=True),
        })
        ap.on_profile_changed()
        for _ in range(5):
            ap.tick(0.0)
        assert ap._last_outcome['params']['x']['delta'] == 0.0
        assert ap._last_outcome['params']['x']['value'] == 5.0


# ── Persistence tests ─────────────────────────────────────────────────────

class TestPersistence:
    """Test save/load of learned values."""

    def test_save_and_load(self):
        with tempfile.NamedTemporaryFile(mode='w', suffix='.json',
                                         delete=False) as f:
            path = f.name
        try:
            ap1 = _make_params({
                'a': dict(value=4.2, bounds=(0, 100)),
                'b': dict(value=0.7, bounds=(0, 1)),
            })
            ap1.save(path)

            ap2 = _make_params({
                'a': dict(value=1.0, bounds=(0, 100)),
                'b': dict(value=0.1, bounds=(0, 1)),
            }, persist_path=path)
            assert ap2.get('a') == pytest.approx(4.2)
            assert ap2.get('b') == pytest.approx(0.7)
        finally:
            os.unlink(path)

    def test_load_clamps_to_bounds(self):
        with tempfile.NamedTemporaryFile(mode='w', suffix='.json',
                                         delete=False) as f:
            json.dump({'a': 999.0, 'b': -5.0}, f)
            path = f.name
        try:
            ap = _make_params({
                'a': dict(value=5.0, bounds=(1, 10)),
                'b': dict(value=0.5, bounds=(0, 1)),
            }, persist_path=path)
            assert ap.get('a') == 10.0
            assert ap.get('b') == 0.0
        finally:
            os.unlink(path)

    def test_corrupt_file_keeps_seeds(self):
        with tempfile.NamedTemporaryFile(mode='w', suffix='.json',
                                         delete=False) as f:
            f.write("not valid json{{{")
            path = f.name
        try:
            ap = _make_params({
                'a': dict(value=3.0, bounds=(0, 100)),
            }, persist_path=path)
            assert ap.get('a') == 3.0
        finally:
            os.unlink(path)

    def test_missing_file_keeps_seeds(self):
        ap = _make_params({
            'a': dict(value=3.0, bounds=(0, 100)),
        }, persist_path='/tmp/nonexistent_alink_test_xyz.json')
        assert ap.get('a') == 3.0

    def test_auto_persist_on_outcome(self):
        with tempfile.NamedTemporaryFile(mode='w', suffix='.json',
                                         delete=False) as f:
            path = f.name
        try:
            ap = _make_params({
                'a': dict(value=5.0, lr_up=1.0, lr_down=0.5,
                          bounds=(0, 100), increase_on_bad=True),
            }, outcome_window=3, persist_path=path)
            ap.on_profile_changed()
            for _ in range(3):
                ap.tick(0.0)
            with open(path) as f:
                data = json.load(f)
            assert 'a' in data
            assert data['a'] < 5.0
        finally:
            os.unlink(path)

    def test_persists_all_params(self):
        with tempfile.NamedTemporaryFile(mode='w', suffix='.json',
                                         delete=False) as f:
            path = f.name
        try:
            ap = _make_params({
                'x': dict(value=1.0, bounds=(0, 10)),
                'y': dict(value=2.0, bounds=(0, 10)),
                'z': dict(value=3.0, bounds=(0, 10)),
            })
            ap.save(path)
            with open(path) as f:
                data = json.load(f)
            assert data == {'x': 1.0, 'y': 2.0, 'z': 3.0}
        finally:
            os.unlink(path)


# ── ProfileSelector integration tests ─────────────────────────────────────

class TestProfileSelectorIntegration:
    """Test that ProfileSelector wires all 4 adaptive params correctly."""

    def _make_config(self, overrides=None):
        config_str = """
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

[telemetry]
log_enabled = False
log_dir = /var/log/alink
log_rotate_mb = 50
outcome_window_ticks = 10
adapter_id = default
"""
        config = configparser.ConfigParser()
        config.read_string(config_str)
        if overrides:
            for section, kvs in overrides.items():
                for k, v in kvs.items():
                    config.set(section, k, str(v))
        return config

    def _make_selector(self, overrides=None):
        class MockHandshake:
            def get_fps(self): return 60
            def correct_timestamp(self, ts): return ts
        return ProfileSelector(self._make_config(overrides), MockHandshake())

    def test_has_adaptive_params(self):
        ps = self._make_selector()
        assert hasattr(ps, '_adaptive')
        assert isinstance(ps._adaptive, AdaptiveParams)

    def test_all_four_params_registered(self):
        ps = self._make_selector()
        for name in ('snr_safety_margin', 'fec_redundancy_ratio',
                      'utilization_factor', 'hysteresis_up_db'):
            assert name in ps._adaptive.params

    def test_seeds_from_config(self):
        ps = self._make_selector()
        assert ps._adaptive.get('snr_safety_margin') == pytest.approx(3.0)
        assert ps._adaptive.get('fec_redundancy_ratio') == pytest.approx(0.25)
        assert ps._adaptive.get('utilization_factor') == pytest.approx(0.45)
        assert ps._adaptive.get('hysteresis_up_db') == pytest.approx(2.0)

    def test_margin_uses_adaptive_safety_margin(self):
        ps = self._make_selector()
        ps.evaluate_link(best_snr=20, all_packets=1000, lost_packets=0,
                         fec_rec=0, fec_k=8, fec_n=12)
        # Override the adaptive value
        ps._adaptive.params['snr_safety_margin'].value = 5.0
        margin = ps._margin(3)  # MCS 3, threshold 14 dB
        # margin = 20 - 14 - (5.0 + 0*20 + 0*5) = 1.0
        assert margin == pytest.approx(1.0)

    def test_directions_correct(self):
        ps = self._make_selector()
        assert ps._adaptive.params['snr_safety_margin'].increase_on_bad is True
        assert ps._adaptive.params['fec_redundancy_ratio'].increase_on_bad is True
        assert ps._adaptive.params['utilization_factor'].increase_on_bad is False
        assert ps._adaptive.params['hysteresis_up_db'].increase_on_bad is True

    def test_groups_correct(self):
        ps = self._make_selector()
        assert ps._adaptive.params['snr_safety_margin'].group == 'mcs_gate'
        assert ps._adaptive.params['hysteresis_up_db'].group == 'mcs_gate'
        assert ps._adaptive.params['fec_redundancy_ratio'].group == 'profile_compute'
        assert ps._adaptive.params['utilization_factor'].group == 'profile_compute'


# ── Group isolation tests ────────────────────────────────────────────────

class TestGroupIsolation:
    """Test that only the relevant param group is updated per outcome."""

    def _make_grouped_params(self, **kwargs):
        ap = _make_params(params_dict=None, **kwargs)
        ap.add('margin', AdaptiveParam(
            value=5.0, lr_up=1.0, lr_down=0.5,
            bounds=(0, 100), increase_on_bad=True, group='mcs_gate'))
        ap.add('hysteresis', AdaptiveParam(
            value=3.0, lr_up=0.5, lr_down=0.2,
            bounds=(0, 10), increase_on_bad=True, group='mcs_gate'))
        ap.add('fec_ratio', AdaptiveParam(
            value=0.25, lr_up=0.05, lr_down=0.01,
            bounds=(0.1, 0.5), increase_on_bad=True, group='profile_compute'))
        ap.add('util', AdaptiveParam(
            value=0.5, lr_up=0.1, lr_down=0.02,
            bounds=(0.2, 0.9), increase_on_bad=False, group='profile_compute'))
        return ap

    def test_mcs_change_updates_only_mcs_gate(self):
        ap = self._make_grouped_params()
        ap.on_profile_changed(mcs_changed=True)
        for _ in range(5):
            ap.tick(0.0)
        # mcs_gate params should change
        assert ap.get('margin') < 5.0
        assert ap.get('hysteresis') < 3.0
        # profile_compute params should NOT change
        assert ap.get('fec_ratio') == pytest.approx(0.25)
        assert ap.get('util') == pytest.approx(0.5)

    def test_same_mcs_change_updates_only_profile_compute(self):
        ap = self._make_grouped_params()
        ap.on_profile_changed(mcs_changed=False)
        for _ in range(5):
            ap.tick(0.0)
        # profile_compute params should change
        assert ap.get('fec_ratio') < 0.25  # increase_on_bad=True, good → decrease
        assert ap.get('util') > 0.5  # increase_on_bad=False, good → increase
        # mcs_gate params should NOT change
        assert ap.get('margin') == pytest.approx(5.0)
        assert ap.get('hysteresis') == pytest.approx(3.0)

    def test_bad_mcs_outcome_updates_only_mcs_gate(self):
        ap = self._make_grouped_params()
        ap.on_profile_changed(mcs_changed=True)
        ap.tick(0.1)  # loss
        for _ in range(4):
            ap.tick(0.0)
        # mcs_gate params should increase (bad outcome, increase_on_bad=True)
        assert ap.get('margin') > 5.0
        assert ap.get('hysteresis') > 3.0
        # profile_compute params untouched
        assert ap.get('fec_ratio') == pytest.approx(0.25)
        assert ap.get('util') == pytest.approx(0.5)

    def test_bad_same_mcs_outcome_updates_only_profile_compute(self):
        ap = self._make_grouped_params()
        ap.on_profile_changed(mcs_changed=False)
        ap.tick(0.1)  # loss
        for _ in range(4):
            ap.tick(0.0)
        # profile_compute params should change (bad outcome)
        assert ap.get('fec_ratio') > 0.25  # increase_on_bad=True → increase
        assert ap.get('util') < 0.5  # increase_on_bad=False → decrease
        # mcs_gate params untouched
        assert ap.get('margin') == pytest.approx(5.0)
        assert ap.get('hysteresis') == pytest.approx(3.0)

    def test_last_outcome_includes_group(self):
        ap = self._make_grouped_params()
        ap.on_profile_changed(mcs_changed=True)
        for _ in range(5):
            ap.tick(0.0)
        assert ap._last_outcome['group'] == 'mcs_gate'

        ap.on_profile_changed(mcs_changed=False)
        for _ in range(5):
            ap.tick(0.0)
        assert ap._last_outcome['group'] == 'profile_compute'

    def test_ungrouped_param_always_updates(self):
        ap = _make_params()
        ap.add('grouped', AdaptiveParam(
            value=5.0, lr_up=1.0, lr_down=0.5,
            bounds=(0, 100), increase_on_bad=True, group='mcs_gate'))
        ap.add('ungrouped', AdaptiveParam(
            value=5.0, lr_up=1.0, lr_down=0.5,
            bounds=(0, 100), increase_on_bad=True))  # group=None
        # Same-MCS outcome — mcs_gate should NOT update, ungrouped SHOULD
        ap.on_profile_changed(mcs_changed=False)
        for _ in range(5):
            ap.tick(0.0)
        assert ap.get('grouped') == pytest.approx(5.0)  # mcs_gate, skipped
        assert ap.get('ungrouped') < 5.0  # group=None, always updates
