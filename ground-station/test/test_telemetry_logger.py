#!/usr/bin/env python3
"""Unit tests for TelemetryLogger in alink_gs."""
import sys
import os
import json
import tempfile
import shutil
import pytest

# Import module-level constants and classes by executing the script up to __main__
_gs_path = os.path.join(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))), 'ground-station', 'alink_gs')
with open(_gs_path) as _f:
    _code = _f.read().split("if __name__")[0]
exec(_code)


class TestTelemetryLoggerBasic:
    """Test basic logging, file creation, and JSONL format."""

    def setup_method(self):
        self.tmpdir = tempfile.mkdtemp()

    def teardown_method(self):
        shutil.rmtree(self.tmpdir)

    def _make_logger(self, rotate_mb=50, outcome_window=10):
        return TelemetryLogger(self.tmpdir, rotate_mb, outcome_window)

    def _log_tick(self, logger, ts=1000, changed=False, loss_rate=0.0,
                  profile=None):
        if profile is None:
            profile = {'mcs': 3, 'gi': 'long', 'fec_k': 6, 'fec_n': 9,
                       'bitrate': 8000, 'power': 40}
        logger.log(
            timestamp_ms=ts, best_rssi=-50, best_snr=20, min_rssi=-55,
            num_antennas=2, all_packets=1000, lost_packets=0,
            fec_rec_packets=0, fec_k=8, fec_n=12,
            loss_rate=loss_rate, fec_pressure=0.0,
            snr_ema=20.0, snr_slope=0.0,
            margin_current=4.0, margin_target=4.0, emergency=False,
            profile=profile, profile_changed=changed,
            adapter_id='test-adapter',
        )

    def test_creates_log_file(self):
        logger = self._make_logger()
        self._log_tick(logger)
        logger.close()
        assert os.path.exists(os.path.join(self.tmpdir, 'telemetry_0.jsonl'))

    def test_jsonl_format_parseable(self):
        logger = self._make_logger()
        for i in range(5):
            self._log_tick(logger, ts=1000 + i)
        logger.close()

        path = os.path.join(self.tmpdir, 'telemetry_0.jsonl')
        with open(path) as f:
            lines = f.readlines()
        assert len(lines) == 5
        for line in lines:
            record = json.loads(line)
            assert 'ts' in record
            assert 'snr' in record

    def test_record_contains_all_fields(self):
        logger = self._make_logger()
        self._log_tick(logger)
        logger.close()

        path = os.path.join(self.tmpdir, 'telemetry_0.jsonl')
        with open(path) as f:
            record = json.loads(f.readline())

        expected_fields = [
            'ts', 'rssi', 'snr', 'rssi_min', 'ant',
            'pkt_all', 'pkt_lost', 'pkt_fec', 'fec_k', 'fec_n',
            'loss_rate', 'fec_pressure',
            'snr_ema', 'snr_slope', 'margin_cur', 'margin_tgt', 'emergency',
            'changed', 'adapter',
            'mcs', 'gi', 'sel_fec_k', 'sel_fec_n', 'bitrate', 'power',
        ]
        for field in expected_fields:
            assert field in record, f"Missing field: {field}"

    def test_none_profile_omits_profile_fields(self):
        logger = self._make_logger()
        logger.log(
            timestamp_ms=1000, best_rssi=-50, best_snr=20, min_rssi=-55,
            num_antennas=2, all_packets=0, lost_packets=0,
            fec_rec_packets=0, fec_k=None, fec_n=None,
            loss_rate=0.0, fec_pressure=0.0,
            snr_ema=0.0, snr_slope=0.0,
            margin_current=0.0, margin_target=0.0, emergency=False,
            profile=None, profile_changed=False,
        )
        logger.close()

        path = os.path.join(self.tmpdir, 'telemetry_0.jsonl')
        with open(path) as f:
            record = json.loads(f.readline())
        assert 'mcs' not in record


