# Ground Station Configuration Reference

This document describes every parameter in `alink_gs.conf`, the ground station configuration file deployed to `/etc/alink_gs.conf`.

---

## [outgoing] — Drone Connection

Where to send profile messages to the drone.

| Parameter | Default | Description |
|-----------|---------|-------------|
| `udp_ip` | 10.5.0.10 | IP address of the drone |
| `udp_port` | 9999 | UDP port the drone listens on |

---

## [wfb-ng] — Signal Metrics Source

Where to read link quality data from wfb-ng.

| Parameter | Default | Description |
|-----------|---------|-------------|
| `host` | 127.0.0.1 | Host running the wfb-ng statistics server |
| `port` | 8103 | Port of the wfb-ng statistics server |

---

## [keyframe] — Video Keyframe Requests

Controls how the GS requests keyframes (IDR frames) from the camera after packet loss. A keyframe lets the video decoder recover from corruption.

| Parameter | Default | Description |
|-----------|---------|-------------|
| `allow_idr` | True | Enable keyframe requests on packet loss |
| `idr_max_messages` | 4 | How many keyframe request messages to send per loss event |
| `idr_send_interval_ms` | 20 | Delay between consecutive keyframe request messages (ms) |

---

## [handshake] — Drone Handshake Timing

The GS periodically pings the drone to learn its video settings (FPS, resolution) and measure round-trip time.

| Parameter | Default | Description |
|-----------|---------|-------------|
| `resync_interval_s` | 30 | Seconds between handshake pings during normal operation |
| `video_fps_default` | 90 | Assumed video FPS before the first handshake completes |
| `fast_retry_ms` | 500 | If a ping gets no reply, retry after this many ms |
| `max_fast_retries` | 3 | How many fast retries before giving up and waiting for the next normal ping |

`fast_retry_ms` and `max_fast_retries` have sensible defaults and can be omitted from the config file.

---

## [hardware] — Adapter and Encoder Limits

Static limits that depend on your WiFi adapter and video encoder, not on flight conditions. Set these once per hardware setup.

| Parameter | Default | Range | Description |
|-----------|---------|-------|-------------|
| `bandwidth` | 20 | 20 or 40 | Channel bandwidth in MHz |
| `gop` | 10 | — | Keyframe interval — how many frames between full video frames. Lower values recover faster from corruption but use more bandwidth |
| `max_bitrate` | 24000 | 15000–40000 | Maximum video bitrate the encoder should produce (kbps) |
| `min_bitrate` | 1000 | 1000–5000 | Minimum video bitrate — floor to prevent unwatchable quality (kbps) |
| `max_power` | 3000 | 1000–2900 | TX power at the lowest link speed (strongest signal) |
| `min_power` | 200 | 50–1000 | TX power at the highest link speed (weakest signal) |
| `max_fec_n` | 30 | — | Maximum FEC block size. Larger blocks tolerate more burst loss but add latency |
| `max_fec_redundancy` | 0.5 | 0.0–1.0 | Maximum fraction of a FEC block that can be redundancy. Caps overhead even under heavy loss |

---

## [gate] — Link Quality Gate

The gate decides **when** to change link speed (MCS). It watches signal quality and decides whether to upgrade, downgrade, or hold steady.

### SNR Smoothing

Raw signal readings are noisy. These parameters smooth them before making decisions.

| Parameter | Default | Range | Description |
|-----------|---------|-------|-------------|
| `snr_ema_alpha` | 0.3 | 0.05–0.8 | How quickly the smoothed SNR reacts to changes. Higher = more reactive, noisier. Lower = more stable, slower to respond |
| `snr_slope_alpha` | 0.3 | 0.05–0.8 | Same idea, but for the SNR *trend* (is signal getting better or worse?). Higher = trusts recent trend more |
| `snr_predict_horizon_ticks` | 3 | 0–10 | How far ahead to project the SNR trend when deciding upgrades. Higher = more cautious about upgrading into a falling signal |

### Safety Margin

The system adds a safety buffer on top of the minimum SNR required for each link speed. This buffer grows when the link is stressed (high loss or FEC pressure).

| Parameter | Default | Range | Description |
|-----------|---------|-------|-------------|
| `snr_safety_margin` | 3 | 1.0–8.0 | Base safety buffer in dB. Higher = more conservative, stays at lower speeds longer |
| `loss_margin_weight` | 20 | 5.0–50.0 | How much packet loss widens the safety buffer. At 10% loss, this adds 2 dB (0.10 x 20) |
| `fec_margin_weight` | 5 | 1.0–15.0 | How much FEC pressure widens the safety buffer. At 50% FEC pressure, this adds 2.5 dB (0.50 x 5) |

### Hysteresis (Upgrade/Downgrade Thresholds)

Prevents the system from rapidly switching back and forth between speeds.

