# OpenIPC Adaptive-Link

Adaptive wireless link profile selector for OpenIPC FPV drone systems. Dynamically adjusts video bitrate, MCS, FEC, TX power, and other transmission parameters based on real-time signal quality from the ground station.

## Quick Start

### Drone Installation

```bash
# Build the drone binary
make clean && make ssc338q
# Copy to drone
scp -O drone/alink_drone root@<drone-ip>:/usr/bin/alink_drone
# Copy the config
scp -O config/alink.conf root@<drone-ip>:/etc/alink.conf
# Restart the drone
reboot
```

### Ground Station Installation

```bash
scp ground-station/alink_gs root@<gs-ip>:/usr/bin/alink_gs
scp config/alink_gs.conf root@<gs-ip>:/etc/alink_gs.conf
# Restart GS
reboot
```

## How It Works

A wireless link can handle higher data-rates over short distances with strong signal than long distances with weak signal. Adaptive-Link dynamically selects transmission profiles based on real-time link quality.

- **Ground Station** (`alink_gs`): Monitors RSSI/SNR, runs a two-channel gate on physical link quantities (SNR margin + emergency loss/FEC triggers), selects profiles
- **Drone** (`alink_drone`): Receives profile selections from GS and applies them

### Two-Channel Gate (Ground Station)

The ground station uses a two-channel decision system on physical link quantities (no synthetic score):

**Channel A — SNR Margin (slow/symmetric):**
- Computes SNR margin as: `snr_ema - MCS_SNR_THRESHOLD[mcs] - stress_margin`
- Stress margin widens under link pressure: `snr_safety_margin + loss_rate * loss_weight + fec_pressure * fec_weight`
- Upgrades require margin ≥ hysteresis threshold + slope prediction confirmation
- Downgrades fire when margin falls below negative hysteresis threshold

**Channel B — Emergency Triggers (fast/asymmetric):**
- Loss rate ≥ 15% forces immediate one-step MCS downgrade
- FEC pressure ≥ 75% forces immediate one-step MCS downgrade
- Bypasses rate limiting and confidence gating

### Dynamic Profile Calculation Mode

When enabled, the GS computes profile parameters from real-time link metrics using 802.11n reference tables instead of table lookup. This is the recommended mode and **does not require a txprofiles.conf file**.

- **MCS selection:** Based on SNR thresholds with configurable safety margin
- **Guard interval:** Short GI selected when SNR margin is comfortable (>5dB default)
- **FEC adjustment:** Frame-aligned algorithm that sizes FEC blocks to match video frame packet count
- **Bitrate computation:** Derived from PHY rate × utilization factor × FEC efficiency
- **Power scaling:** Linear inverse scaling with MCS level for link stability

This mode provides finer-grained adaptive control without needing profile tables.

### Telemetry Logging

The GS can log telemetry data in JSONL format for ML training:

```ini
[telemetry]
log_enabled = True
log_dir = /var/log/alink
log_rotate_mb = 50
outcome_window_ticks = 10
```

Each record includes link metrics, SNR margins, gate state, and selected profile parameters. Outcome tracking labels link quality following profile changes.

## ML Offline Analysis Tools

The `ground-station/ml/` directory contains offline analysis tools:

```bash
# Analyze telemetry data and generate plots
python3 ground-station/ml/analyze_telemetry.py --input /var/log/alink --output ./analysis-output

# Compute ML features from telemetry data
python3 ground-station/ml/feature_engineering.py --input /var/log/alink

# Bayesian parameter optimization
python3 ground-station/ml/optimize_params.py --input /var/log/alink

# Offline profile selection simulation
python3 ground-station/ml/replay_simulator.py --input /var/log/alink
```

## Project Structure