class TestTelemetryLoggerRotation:
    """Test file rotation by size."""

    def setup_method(self):
        self.tmpdir = tempfile.mkdtemp()

    def teardown_method(self):
        shutil.rmtree(self.tmpdir)

    def test_rotates_when_size_exceeded(self):
        # Use tiny rotate size to trigger rotation quickly
        logger = TelemetryLogger(self.tmpdir, rotate_mb=0, outcome_window=10)
        # rotate_mb=0 means rotate_bytes=0, so every write triggers rotation
        profile = {'mcs': 3, 'gi': 'long', 'fec_k': 6, 'fec_n': 9,
                   'bitrate': 8000, 'power': 40}
        for i in range(5):
            logger.log(
                timestamp_ms=1000 + i, best_rssi=-50, best_snr=20,
                min_rssi=-55, num_antennas=2, all_packets=1000,
                lost_packets=0, fec_rec_packets=0, fec_k=8, fec_n=12,
                loss_rate=0.0, fec_pressure=0.0,
                snr_ema=20.0, snr_slope=0.0,
                margin_current=4.0, margin_target=4.0, emergency=False,
                profile=profile, profile_changed=False,
            )
        logger.close()

        # Should have created multiple files
        files = [f for f in os.listdir(self.tmpdir) if f.endswith('.jsonl')]
        assert len(files) > 1