| Parameter | Default | Range | Description |
|-----------|---------|-------|-------------|
| `hysteresis_up_db` | 2.5 | 0.5–6.0 | Extra dB of headroom required above a speed's threshold before upgrading to it. Higher = harder to upgrade |
| `hysteresis_down_db` | 1.0 | 0.0–4.0 | How far below a speed's threshold before downgrading away from it. Higher = slower to downgrade |

### Emergency Triggers

When things go really wrong, these force an immediate downgrade without waiting for normal gating.

| Parameter | Default | Range | Description |
|-----------|---------|-------|-------------|
| `emergency_loss_rate` | 0.15 | 0.05–0.35 | Packet loss rate (0–1) that triggers an emergency downgrade. At 0.15, losing 15% of packets forces a speed drop |
| `emergency_fec_pressure` | 0.75 | 0.4–0.95 | FEC recovery pressure (0–1) that triggers an emergency downgrade. At 0.75, the error correction system is 75% saturated |

### MCS Constraints

| Parameter | Default | Range | Description |
|-----------|---------|-------|-------------|
| `max_mcs` | 5 | 0–7 | Maximum link speed index allowed. Caps the fastest speed even if signal is strong enough for higher |
| `max_mcs_step_up` | 1 | 0–3 | Maximum speed steps per single upgrade. At 1, the system climbs one step at a time. Set to 0 to allow unlimited jumps |

---

## [profile selection] — Timing and Pacing

Controls **how fast** the system reacts to link changes. These are timing gates that prevent changes from happening too quickly.

| Parameter | Default | Range | Description |
|-----------|---------|-------|-------------|
| `hold_fallback_mode_ms` | 1000 | 200–5000 | After recovering from the lowest speed (fallback), wait this long before upgrading (ms) |
| `hold_modes_down_ms` | 3000 | 500–10000 | After any speed change, wait this long before allowing the next upgrade (ms). Does not block downgrades |
| `min_between_changes_ms` | 200 | 50–1000 | Minimum time between any profile updates, including parameter-only changes (ms). Emergency downgrades bypass this |
| `fast_downgrade` | True | True / False | When True, non-emergency downgrades apply immediately. When False, they wait for `hold_modes_down_ms` |
| `upward_confidence_loops` | 3 | 1–10 | How many consecutive ticks the system must want to upgrade before actually doing it. Prevents upgrades on brief signal spikes |

---

## [dynamic] — Profile Computation Tuning

Controls **what values** the system computes for each profile (guard interval, FEC, bitrate).

### Guard Interval

Short guard interval gives ~11% more throughput but is less tolerant of interference.

| Parameter | Default | Range | Description |
|-----------|---------|-------|-------------|
| `short_gi_snr_margin` | 5 | 2.0–10.0 | Extra dB of SNR above the current speed's threshold required to use short guard interval |
| `short_gi_max_loss` | 0.02 | 0.005–0.10 | Maximum loss rate to allow short guard interval. Above this, falls back to long GI |
| `short_gi_max_fec_pressure` | 0.3 | 0.1–0.8 | Maximum FEC pressure to allow short guard interval |

### FEC (Forward Error Correction)

FEC adds redundant data so the receiver can recover from lost packets without retransmission.

| Parameter | Default | Range | Description |
|-----------|---------|-------|-------------|
| `fec_redundancy_ratio` | 0.25 | 0.15–0.40 | Target fraction of each FEC block used for redundancy. At 0.25, one quarter of transmitted data is error correction overhead |
| `loss_threshold_for_fec_downgrade` | 0.10 | 0.01–0.15 | When loss exceeds this rate, the system increases FEC protection by shrinking the data portion of each block |

### Bitrate

| Parameter | Default | Range | Description |
|-----------|---------|-------|-------------|
| `utilization_factor` | 0.8 | 0.2–0.7 | Fraction of the raw air data rate used for video. The rest is overhead (frame headers, control frames, etc.). Lower = more conservative bitrate estimates |

---

## [telemetry] — ML Training Data Logging

Optional logging of per-tick link metrics for offline analysis and parameter optimization.

| Parameter | Default | Description |
|-----------|---------|-------------|
| `log_enabled` | True | Enable or disable telemetry logging |
| `log_dir` | /var/log/alink | Directory to write log files |
| `log_rotate_mb` | 50 | Rotate to a new log file after reaching this size in MB |
| `outcome_window_ticks` | 10 | After a profile change, observe link quality for this many ticks to label the change as good, marginal, or bad |
| `adapter_id` | default | Identifier for this adapter. Used to separate logs when running multiple adapters |

---

## [optimizer] — Parameter Optimization Metadata

Used only by the offline optimizer tool (`ground-station/ml/optimize_params.py`), not by the GS itself.

| Parameter | Default | Description |
|-----------|---------|-------------|
| `skip_optimize_params` | (empty) | Comma-separated list of parameter names to freeze during optimization. Useful for locking hardware-specific values while tuning algorithm parameters |