```
adaptive-link/
├── drone/                    # C daemon source code
│   ├── src/                  # Source files (12 modules)
│   │   ├── main.c            # Entry point, thread orchestration
│   │   ├── alink_types.h     # Shared types and constants
│   │   ├── config.c          # Configuration parsing
│   │   ├── hardware.c        # WiFi adapter, camera queries
│   │   ├── command.c         # Template substitution, execution
│   │   ├── profile.c         # Profile application
│   │   ├── osd.c             # On-screen display
│   │   ├── keyframe.c        # Keyframe request deduplication
│   │   ├── rssi_monitor.c    # Drone antenna RSSI monitoring
│   │   ├── tx_monitor.c      # TX drop monitoring
│   │   ├── message.c         # UDP message parsing
│   │   ├── fallback.c        # Heartbeat timeout handling
│   │   └── http_client.c     # Native HTTP client
│   └── test/                 # Unity test framework
│       ├── test_util.c
│       └── test_message.c
├── ground-station/           # Python ground station
│   ├── alink_gs              # Main script (~1000 lines)
│   ├── ml/                   # ML offline analysis tools
│   │   ├── analyze_telemetry.py
│   │   ├── feature_engineering.py
│   │   ├── optimize_params.py
│   │   └── replay_simulator.py
│   └── test/                 # Python tests
│       ├── test_dynamic_profile.py
│       ├── test_feature_engineering.py
│       ├── test_telemetry_logger.py
│       ├── test_replay_simulator.py
│       └── test_optimize_params.py
├── config/                   # Configuration templates
│   ├── alink.conf            # Drone daemon config
│   ├── alink_gs.conf         # Ground station config
│   └── wlan_adapters.yaml    # WiFi adapter capabilities
├── profiles/                 # TX profile presets
│   ├── default.conf
│   ├── safe-9mbps.conf
│   └── ...
├── scripts/                  # Installation scripts
│   └── install.sh
└── docs/                     # Documentation
    ├── ARCHITECTURE.md       # Technical architecture
    └── FLOW.md               # Data flow
```

## Configuration

### Drone (`/etc/alink.conf`)

```ini
allow_set_power=1
use_0_to_4_txpower=1
power_level_0_to_4=0
fallback_ms=1000
osd_level=0
get_card_info_from_yaml=1
http_timeout_ms=500
```

### Ground Station (`/etc/alink_gs.conf`)

```ini
[outgoing]
udp_ip = 10.5.0.10
udp_port = 9999

[wfb-ng]
host = 127.0.0.1
port = 8103

[keyframe]
allow_idr = True
idr_max_messages = 4
idr_send_interval_ms = 20

[profile selection]
hold_fallback_mode_ms = 1000
hold_modes_down_ms = 3000
min_between_changes_ms = 200
fast_downgrade = True
upward_confidence_loops = 3

[gate]
hysteresis_up_db = 2.5
hysteresis_down_db = 1.0
snr_slope_alpha = 0.3
snr_predict_horizon_ticks = 3
emergency_loss_rate = 0.15
emergency_fec_pressure = 0.75

[dynamic]
snr_safety_margin = 3
snr_ema_alpha = 0.3
loss_margin_weight = 20
fec_margin_weight = 5
max_mcs = 5
short_gi_snr_margin = 5
utilization_factor = 0.8
max_bitrate = 24000
min_bitrate = 1000
bandwidth = 20
gop = 10

[telemetry]
log_enabled = True
log_dir = /var/log/alink
log_rotate_mb = 50
outcome_window_ticks = 10
```

## Building from Source

```bash
# Build drone daemon
make

# Run tests
make test

# Clean
make clean

# Python tests
python3 -m pytest ground-station/test/ -v
```

## Ground Station Power Settings

Set appropriate power levels for your WiFi cards (example for 8812AU):

```bash
sudo modprobe 88XXau_wfb rtw_tx_pwr_idx_override=30
```

## Documentation

- [Architecture](docs/ARCHITECTURE.md) - Technical architecture details
- [Flow](docs/FLOW.md) - Data flow documentation
- [Contributing](CONTRIBUTING.md) - Development guidelines

## License

See [LICENSE](LICENSE)