class TestTelemetryLoggerOutcome:
    """Test outcome labeling after profile changes."""

    def setup_method(self):
        self.tmpdir = tempfile.mkdtemp()

    def teardown_method(self):
        shutil.rmtree(self.tmpdir)

    def _read_records(self):
        path = os.path.join(self.tmpdir, 'telemetry_0.jsonl')
        with open(path) as f:
            return [json.loads(line) for line in f]

    def test_good_outcome(self):
        logger = TelemetryLogger(self.tmpdir, outcome_window=5)
        profile = {'mcs': 3, 'gi': 'long', 'fec_k': 6, 'fec_n': 9,
                   'bitrate': 8000, 'power': 40}

        # Profile change tick — reported FEC still old (not yet confirmed)
        logger.log(
            timestamp_ms=1000, best_rssi=-50, best_snr=20, min_rssi=-55,
            num_antennas=2, all_packets=1000, lost_packets=0,
            fec_rec_packets=0, fec_k=8, fec_n=12,
            loss_rate=0.001, fec_pressure=0.0,
            snr_ema=20.0, snr_slope=0.0,
            margin_current=4.0, margin_target=4.0, emergency=False,
            profile=profile, profile_changed=True,
        )

        # 5 follow-up ticks with low loss — FEC now matches profile (confirmed)
        for i in range(5):
            logger.log(
                timestamp_ms=1100 + i * 100, best_rssi=-50, best_snr=20,
                min_rssi=-55, num_antennas=2, all_packets=1000,
                lost_packets=0, fec_rec_packets=0, fec_k=6, fec_n=9,
                loss_rate=0.005, fec_pressure=0.0,
                snr_ema=20.0, snr_slope=0.0,
                margin_current=4.0, margin_target=4.0, emergency=False,
                profile=profile, profile_changed=False,
            )
        logger.close()

        records = self._read_records()
        outcomes = [r for r in records if r.get('type') == 'outcome']
        assert len(outcomes) == 1
        assert outcomes[0]['label'] == 'good'
        assert outcomes[0]['change_ts'] == 1000

    def test_bad_outcome(self):
        logger = TelemetryLogger(self.tmpdir, outcome_window=3)
        profile = {'mcs': 7, 'gi': 'short', 'fec_k': 10, 'fec_n': 12,
                   'bitrate': 25000, 'power': 30}

        # Profile change
        logger.log(
            timestamp_ms=2000, best_rssi=-70, best_snr=15, min_rssi=-75,
            num_antennas=2, all_packets=1000, lost_packets=50,
            fec_rec_packets=10, fec_k=10, fec_n=12,
            loss_rate=0.06, fec_pressure=0.3,
            snr_ema=15.0, snr_slope=-0.5,
            margin_current=-1.0, margin_target=-1.0, emergency=False,
            profile=profile, profile_changed=True,
        )

        # Follow-up ticks with high loss
        for i in range(3):
            logger.log(
                timestamp_ms=2100 + i * 100, best_rssi=-70, best_snr=15,
                min_rssi=-75, num_antennas=2, all_packets=1000,
                lost_packets=80, fec_rec_packets=20, fec_k=10, fec_n=12,
                loss_rate=0.08, fec_pressure=0.5,
                snr_ema=15.0, snr_slope=-0.5,
                margin_current=-2.0, margin_target=-2.0, emergency=True,
                profile=profile, profile_changed=False,
            )
        logger.close()

        records = self._read_records()
        outcomes = [r for r in records if r.get('type') == 'outcome']
        assert len(outcomes) == 1
        assert outcomes[0]['label'] == 'bad'

    def test_marginal_outcome(self):
        logger = TelemetryLogger(self.tmpdir, outcome_window=3)
        profile = {'mcs': 5, 'gi': 'long', 'fec_k': 8, 'fec_n': 12,
                   'bitrate': 15000, 'power': 35}

        # Profile change
        logger.log(
            timestamp_ms=3000, best_rssi=-60, best_snr=18, min_rssi=-65,
            num_antennas=2, all_packets=1000, lost_packets=20,
            fec_rec_packets=5, fec_k=8, fec_n=12,
            loss_rate=0.03, fec_pressure=0.1,
            snr_ema=18.0, snr_slope=0.0,
            margin_current=1.0, margin_target=1.0, emergency=False,
            profile=profile, profile_changed=True,
        )

        # Follow-up with moderate loss (avg > 0.02 but max < 0.05)
        for i in range(3):
            logger.log(
                timestamp_ms=3100 + i * 100, best_rssi=-60, best_snr=18,
                min_rssi=-65, num_antennas=2, all_packets=1000,
                lost_packets=25, fec_rec_packets=8, fec_k=8, fec_n=12,
                loss_rate=0.03, fec_pressure=0.15,
                snr_ema=18.0, snr_slope=0.0,
                margin_current=0.5, margin_target=0.5, emergency=False,
                profile=profile, profile_changed=False,
            )
        logger.close()

        records = self._read_records()
        outcomes = [r for r in records if r.get('type') == 'outcome']
        assert len(outcomes) == 1
        assert outcomes[0]['label'] == 'marginal'

    def test_outcome_finalized_on_next_change(self):
        """Outcome window is finalized early when a new profile change occurs."""
        logger = TelemetryLogger(self.tmpdir, outcome_window=10)
        profile = {'mcs': 3, 'gi': 'long', 'fec_k': 6, 'fec_n': 9,
                   'bitrate': 8000, 'power': 40}

        # First change — reported FEC still old
        logger.log(
            timestamp_ms=1000, best_rssi=-50, best_snr=20, min_rssi=-55,
            num_antennas=2, all_packets=1000, lost_packets=0,
            fec_rec_packets=0, fec_k=8, fec_n=12,
            loss_rate=0.001, fec_pressure=0.0,
            snr_ema=20.0, snr_slope=0.0,
            margin_current=4.0, margin_target=4.0, emergency=False,
            profile=profile, profile_changed=True,
        )

        # 2 follow-up ticks with confirmed FEC (window is 10)
        for i in range(2):
            logger.log(
                timestamp_ms=1100 + i * 100, best_rssi=-50, best_snr=20,
                min_rssi=-55, num_antennas=2, all_packets=1000,
                lost_packets=0, fec_rec_packets=0, fec_k=6, fec_n=9,
                loss_rate=0.005, fec_pressure=0.0,
                snr_ema=20.0, snr_slope=0.0,
                margin_current=4.0, margin_target=4.0, emergency=False,
                profile=profile, profile_changed=False,
            )

        # Second profile change — should finalize first outcome early
        # profile2 FEC matches reported, so confirms immediately
        profile2 = {'mcs': 5, 'gi': 'short', 'fec_k': 8, 'fec_n': 12,
                     'bitrate': 15000, 'power': 35}
        logger.log(
            timestamp_ms=2000, best_rssi=-45, best_snr=25, min_rssi=-50,
            num_antennas=2, all_packets=1000, lost_packets=0,
            fec_rec_packets=0, fec_k=8, fec_n=12,
            loss_rate=0.001, fec_pressure=0.0,
            snr_ema=25.0, snr_slope=0.2,
            margin_current=5.0, margin_target=5.0, emergency=False,
            profile=profile2, profile_changed=True,
        )
        logger.close()

        records = self._read_records()
        outcomes = [r for r in records if r.get('type') == 'outcome']
        # First outcome finalized early (2 confirmed ticks)
        # Second outcome: FEC matches immediately, finalized on close (1 tick)
        assert len(outcomes) == 2
        assert outcomes[0]['change_ts'] == 1000
        assert outcomes[0]['ticks'] == 2


