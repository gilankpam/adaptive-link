"""Deterministic replay simulator for evaluating ProfileSelector parameter sets.

Replays historical telemetry data through ProfileSelector with candidate parameters,
using a counterfactual SNR-based link model to estimate what would have happened
under different parameter choices. RF metrics (SNR, RSSI) are independent of TX
parameters and serve as valid ground truth for counterfactual evaluation.
"""

import configparser
import math
import sys
import os

# Add parent directory so we can import from alink_gs
sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..'))

# Load ProfileSelector and constants from alink_gs by exec'ing everything
# before __main__. This avoids running the script's main loop.
_gs_path = os.path.join(os.path.dirname(__file__), '..', 'alink_gs')
with open(_gs_path) as _f:
    _code = _f.read().split("if __name__")[0]
_ns = {}
exec(_code, _ns)
_ProfileSelector = _ns['ProfileSelector']
MCS_SNR_THRESHOLDS = _ns['MCS_SNR_THRESHOLDS']


def load_config_from_file(config_path):
    """Read an INI config file and return its contents as a string."""
    with open(config_path) as f:
        return f.read()


class LinkModel:
    """Estimates packet loss rate from SNR and chosen MCS level.

    Uses a sigmoid function centered on MCS_SNR_THRESHOLDS to model the
    802.11n packet error rate cliff. Near-zero loss when SNR is well above
    threshold, sharp increase below.
    """

    def __init__(self, steepness=1.5):
        """Initialize with configurable steepness for the sigmoid curve.

        Args:
            steepness: Controls how sharp the loss cliff is. Higher values
                       make the transition sharper. Default 1.5 is conservative.
        """
        self.steepness = steepness

    def estimate_loss(self, snr, mcs, bandwidth=20):
        """Estimate packet loss rate for given SNR and MCS.

        Args:
            snr: Signal-to-noise ratio in dB.
            mcs: Modulation and coding scheme index (0-7).
            bandwidth: Channel bandwidth (20 or 40 MHz). Not currently used
                       but reserved for future bandwidth-dependent models.

        Returns:
            Estimated loss rate in [0.0, 1.0].
        """
        mcs = max(0, min(7, mcs))
        threshold = MCS_SNR_THRESHOLDS[mcs]
        # Sigmoid: loss = 1 / (1 + exp(steepness * (snr - threshold)))
        # When snr >> threshold: loss -> 0
        # When snr << threshold: loss -> 1
        x = self.steepness * (snr - threshold)
        # Clamp to avoid overflow
        x = max(-20, min(20, x))
        loss = 1.0 / (1.0 + math.exp(x))
        return loss

    def estimate_fec_recoveries(self, loss_rate, fec_k, fec_n, packet_count):
        """Estimate how many packets FEC would recover.

        Args:
            loss_rate: Estimated packet loss rate (0-1).
            fec_k: FEC data packets per block.
            fec_n: FEC total packets per block (data + parity).
            packet_count: Total packets in the interval.

        Returns:
            Estimated number of FEC-recovered packets.
        """
        if fec_n <= 0 or fec_k <= 0 or fec_n <= fec_k:
            return 0
        redundancy = fec_n - fec_k
        num_blocks = max(1, packet_count // fec_n)
        lost_per_block = loss_rate * fec_n
        # FEC can recover up to (fec_n - fec_k) losses per block
        recovered_per_block = min(lost_per_block, redundancy)
        return int(recovered_per_block * num_blocks)


class ReplayResult:
    """Aggregate fitness metrics from a replay run."""

    def __init__(self):
        self.total_ticks = 0
        self.mean_bitrate = 0.0
        self.mean_loss = 0.0
        self.transition_count = 0
        self.crash_events = 0
        self.mcs_distribution = {}
        self.total_fitness = 0.0

    @property
    def transition_rate(self):
        """Profile changes per second (assuming 10Hz tick rate)."""
        if self.total_ticks == 0:
            return 0.0
        duration_s = self.total_ticks / 10.0
        return self.transition_count / max(duration_s, 0.1)

    @property
    def stability_score(self):
        """Stability metric: 1.0 = no transitions, 0.0 = 1+ transitions/sec."""
        return 1.0 - min(self.transition_rate / 1.0, 1.0)


class ReplaySimulator:
    """Deterministic replay of telemetry data through ProfileSelector.

    Feeds recorded RF metrics (SNR, RSSI) through ProfileSelector with
    candidate parameters. Uses LinkModel for counterfactual packet metrics.
    Time injection ensures hold timers behave based on telemetry timestamps.
    """

    # Fitness weights
    W_THROUGHPUT = 0.5
    W_RELIABILITY = 0.4
    W_STABILITY = 0.2
    W_CRASH = 5.0
    CRASH_LOSS_THRESHOLD = 0.5

    def __init__(self, ticks_df, config, link_model=None):
        """Initialize replay simulator.

        Args:
            ticks_df: DataFrame of telemetry tick records (from load_telemetry).
            config: configparser.ConfigParser with candidate parameters.
            link_model: Optional LinkModel instance. Uses default if not provided.
        """
        self.ticks = ticks_df
        self.link_model = link_model or LinkModel()

        # Create ProfileSelector
        self.selector = _ProfileSelector(config)

        # Time injection state
        self._current_tick_ts = 0

        # Override _now_ms to return tick timestamp
        self.selector._now_ms = lambda: self._current_tick_ts

    def run(self):
        """Execute deterministic replay over all ticks.

        Returns:
            ReplayResult with aggregate fitness metrics.
        """
        result = ReplayResult()

        if len(self.ticks) == 0:
            return result

        # Sort by timestamp for deterministic order
        ticks = self.ticks.sort_values('ts').reset_index(drop=True)
        result.total_ticks = len(ticks)

        # Synthetic cumulative counters
        cum_all_packets = 0
        cum_lost_packets = 0
        cum_fec_rec = 0

        # Per-tick accumulators
        total_bitrate = 0.0
        total_loss = 0.0
        prev_mcs = 0
        packets_per_tick = 100  # Assumed packet rate per tick

        for i, tick in ticks.iterrows():
            # Inject time from tick timestamp
            self._current_tick_ts = int(tick['ts'])

            # RF metrics — used as-is (independent of TX choices)
            best_rssi = float(tick.get('rssi', -50))
            best_snr = float(tick.get('snr', 20))
            min_rssi = float(tick.get('rssi_min', best_rssi))
            num_antennas = int(tick.get('ant', 1))

            # Counterfactual packet metrics using LinkModel
            # Use the MCS that was selected in the PREVIOUS tick
            estimated_loss = self.link_model.estimate_loss(best_snr, prev_mcs)

            # FEC parameters from current selector state
            current_profile = self.selector.current_profile
            if current_profile and 'fec_k' in current_profile:
                fec_k = current_profile['fec_k']
                fec_n = current_profile['fec_n']
            else:
                fec_k = 8
                fec_n = 12

            # Estimate packet counts
            lost_this_tick = int(estimated_loss * packets_per_tick)
            fec_recovered = self.link_model.estimate_fec_recoveries(
                estimated_loss, fec_k, fec_n, packets_per_tick)

            # Update cumulative counters
            cum_all_packets += packets_per_tick
            cum_lost_packets += lost_this_tick
            cum_fec_rec += fec_recovered

            # Feed through ProfileSelector
            raw_score = self.selector.compute_score(
                best_rssi, best_snr, min_rssi,
                cum_all_packets, cum_lost_packets, cum_fec_rec,
                fec_k, fec_n, num_antennas)

            prev_mcs_before_select = self.selector.current_profile_idx
            changed, new_idx, new_profile = self.selector.select(raw_score)

            # Stability tracks MCS transitions only — parameter-only changes
            # (FEC, bitrate, GI, power at same MCS) are cheap fine-tuning and
            # should not count against stability.
            if new_idx != prev_mcs_before_select and prev_mcs_before_select >= 0:
                result.transition_count += 1

            # Get current MCS for next tick's loss estimation
            if new_profile and 'mcs' in new_profile:
                prev_mcs = new_profile['mcs']

            # Get bitrate for throughput scoring
            bitrate = 0
            if new_profile and 'bitrate' in new_profile:
                bitrate = new_profile['bitrate']
            total_bitrate += bitrate

            # Track loss and crash events
            total_loss += estimated_loss
            if estimated_loss > self.CRASH_LOSS_THRESHOLD:
                result.crash_events += 1

            # MCS distribution
            mcs_key = prev_mcs
            result.mcs_distribution[mcs_key] = result.mcs_distribution.get(mcs_key, 0) + 1

        # Compute aggregate metrics
        n = result.total_ticks
        result.mean_bitrate = total_bitrate / n if n > 0 else 0
        result.mean_loss = total_loss / n if n > 0 else 0

        # Compute fitness
        max_bitrate = 30000  # Reference max bitrate for normalization
        throughput_score = min(result.mean_bitrate / max_bitrate, 1.0)
        reliability_score = 1.0 - result.mean_loss
        stability_score = result.stability_score
        crash_rate = result.crash_events / n if n > 0 else 0

        result.total_fitness = (
            self.W_THROUGHPUT * throughput_score +
            self.W_RELIABILITY * reliability_score +
            self.W_STABILITY * stability_score -
            self.W_CRASH * crash_rate
        )

        return result
