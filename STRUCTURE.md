# STRUCTURE.md - Adaptive-Link Architecture Overview

## Table of Contents

- [Project Summary](#project-summary)
- [Tech Stack](#tech-stack)
- [Directory Structure](#directory-structure)
- [Core Architecture](#core-architecture)
- [Key Components](#key-components)
  - [Drone Daemon (multi-module C)](#drone-daemon-multi-module-c)
  - [Ground Station (alink_gs)](#ground-station-alink_gs)
  - [Profile System](#profile-system)
  - [Command Template System](#command-template-system)
- [Data Flow](#data-flow)
  - [Signal Flow](#signal-flow)
  - [Score Calculation (Ground Station)](#score-calculation-ground-station)
  - [Profile Selection Algorithm (Drone)](#profile-selection-algorithm-drone)
  - [Profile Application Order](#profile-application-order)
- [Communication Protocols](#communication-protocols)
- [Threading Model](#threading-model)
- [Concurrency & Shared State](#concurrency--shared-state)
- [Configuration Reference](#configuration-reference)
- [Build, Test & Run](#build-test--run)
- [Deployment](#deployment)
- [Technical Debt & Observations](#technical-debt--observations)

---

## Project Summary

OpenIPC Adaptive-Link is an adaptive wireless link profile selector for OpenIPC FPV drone systems. It dynamically adjusts video bitrate, MCS (modulation/coding scheme), FEC (forward error correction), TX power, and other transmission parameters based on real-time signal quality from the ground station.

The system continuously monitors link quality and smoothly transitions between pre-defined transmission profiles, using hysteresis, exponential smoothing, and time-based guards to prevent oscillation.

---

## Tech Stack

| Component | Technology |
|-----------|-----------|
| Drone daemon | C (C99), pthreads |
| Ground station | Python 3 (stdlib only) |
| Build system | GNU Make |
| IPC | UDP sockets, Unix domain sockets |
| External tools | `wfb_tx_cmd` (wfb-ng), `curl` (Majestic HTTP API), `iw` (WiFi), `cli` (Majestic CLI), `yaml-cli-multi` |
| Config format | INI-style (custom parser on drone, `configparser` on GS) |
| Profile format | Space-delimited range-mapped table |
| Deployment | Shell script (`alink_install.sh`), systemd (GS), rc.local (drone) |

No external C libraries beyond libc and pthreads. No Python packages beyond the standard library.

---

## Directory Structure

```
adaptive-link/
├── main.c                     # Entry point, socket setup, thread orchestration
├── alink_types.h              # Shared types, constants, macros (header-only)
├── util.h / util.c            # String helpers, time calculations
├── config.h / config.c        # alink.conf + txprofiles.conf parsing
├── hardware.h / hardware.c    # WiFi adapter, power tables, camera/video
├── command.h / command.c      # Template substitution, system() execution
├── profile.h / profile.c     # Profile selection, hysteresis, application
├── osd.h / osd.c             # OSD string assembly, display thread
├── keyframe.h / keyframe.c   # Keyframe request deduplication
├── rssi_monitor.h / rssi_monitor.c  # Drone antenna RSSI queue + thread
├── tx_monitor.h / tx_monitor.c      # TX drop monitoring thread
├── message.h / message.c     # UDP heartbeat message parsing
├── cmd_server.h / cmd_server.c      # Unix socket command handler
├── fallback.h / fallback.c   # Message count / fallback thread
│
├── alink_drone.c              # Original monolithic source (kept as reference)
├── alink_gs                   # Ground station script (Python 3, ~376 lines)
├── Makefile                   # Build configuration (13 compilation units)
├── alink_install.sh           # Installation script for both drone and GS
│
├── alink.conf                 # Drone daemon configuration (41 parameters)
├── alink_gs.conf              # Ground station configuration
├── txprofiles.conf            # Active TX profile mapping (symlinked from txprofiles/)
├── wlan_adapters.yaml         # WiFi adapter power tables and capabilities
│
├── txprofiles/                # Profile presets for different hardware/scenarios
│   ├── txprofiles.conf                        # Default profiles
│   ├── txprofiles.SAFE-20MHz-9mbps.conf       # Conservative long-range
│   ├── txprofiles.AF1-EU2-20Mhz-30mbps.conf  # High-performance
│   └── ...                                    # Other presets
│
├── more/                      # Legacy/experimental code
├── LICENSE
├── README.md
└── CLAUDE.md
```

---

## Core Architecture

```
                          UDP (10.5.0.10:9999)
  ┌─────────────────┐  ────────────────────────>  ┌──────────────────────┐
  │  Ground Station  │    score, RSSI, SNR,        │    Drone Daemon      │
  │   (alink_gs)     │    FEC stats, keyframe      │   (alink_drone)      │
  │                  │    codes                     │                      │
  │  Python 3        │                              │  C + pthreads        │
  └────────┬─────────┘                              └──────────┬───────────┘
           │                                                   │
    TCP to wfb-ng                                    Executes commands:
    JSON stats port                                  ├── wfb_tx_cmd (MCS, FEC)
    (127.0.0.1:8103)                                 ├── curl (Majestic API)
                                                     ├── iw (TX power)
                                                     └── Writes OSD to /tmp/
```

**One-directional link:** The ground station continuously pushes link quality data to the drone. There is no drone-to-GS feedback channel in this system.

---

## Key Components

### Drone Daemon (multi-module C)

A multi-module, multi-threaded C daemon (13 source files). Core responsibilities:

1. **UDP listener** - Receives link quality scores from GS
2. **Profile engine** - Maps scores to transmission profiles via range matching
3. **Command executor** - Applies profiles by running shell commands with template substitution
4. **Stability logic** - Hysteresis, exponential smoothing, rate limiting, hold-down timers
5. **Monitoring** - TX drop detection, antenna RSSI parsing, OSD updates
6. **External control** - Unix socket interface for power changes and parameter queries

### Ground Station (alink_gs)

A Python 3 script that:

1. Connects to wfb-ng's JSON statistics TCP port
2. Extracts per-antenna RSSI, SNR, FEC recovery, and packet loss metrics
3. Normalizes and weights metrics into a composite score (range 1000-2000)
4. Applies Kalman filtering for noise estimation
5. Optionally penalizes score based on filtered noise level
6. Sends UDP heartbeat messages to the drone (~10 Hz)
7. Generates keyframe request codes on packet loss events

### Profile System

Profiles in `txprofiles.conf` define transmission parameter sets mapped to score ranges:

```
<score_min> - <score_max> <GI> <MCS> <FecK> <FecN> <bitrate> <GOP> <power> <ROI_QP> <BW> <QpDelta>
```

| Field | Example | Description |
|-------|---------|-------------|
| score range | 1000-1050 | Link quality score window |
| GI | long/short | Guard interval |
| MCS | 0-7 | Modulation and coding scheme |
| FecK/FecN | 2/3 | FEC ratio (k data, n total) |
| bitrate | 2000 | Video bitrate in kbps |
| GOP | 10 | Group of pictures |
| power | 30 | TX power level (raw or index 0-4) |
| ROI_QP | 0,0,0,0 | Region of interest QP values |
| BW | 20 | Channel bandwidth (MHz) |
| QpDelta | -12 | Quantization parameter delta |

Profile 999 is the **fallback profile** used when GS heartbeats are lost.

Lower scores map to conservative profiles (low MCS, low bitrate, high FEC redundancy).
Higher scores map to aggressive profiles (high MCS, high bitrate, less FEC).

### Command Template System

Settings are applied via shell command templates defined in `alink.conf`. Templates use `{placeholder}` syntax:

```ini
powerCommandTemplate = iw dev wlan0 set txpower fixed {power}
mcsCommandTemplate   = wfb_tx_cmd 8000 set_radio -B {bandwidth} -G {gi} -S {stbc} -L {ldpc} -M {mcs}
bitrateCommandTemplate = curl -s 'http://localhost/api/v1/set?video0.bitrate={bitrate}'
fecCommandTemplate   = wfb_tx_cmd 8000 set_fec -k {fecK} -n {fecN}
idrCommandTemplate   = curl localhost/request/idr
```

This decouples the control logic from hardware-specific commands, making it adaptable to different camera/radio stacks.

---

## Data Flow

### Signal Flow

```
wfb-ng RX stats ──TCP──> alink_gs ──UDP──> alink_drone ──system()──> wfb_tx_cmd / curl / iw
     │                      │                    │
  RSSI, SNR,          Score calc +          Profile match +
  FEC, loss           Kalman filter         Smoothing/hysteresis
```

### Score Calculation (Ground Station)

```
1. Normalize:
   snr_norm  = clamp((snr - SNR_MIN) / (SNR_MAX - SNR_MIN), 0, 1)
   rssi_norm = clamp((rssi - RSSI_MIN) / (RSSI_MAX - RSSI_MIN), 0, 1)

2. Combine:
   raw_score = 1000 + (snr_weight * snr_norm + rssi_weight * rssi_norm) * 1000

3. Error estimation:
   error_ratio = (5 * lost_packets + adjusted_fec_recovered) / (total_packets / num_antennas)
   filtered_noise = kalman_update(error_ratio)

4. Penalty (optional):
   deduction = min(((filtered_noise - min_noise) / (max_noise - min_noise)) ^ exponent, 1.0)
   final_score = raw_score * (1 - deduction)

5. FEC change signal (0-5 scale based on filtered noise level)
```

### Profile Selection Algorithm (Drone)

The `start_selection()` and `value_chooses_profile()` functions implement a multi-stage selection pipeline:

```
1. FALLBACK CHECK
   Score == 999 → immediately select fallback profile

2. WEIGHTED COMBINATION
   combined = rssi_score * rssi_weight + snr_score * snr_weight

3. SCORE CAPPING
   combined = min(combined, limit_max_score_to)   // default: 2000

4. EXPONENTIAL SMOOTHING
   factor = (combined >= previous) ? smoothing_up : smoothing_down
   smoothed = factor * combined + (1 - factor) * smoothed

5. RATE LIMITING
   Skip if elapsed_ms < min_between_changes_ms    // default: 200ms

6. HYSTERESIS CHECK
   pct_change = |combined - last_sent| / last_sent * 100
   threshold  = (combined >= last_sent) ? hysteresis_up : hysteresis_down
   Skip if pct_change < threshold                  // default: 5%

7. PROFILE LOOKUP
   Match smoothed score against profile range boundaries

8. TIME GUARDS
   - Leaving fallback: wait hold_fallback_mode_s   // default: 1s
   - Upgrading profile: wait hold_modes_down_s      // default: 3s

9. APPLY PROFILE
   Execute command templates for the new profile
```

### Profile Application Order

Command execution order differs based on direction of change:

**Upgrading (better link):** QpDelta → FPS → Power → GOP → MCS → FEC/Bitrate → ROI → IDR

**Downgrading (worse link):** QpDelta → FPS → FEC/Bitrate → GOP → MCS → Power → ROI → IDR

The key difference: when downgrading, FEC/bitrate changes happen **before** MCS changes to ensure error protection is in place before reducing modulation rate. Commands are paced with configurable delays (`pace_exec`, default 50ms).

---

## Communication Protocols

### UDP Messages (GS to Drone)

```
Wire format: <4-byte length (network byte order)><payload string>

Payload: timestamp:rssi_score:snr_score:fec_rec:lost:best_rssi:best_snr:antennas:penalty:fec_change[:keyframe_code]

Example: 1700000000:1500:1500:42:3:-65:28:4:10:2:abcd
```

Special messages are prefixed with `special:`:
- `special:pause_adaptive` - Pause profile changes
- `special:resume_adaptive` - Resume profile changes
- `special:request_keyframe:<code>` - Request IDR frame (deduplicated by code)

### Unix Socket (External Control, /tmp/alink_cmd.sock)

Binary protocol with 4-byte header:

```c
struct { uint16_t cmd; uint16_t len; }  // network byte order
```

| Command | ID | Payload | Purpose |
|---------|----|---------|---------|
| CMD_SET_POWER | 1 | uint32_t (0-4) | Set TX power level |
| CMD_ANTENNA_STATS | 3 | RX_ANT string | Feed antenna RSSI data |
| CMD_GET | 4 | param name string | Query parameter value |
| CMD_SET | 5 | "param value" string | Set parameter |

### OSD Output

Written to `/tmp/MSPOSD.msg` every 1 second (or via UDP). Verbosity controlled by `osd_level` (0-6):

- **0:** Disabled
- **1:** Regular OSD only
- **2-3:** Profile + FEC + stats
- **4-5:** Full diagnostics (single-line or multi-line)
- **6:** Full + camera binary info

---

## Threading Model

| Thread | Module | Entry Point | Frequency | Responsibility |
|--------|--------|-------------|-----------|----------------|
| Main | `main.c` | `main()` | Per UDP packet | Receive messages, parse, trigger profile selection |
| RSSI Monitor | `rssi_monitor.c` | `rssi_thread_func()` | Per queue item | Parse drone antenna RSSI, detect weak antennas (>20dB spread) |
| Fallback | `fallback.c` | `fallback_thread_func()` | Every `fallback_ms` | Count heartbeats; trigger fallback on GS silence |
| TX Drop Monitor | `tx_monitor.c` | `txmon_thread_func()` | Every `check_xtx_period_ms` | Read `/sys/class/net/wlan0/statistics/tx_dropped`, reduce bitrate on drops |
| OSD Updater | `osd.c` | `osd_thread_func()` | Every 1s | Generate and write OSD string |
| Cmd Server | `cmd_server.c` | `cmdsrv_thread_func()` | Per socket connection | Handle external commands via Unix socket |

---

## Concurrency & Shared State

| Mutex | Owner | Protects | Writers | Readers |
|-------|-------|----------|---------|---------|
| `count_mutex` | `main.c` | `message_count` | Main thread (+1 per msg) | Fallback thread (read + reset) |
| `pause_mutex` | `main.c` | `paused` flag | Cmd server, keyframe (special msgs) | Message processing, fallback |
| `tx_power_mutex` | `main.c` | `power_level_0_to_4` | Cmd server (set) | `profile_apply()` (read) |
| `keyframe_state_t.mutex` | `keyframe.c` | `KeyframeRequest codes[]` | Main thread (add codes) | Main thread (check duplicates) |
| `rssi_state_t.lock` | `rssi_monitor.c` | RSSI circular queue | Cmd server (enqueue) | RSSI thread (dequeue) |

The first three mutexes are allocated in `alink_daemon_t` (main.c) and passed by pointer to thread modules. The last two are fully encapsulated within their respective module structs.

Additional synchronization:
- `selection_busy` flag in `profile_state_t` prevents nested profile changes (non-atomic, but only written/read from main thread)
- `initialized` flag in `alink_daemon_t` gates operations until first UDP message received

---

## Configuration Reference

### alink.conf (Drone) - Key Parameters

| Category | Parameter | Default | Description |
|----------|-----------|---------|-------------|
| Power | `allow_set_power` | 1 | Enable TX power control |
| Power | `use_0_to_4_txpower` | 1 | Use power table (vs raw multiplication) |
| Power | `power_level_0_to_4` | 0 | Power index into adapter table |
| Weights | `rssi_weight` | 0.5 | RSSI contribution to combined score |
| Weights | `snr_weight` | 0.5 | SNR contribution to combined score |
| Timing | `fallback_ms` | 1000 | Timeout before entering fallback profile |
| Timing | `hold_fallback_mode_s` | 1 | Minimum time in fallback before leaving |
| Timing | `hold_modes_down_s` | 3 | Minimum time before upgrading profile |
| Timing | `min_between_changes_ms` | 200 | Minimum interval between profile changes |
| Smoothing | `exp_smoothing_factor` | 0.1 | Smoothing for score increases |
| Smoothing | `exp_smoothing_factor_down` | 1.0 | Smoothing for score decreases |
| Hysteresis | `hysteresis_percent` | 5 | Change threshold for increases (%) |
| Hysteresis | `hysteresis_percent_down` | 5 | Change threshold for decreases (%) |
| FEC | `allow_dynamic_fec` | 0 | Adjust FEC based on noise signals |
| FEC | `fec_k_adjust` | 1 | Decrease K (vs increase N) for FEC |
| Keyframe | `allow_request_keyframe` | 1 | Allow IDR frame requests |
| Keyframe | `request_keyframe_interval_ms` | 1112 | Minimum between IDR requests |
| TX Monitor | `check_xtx_period_ms` | 2250 | TX dropped check interval |
| TX Monitor | `xtx_reduce_bitrate_factor` | 0.8 | Bitrate reduction on TX drops |
| OSD | `osd_level` | 0 | OSD verbosity (0-6) |

### alink_gs.conf (Ground Station) - Key Parameters

| Section | Parameter | Default | Description |
|---------|-----------|---------|-------------|
| outgoing | `udp_ip` | 10.5.0.10 | Drone IP address |
| outgoing | `udp_port` | 9999 | Drone listen port |
| json | `HOST` | 127.0.0.1 | wfb-ng stats host |
| json | `PORT` | 8103 | wfb-ng stats port |
| weights | `snr_weight` | 0.5 | SNR contribution to score |
| weights | `rssi_weight` | 0.5 | RSSI contribution to score |
| ranges | `SNR_MIN/MAX` | 12/38 | SNR normalization bounds |
| ranges | `RSSI_MIN/MAX` | -80/-30 | RSSI normalization bounds (dBm) |
| noise | `kalman_estimate` | 0.005 | Initial Kalman filter estimate |

---

## Build, Test & Run

### Building

```bash
# Native build
make

# Cross-compile for OpenIPC target (e.g., HiSilicon)
make CC=arm-linux-gnueabihf-gcc OPT="-O2 -march=armv7-a"

# Clean
make clean
```

The build produces a single binary: `alink_drone`. No external library dependencies beyond libc and pthreads.

The ground station script (`alink_gs`) is interpreted Python 3 and requires no build step.

### Testing

There are **no automated tests** configured. Testing is done manually on hardware:

1. Deploy to drone and ground station hardware
2. Verify UDP communication (score messages arriving)
3. Observe profile changes via OSD or logs
4. Test edge cases: signal loss/recovery, rapid changes, antenna failures

### Running Locally (Development)

**Drone daemon:**
```bash
./alink_drone [listen_ip] [listen_port] [gs_ip]
# Defaults: 10.5.0.10 9999 10.5.0.1
```

Requires:
- `/etc/alink.conf` (or local `alink.conf`)
- `/etc/txprofiles.conf` (or local `txprofiles.conf`)
- `/etc/wlan_adapters.yaml` (if `get_card_info_from_yaml=1`)

**Ground station:**
```bash
python3 alink_gs [--udp-ip IP] [--udp-port PORT]
```

Requires:
- `/etc/alink_gs.conf` (or local `alink_gs.conf`)
- wfb-ng running with JSON stats enabled on the configured port

---

## Deployment

The `alink_install.sh` script handles installation on both sides:

### Ground Station
```bash
sudo ./alink_install.sh gs install    # Fresh install
sudo ./alink_install.sh gs update     # Update binary only
sudo ./alink_install.sh gs remove     # Uninstall
```
- Downloads `alink_gs` from GitHub releases to `/usr/local/bin/`
- Creates systemd service (`alink_gs.service`)
- Configures wfb-ng log interval for stats output

### Drone
```bash
sudo ./alink_install.sh drone install
sudo ./alink_install.sh drone update
sudo ./alink_install.sh drone remove
```
- Downloads `alink_drone` to `/usr/bin/`
- Downloads configs to `/etc/`
- Adds startup entry to `/etc/rc.local`
- Configures Majestic camera settings (qpDelta, noise level)
- Enables tunnel mode in `datalink.conf`

---

## Technical Debt & Observations

### Architecture

1. **`system()` for command execution:** All hardware control goes through `system()` calls with string-interpolated commands. This works but is fragile (shell injection risk if config values are not sanitized, no structured error handling from commands, subprocess overhead per call).

2. **One-directional communication:** The GS sends scores to the drone, but the drone has no channel to send feedback to the GS (e.g., confirming profile changes, reporting TX drops). This limits coordination.

3. **No structured logging:** Diagnostic output goes to stdout/stderr and OSD. No log levels, no structured format, no rotation.

### Code Quality

4. **`selection_busy` flag is not atomic or mutex-protected.** It is read and written from the main thread only (so currently safe), but the pattern is fragile if threading changes.

5. **`profile_apply()` is called from both the main thread and the cmd_server thread** without mutex protection. This matches the original behavior and has not caused issues in practice, but is technically a race condition.

6. **No automated tests or CI.** Changes are validated manually on hardware only.

### Configuration

7. **Custom config parser (drone):** The C side uses a hand-rolled parser rather than a standard library. Each parameter requires explicit parsing code with fallback defaults.

8. **Duplicate weight definitions:** Both `alink.conf` (drone) and `alink_gs.conf` (GS) define `rssi_weight` and `snr_weight`. The GS uses its weights for score calculation, and the drone uses its weights for combining the already-weighted scores - meaning weights are applied twice if not coordinated.

### Robustness

9. **No message authentication:** UDP messages between GS and drone have no authentication or integrity checking. A malicious actor on the network could inject fake scores.

10. **Fallback profile timing:** If the GS process restarts, the drone enters fallback mode for `hold_fallback_mode_s` (default 1s) before accepting new scores. This is a brief disruption but could be longer with conservative settings.

11. **No graceful shutdown:** The daemon runs until killed. No signal handlers for cleanup (closing sockets, resetting hardware state).