class TestTelemetryLoggerFecConfirmation:
    """Test FEC-confirmed outcome tracking."""

    def setup_method(self):
        self.tmpdir = tempfile.mkdtemp()

    def teardown_method(self):
        shutil.rmtree(self.tmpdir)

    def _read_records(self):
        path = os.path.join(self.tmpdir, 'telemetry_0.jsonl')
        with open(path) as f:
            return [json.loads(line) for line in f]

    def _log_tick(self, logger, ts, fec_k=8, fec_n=12, loss_rate=0.001,
                  profile=None, changed=False):
        if profile is None:
            profile = {'mcs': 3, 'gi': 'long', 'fec_k': 6, 'fec_n': 9,
                       'bitrate': 8000, 'power': 40}
        logger.log(
            timestamp_ms=ts, best_rssi=-50, best_snr=20, min_rssi=-55,
            num_antennas=2, all_packets=1000, lost_packets=0,
            fec_rec_packets=0, fec_k=fec_k, fec_n=fec_n,
            loss_rate=loss_rate, fec_pressure=0.0,
            snr_ema=20.0, snr_slope=0.0,
            margin_current=4.0, margin_target=4.0, emergency=False,
            profile=profile, profile_changed=changed,
        )

    def test_outcome_waits_for_fec_confirmation(self):
        """Outcome window starts only after FEC confirmation, not immediately."""
        logger = TelemetryLogger(self.tmpdir, outcome_window=3, confirm_timeout=5)
        profile = {'mcs': 3, 'gi': 'long', 'fec_k': 6, 'fec_n': 9,
                   'bitrate': 8000, 'power': 40}

        # Profile change tick — reported FEC still old
        self._log_tick(logger, ts=1000, fec_k=8, fec_n=12,
                       profile=profile, changed=True)

        # 2 ticks still showing old FEC (not confirmed yet)
        for i in range(2):
            self._log_tick(logger, ts=1100 + i * 100, fec_k=8, fec_n=12,
                           profile=profile)

        # FEC confirmed — 3 ticks to fill outcome window
        for i in range(3):
            self._log_tick(logger, ts=1300 + i * 100, fec_k=6, fec_n=9,
                           loss_rate=0.001, profile=profile)
        logger.close()

        records = self._read_records()
        outcomes = [r for r in records if r.get('type') == 'outcome']
        assert len(outcomes) == 1
        assert outcomes[0]['label'] == 'good'
        assert outcomes[0]['ticks'] == 3  # only confirmed ticks
        assert outcomes[0]['change_ts'] == 1000

    def test_outcome_discarded_on_confirm_timeout(self):
        """If FEC never matches within timeout, no outcome is recorded."""
        logger = TelemetryLogger(self.tmpdir, outcome_window=3, confirm_timeout=3)
        profile = {'mcs': 3, 'gi': 'long', 'fec_k': 6, 'fec_n': 9,
                   'bitrate': 8000, 'power': 40}

        # Profile change
        self._log_tick(logger, ts=1000, fec_k=8, fec_n=12,
                       profile=profile, changed=True)

        # 5 ticks, FEC never confirms
        for i in range(5):
            self._log_tick(logger, ts=1100 + i * 100, fec_k=8, fec_n=12,
                           profile=profile)
        logger.close()

        records = self._read_records()
        outcomes = [r for r in records if r.get('type') == 'outcome']
        assert len(outcomes) == 0

    def test_immediate_confirmation_when_fec_matches(self):
        """When reported FEC matches profile on change tick, confirm immediately."""
        logger = TelemetryLogger(self.tmpdir, outcome_window=3)
        profile = {'mcs': 5, 'gi': 'long', 'fec_k': 8, 'fec_n': 12,
                   'bitrate': 15000, 'power': 35}

        # Profile FEC matches reported FEC — confirms on same tick
        self._log_tick(logger, ts=1000, fec_k=8, fec_n=12,
                       loss_rate=0.001, profile=profile, changed=True)

        # 2 more ticks to fill window
        for i in range(2):
            self._log_tick(logger, ts=1100 + i * 100, fec_k=8, fec_n=12,
                           loss_rate=0.001, profile=profile)
        logger.close()

        records = self._read_records()
        outcomes = [r for r in records if r.get('type') == 'outcome']
        assert len(outcomes) == 1
        assert outcomes[0]['label'] == 'good'
        assert outcomes[0]['ticks'] == 3


