#!/usr/bin/env python3
"""Unit tests for TelemetryLogger in alink_gs."""
import sys
import os
import json
import tempfile
import shutil
import unittest

# Import module-level constants and classes by executing the script up to __main__
_gs_path = os.path.join(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))), 'ground-station', 'alink_gs')
with open(_gs_path) as _f:
    _code = _f.read().split("if __name__")[0]
exec(_code)


class TestTelemetryLoggerBasic(unittest.TestCase):
    """Test basic logging, file creation, and JSONL format."""

    def setUp(self):
        self.tmpdir = tempfile.mkdtemp()

    def tearDown(self):
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
            rf_score=0.8, loss_score=1.0, fec_score=1.0,
            diversity_score=0.95, combined_score=1800.0,
            ema_fast=1800.0, ema_slow=1790.0, snr_ema=20.0,
            profile=profile, profile_changed=changed,
            adapter_id='test-adapter',
        )

    def test_creates_log_file(self):
        logger = self._make_logger()
        self._log_tick(logger)
        logger.close()
        self.assertTrue(os.path.exists(os.path.join(self.tmpdir, 'telemetry_0.jsonl')))

    def test_jsonl_format_parseable(self):
        logger = self._make_logger()
        for i in range(5):
            self._log_tick(logger, ts=1000 + i)
        logger.close()

        path = os.path.join(self.tmpdir, 'telemetry_0.jsonl')
        with open(path) as f:
            lines = f.readlines()
        self.assertEqual(len(lines), 5)
        for line in lines:
            record = json.loads(line)
            self.assertIn('ts', record)
            self.assertIn('snr', record)

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
            'loss_rate', 'fec_pressure', 'rf_score', 'loss_score',
            'fec_score', 'div_score', 'score', 'ema_fast', 'ema_slow',
            'snr_ema', 'changed', 'adapter',
            'mcs', 'gi', 'sel_fec_k', 'sel_fec_n', 'bitrate', 'power',
        ]
        for field in expected_fields:
            self.assertIn(field, record, f"Missing field: {field}")

    def test_none_profile_omits_profile_fields(self):
        logger = self._make_logger()
        logger.log(
            timestamp_ms=1000, best_rssi=-50, best_snr=20, min_rssi=-55,
            num_antennas=2, all_packets=0, lost_packets=0,
            fec_rec_packets=0, fec_k=None, fec_n=None,
            loss_rate=0.0, fec_pressure=0.0,
            rf_score=0.0, loss_score=1.0, fec_score=1.0,
            diversity_score=1.0, combined_score=1000.0,
            ema_fast=1000.0, ema_slow=1000.0, snr_ema=0.0,
            profile=None, profile_changed=False,
        )
        logger.close()

        path = os.path.join(self.tmpdir, 'telemetry_0.jsonl')
        with open(path) as f:
            record = json.loads(f.readline())
        self.assertNotIn('mcs', record)


class TestTelemetryLoggerRotation(unittest.TestCase):
    """Test file rotation by size."""

    def setUp(self):
        self.tmpdir = tempfile.mkdtemp()

    def tearDown(self):
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
                rf_score=0.8, loss_score=1.0, fec_score=1.0,
                diversity_score=0.95, combined_score=1800.0,
                ema_fast=1800.0, ema_slow=1790.0, snr_ema=20.0,
                profile=profile, profile_changed=False,
            )
        logger.close()

        # Should have created multiple files
        files = [f for f in os.listdir(self.tmpdir) if f.endswith('.jsonl')]
        self.assertGreater(len(files), 1)


