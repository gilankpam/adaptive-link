# OpenIPC Adaptive-Link

Adaptive wireless link profile selector for OpenIPC FPV drone systems. Dynamically adjusts video bitrate, MCS, FEC, TX power, and other transmission parameters based on real-time signal quality from the ground station.

## Quick Start

### Drone Installation

```bash
# Auto-install (fetches latest pre-release)
cd /etc
curl -L -o alink_install.sh https://raw.githubusercontent.com/OpenIPC/adaptive-link/refs/heads/main/scripts/install.sh
chmod +x alink_install.sh
./alink_install.sh drone install
reboot
```

Config files: `/etc/alink.conf` and `/etc/txprofiles.conf`

### Ground Station Installation

```bash
# Install on Radxa ground station
sudo curl -L -o install.sh https://raw.githubusercontent.com/OpenIPC/adaptive-link/refs/heads/main/scripts/install.sh
sudo chmod +x install.sh
sudo ./install.sh gs install
```

Service managed by systemd: `sudo systemctl status alink_gs`

## How It Works

A wireless link can handle higher data-rates over short distances with strong signal than long distances with weak signal. Adaptive-Link dynamically selects transmission profiles based on real-time link quality.

- **Ground Station** (`alink_gs`): Monitors RSSI/SNR, calculates multi-factor link quality scores, selects profiles
- **Drone** (`alink_drone`): Receives profile selections from GS and applies them

### Scoring System

Scores range from 1000 (poor link) to 2000 (excellent link):

```
# Example in alink_gs.conf
rssi_min = -80   # Score 1000
rssi_max = -40   # Score 2000
snr_min = 12     # Score 1000
snr_max = 36     # Score 2000
```

With RSSI=-60, SNR=20: normalized scores are 1500 and 1333 respectively.

## Project Structure

```
adaptive-link/
├── drone/              # C daemon source code
│   └── src/            # Source files
├── ground-station/     # Python ground station script
├── config/             # Configuration templates
│   ├── alink.conf.example
│   ├── alink_gs.conf.example
│   └── wlan_adapters.yaml
├── profiles/           # TX profile presets
├── scripts/            # Installation scripts
├── test/               # Tests
│   ├── c/              # C unit tests
│   └── python/         # Python unit tests
└── docs/               # Documentation
    ├── ARCHITECTURE.md # Technical details
    └── FLOW.md         # Data flow
```

## Configuration

### Drone (`/etc/alink.conf`)

```ini
allow_set_power=1
use_0_to_4_txpower=1
power_level_0_to_4=0
fallback_ms=1000
osd_level=0
```

### Ground Station (`/config/alink_gs.conf`)

```ini
[outgoing]
udp_ip = 10.5.0.10
udp_port = 9999

[weights]
snr_weight = 0.5
rssi_weight = 0.5

[ranges]
SNR_MIN = 12
SNR_MAX = 36
RSSI_MIN = -80
RSSI_MAX = -40
```

### TX Profiles (`/etc/txprofiles.conf`)

```
# range_min - range_max gi mcs fecK fecN bitrate gop power roi_qp bandwidth qp_delta
999 - 999 long 0 8 12 1999 10 30 0,0,0,0 20 -12
1000 - 1050 long 0 8 12 2000 10 30 0,0,0,0 20 -12
1051 - 1500 long 1 8 12 4000 10 25 0,0,0,0 20 -12
1501 - 1950 long 2 8 12 8000 10 20 12,6,6,12 20 -12
1951 - 2001 short 2 8 12 9000 10 20 12,6,6,12 20 -12
```

## Building from Source

```bash
# Build drone daemon
make

# Run tests
make test

# Clean
make clean
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