class TestTelemetryLoggerEdgeCases:
    """Test edge cases and robustness."""

    def setup_method(self):
        self.tmpdir = tempfile.mkdtemp()

    def teardown_method(self):
        shutil.rmtree(self.tmpdir)

    def test_close_without_logging(self):
        logger = TelemetryLogger(self.tmpdir)
        logger.close()

    def test_creates_log_dir_if_missing(self):
        nested = os.path.join(self.tmpdir, 'sub', 'dir')
        logger = TelemetryLogger(nested)
        assert os.path.isdir(nested)
        logger.close()

    def test_no_outcome_without_change(self):
        logger = TelemetryLogger(self.tmpdir, outcome_window=3)
        profile = {'mcs': 3, 'gi': 'long', 'fec_k': 6, 'fec_n': 9,
                   'bitrate': 8000, 'power': 40}
        for i in range(10):
            logger.log(
                timestamp_ms=1000 + i * 100, best_rssi=-50, best_snr=20,
                min_rssi=-55, num_antennas=2, all_packets=1000,
                lost_packets=0, fec_rec_packets=0, fec_k=8, fec_n=12,
                loss_rate=0.0, fec_pressure=0.0,
                snr_ema=20.0, snr_slope=0.0,
                margin_current=4.0, margin_target=4.0, emergency=False,
                profile=profile, profile_changed=False,
            )
        logger.close()

        path = os.path.join(self.tmpdir, 'telemetry_0.jsonl')
        with open(path) as f:
            records = [json.loads(line) for line in f]
        outcomes = [r for r in records if r.get('type') == 'outcome']
        assert len(outcomes) == 0


class TestTelemetryLoggerNewSession:
    """Test session-based log rotation triggered by drone restarts."""

    def setup_method(self):
        self.tmpdir = tempfile.mkdtemp()

    def teardown_method(self):
        shutil.rmtree(self.tmpdir)

    def _log_tick(self, logger, ts=1000, changed=False, loss_rate=0.0,
                  profile=None, fec_k=8, fec_n=12):
        if profile is None:
            profile = {'mcs': 3, 'gi': 'long', 'fec_k': 6, 'fec_n': 9,
                       'bitrate': 8000, 'power': 40}
        logger.log(
            timestamp_ms=ts, best_rssi=-50, best_snr=20, min_rssi=-55,
            num_antennas=2, all_packets=1000, lost_packets=0,
            fec_rec_packets=0, fec_k=fec_k, fec_n=fec_n,
            loss_rate=loss_rate, fec_pressure=0.0,
            snr_ema=20.0, snr_slope=0.0,
            margin_current=4.0, margin_target=4.0, emergency=False,
            profile=profile, profile_changed=changed,
        )

    def test_new_session_rotates_file(self):
        logger = TelemetryLogger(self.tmpdir, rotate_mb=50, outcome_window=10)
        self._log_tick(logger, ts=1000)
        logger.new_session()
        self._log_tick(logger, ts=2000)
        logger.close()

        assert os.path.exists(os.path.join(self.tmpdir, 'telemetry_0.jsonl'))
        assert os.path.exists(os.path.join(self.tmpdir, 'telemetry_1.jsonl'))

        with open(os.path.join(self.tmpdir, 'telemetry_0.jsonl')) as f:
            records_0 = [json.loads(line) for line in f]
        with open(os.path.join(self.tmpdir, 'telemetry_1.jsonl')) as f:
            records_1 = [json.loads(line) for line in f]

        assert len(records_0) == 1
        assert records_0[0]['ts'] == 1000
        assert len(records_1) == 1
        assert records_1[0]['ts'] == 2000

    def test_multiple_sessions(self):
        logger = TelemetryLogger(self.tmpdir, rotate_mb=50, outcome_window=10)
        self._log_tick(logger, ts=1000)
        logger.new_session()
        self._log_tick(logger, ts=2000)
        logger.new_session()
        self._log_tick(logger, ts=3000)
        logger.close()

        for i in range(3):
            assert os.path.exists(
                os.path.join(self.tmpdir, f'telemetry_{i}.jsonl'))

    def test_new_session_finalizes_pending_outcome(self):
        """Outcome tracking window should be finalized into the old file
        before the session rotation."""
        profile = {'mcs': 5, 'gi': 'short', 'fec_k': 8, 'fec_n': 12,
                   'bitrate': 12000, 'power': 30}
        logger = TelemetryLogger(self.tmpdir, rotate_mb=50, outcome_window=10)

        # Trigger a profile change and let FEC confirm immediately
        self._log_tick(logger, ts=1000, changed=True, profile=profile,
                       fec_k=8, fec_n=12)
        # Feed a few outcome ticks
        for i in range(3):
            self._log_tick(logger, ts=1100 + i * 100, loss_rate=0.01,
                           profile=profile, fec_k=8, fec_n=12)

        # Rotate — should finalize the partial outcome into telemetry_0
        logger.new_session()
        logger.close()

        with open(os.path.join(self.tmpdir, 'telemetry_0.jsonl')) as f:
            records = [json.loads(line) for line in f]
        outcomes = [r for r in records if r.get('type') == 'outcome']
        assert len(outcomes) == 1
        assert outcomes[0]['label'] == 'good'