class TestTelemetryLoggerOutcome(unittest.TestCase):
    """Test outcome labeling after profile changes."""

    def setUp(self):
        self.tmpdir = tempfile.mkdtemp()

    def tearDown(self):
        shutil.rmtree(self.tmpdir)

    def _read_records(self):
        path = os.path.join(self.tmpdir, 'telemetry_0.jsonl')
        with open(path) as f:
            return [json.loads(line) for line in f]

    def test_good_outcome(self):
        logger = TelemetryLogger(self.tmpdir, outcome_window=5)
        profile = {'mcs': 3, 'gi': 'long', 'fec_k': 6, 'fec_n': 9,
                   'bitrate': 8000, 'power': 40}

        # Profile change tick
        logger.log(
            timestamp_ms=1000, best_rssi=-50, best_snr=20, min_rssi=-55,
            num_antennas=2, all_packets=1000, lost_packets=0,
            fec_rec_packets=0, fec_k=8, fec_n=12,
            loss_rate=0.001, fec_pressure=0.0,
            rf_score=0.8, loss_score=1.0, fec_score=1.0,
            diversity_score=0.95, combined_score=1800.0,
            ema_fast=1800.0, ema_slow=1790.0, snr_ema=20.0,
            profile=profile, profile_changed=True,
        )

        # 5 follow-up ticks with low loss
        for i in range(5):
            logger.log(
                timestamp_ms=1100 + i * 100, best_rssi=-50, best_snr=20,
                min_rssi=-55, num_antennas=2, all_packets=1000,
                lost_packets=0, fec_rec_packets=0, fec_k=8, fec_n=12,
                loss_rate=0.005, fec_pressure=0.0,
                rf_score=0.8, loss_score=1.0, fec_score=1.0,
                diversity_score=0.95, combined_score=1800.0,
                ema_fast=1800.0, ema_slow=1790.0, snr_ema=20.0,
                profile=profile, profile_changed=False,
            )
        logger.close()

        records = self._read_records()
        outcomes = [r for r in records if r.get('type') == 'outcome']
        self.assertEqual(len(outcomes), 1)
        self.assertEqual(outcomes[0]['label'], 'good')
        self.assertEqual(outcomes[0]['change_ts'], 1000)

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
            rf_score=0.3, loss_score=0.4, fec_score=0.7,
            diversity_score=0.75, combined_score=1300.0,
            ema_fast=1300.0, ema_slow=1400.0, snr_ema=15.0,
            profile=profile, profile_changed=True,
        )

        # Follow-up ticks with high loss
        for i in range(3):
            logger.log(
                timestamp_ms=2100 + i * 100, best_rssi=-70, best_snr=15,
                min_rssi=-75, num_antennas=2, all_packets=1000,
                lost_packets=80, fec_rec_packets=20, fec_k=10, fec_n=12,
                loss_rate=0.08, fec_pressure=0.5,
                rf_score=0.3, loss_score=0.2, fec_score=0.5,
                diversity_score=0.75, combined_score=1200.0,
                ema_fast=1200.0, ema_slow=1350.0, snr_ema=15.0,
                profile=profile, profile_changed=False,
            )
        logger.close()

        records = self._read_records()
        outcomes = [r for r in records if r.get('type') == 'outcome']
        self.assertEqual(len(outcomes), 1)
        self.assertEqual(outcomes[0]['label'], 'bad')

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
            rf_score=0.5, loss_score=0.7, fec_score=0.9,
            diversity_score=0.85, combined_score=1550.0,
            ema_fast=1550.0, ema_slow=1600.0, snr_ema=18.0,
            profile=profile, profile_changed=True,
        )

        # Follow-up with moderate loss (avg > 0.02 but max < 0.05)
        for i in range(3):
            logger.log(
                timestamp_ms=3100 + i * 100, best_rssi=-60, best_snr=18,
                min_rssi=-65, num_antennas=2, all_packets=1000,
                lost_packets=25, fec_rec_packets=8, fec_k=8, fec_n=12,
                loss_rate=0.03, fec_pressure=0.15,
                rf_score=0.5, loss_score=0.7, fec_score=0.85,
                diversity_score=0.85, combined_score=1530.0,
                ema_fast=1530.0, ema_slow=1580.0, snr_ema=18.0,
                profile=profile, profile_changed=False,
            )
        logger.close()

        records = self._read_records()
        outcomes = [r for r in records if r.get('type') == 'outcome']
        self.assertEqual(len(outcomes), 1)
        self.assertEqual(outcomes[0]['label'], 'marginal')

    def test_outcome_finalized_on_next_change(self):
        """Outcome window is finalized early when a new profile change occurs."""
        logger = TelemetryLogger(self.tmpdir, outcome_window=10)
        profile = {'mcs': 3, 'gi': 'long', 'fec_k': 6, 'fec_n': 9,
                   'bitrate': 8000, 'power': 40}

        # First change
        logger.log(
            timestamp_ms=1000, best_rssi=-50, best_snr=20, min_rssi=-55,
            num_antennas=2, all_packets=1000, lost_packets=0,
            fec_rec_packets=0, fec_k=8, fec_n=12,
            loss_rate=0.001, fec_pressure=0.0,
            rf_score=0.8, loss_score=1.0, fec_score=1.0,
            diversity_score=0.95, combined_score=1800.0,
            ema_fast=1800.0, ema_slow=1790.0, snr_ema=20.0,
            profile=profile, profile_changed=True,
        )

        # Only 2 follow-up ticks (window is 10)
        for i in range(2):
            logger.log(
                timestamp_ms=1100 + i * 100, best_rssi=-50, best_snr=20,
                min_rssi=-55, num_antennas=2, all_packets=1000,
                lost_packets=0, fec_rec_packets=0, fec_k=8, fec_n=12,
                loss_rate=0.005, fec_pressure=0.0,
                rf_score=0.8, loss_score=1.0, fec_score=1.0,
                diversity_score=0.95, combined_score=1800.0,
                ema_fast=1800.0, ema_slow=1790.0, snr_ema=20.0,
                profile=profile, profile_changed=False,
            )

        # Second profile change — should finalize first outcome early
        profile2 = {'mcs': 5, 'gi': 'short', 'fec_k': 8, 'fec_n': 12,
                     'bitrate': 15000, 'power': 35}
        logger.log(
            timestamp_ms=2000, best_rssi=-45, best_snr=25, min_rssi=-50,
            num_antennas=2, all_packets=1000, lost_packets=0,
            fec_rec_packets=0, fec_k=8, fec_n=12,
            loss_rate=0.001, fec_pressure=0.0,
            rf_score=0.9, loss_score=1.0, fec_score=1.0,
            diversity_score=0.95, combined_score=1900.0,
            ema_fast=1900.0, ema_slow=1850.0, snr_ema=25.0,
            profile=profile2, profile_changed=True,
        )
        logger.close()

        records = self._read_records()
        outcomes = [r for r in records if r.get('type') == 'outcome']
        # First outcome finalized early (3 ticks: change + 2 follow-up)
        # Second outcome finalized on close (1 tick: just the change)
        self.assertEqual(len(outcomes), 2)
        self.assertEqual(outcomes[0]['change_ts'], 1000)
        self.assertEqual(outcomes[0]['ticks'], 3)


class TestTelemetryLoggerEdgeCases(unittest.TestCase):
    """Test edge cases and robustness."""

    def setUp(self):
        self.tmpdir = tempfile.mkdtemp()

    def tearDown(self):
        shutil.rmtree(self.tmpdir)

    def test_close_without_logging(self):
        logger = TelemetryLogger(self.tmpdir)
        logger.close()

    def test_creates_log_dir_if_missing(self):
        nested = os.path.join(self.tmpdir, 'sub', 'dir')
        logger = TelemetryLogger(nested)
        self.assertTrue(os.path.isdir(nested))
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
                rf_score=0.8, loss_score=1.0, fec_score=1.0,
                diversity_score=0.95, combined_score=1800.0,
                ema_fast=1800.0, ema_slow=1790.0, snr_ema=20.0,
                profile=profile, profile_changed=False,
            )
        logger.close()

        path = os.path.join(self.tmpdir, 'telemetry_0.jsonl')
        with open(path) as f:
            records = [json.loads(line) for line in f]
        outcomes = [r for r in records if r.get('type') == 'outcome']
        self.assertEqual(len(outcomes), 0)


if __name__ == '__main__':
    unittest.main()
