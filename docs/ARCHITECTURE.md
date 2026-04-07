# ARCHITECTURE.md - Adaptive-Link Architecture Overview

## Table of Contents

- [Project Summary](#project-summary)
- [Tech Stack](#tech-stack)
- [Directory Structure](#directory-structure)
- [Core Architecture](#core-architecture)
- [Key Components](#key-components)
  - [Drone Daemon (multi-module C)](#drone-daemon-multi-module-c)
  - [Ground Station (alink_gs)](#ground-station-alink_gs)
  - [Profile System](#profile-system)
  - [Dynamic Profile Calculation Mode](#dynamic-profile-calculation-mode)
  - [Command Template System](#command-template-system)
- [Data Flow](#data-flow)
  - [Signal Flow](#signal-flow)
  - [Multi-Factor Scoring (Ground Station)](#multi-factor-scoring-ground-station)
  - [Profile Selection Algorithm (Ground Station)](#profile-selection-algorithm-ground-station)
  - [Profile Application (Drone)](#profile-application-drone)
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

**Architecture change (commit a6fcf7b):** Profile selection logic has been offloaded from the drone to the ground station. The GS now calculates multi-factor scores, runs the full selection algorithm, and sends finalized profile parameters to the drone. The drone simply applies received profiles.

**Dynamic mode (commit 4396a63):** New MCS-based adaptive tuning mode that computes profile parameters from real-time link metrics using 802.11n reference tables, replacing table-based profile lookup.

**HTTP client (commit e2e81e6):** Native socket-based HTTP client replaces curl dependency; cmd_server module removed.

---

## Tech Stack

| Component | Technology |
|-----------|-----------|
| Drone daemon | C (C99), pthreads |
| Ground station | Python 3 (stdlib only) |
| Build system | GNU Make |
| IPC | UDP sockets |
| External tools | `wfb_tx_cmd` (wfb-ng), `iw` (WiFi) |
| HTTP client | Native socket-based (no curl) |
| Config format | INI-style (custom parser on drone, `configparser` on GS) |
| Profile format | Space-delimited range-mapped table |
| Deployment | Shell script (`scripts/install.sh`), systemd (GS), rc.local (drone) |

No external C libraries beyond libc and pthreads. No Python packages beyond the standard library.

---

## Directory Structure

```
adaptive-link/
├── drone/
│   ├── Makefile                         # Drone build configuration
│   ├── src/                             # C daemon source files
│   │   ├── main.c                       # Entry point, socket setup, thread orchestration
│   │   ├── alink_types.h                # Shared types, constants, macros (header-only)
│   │   ├── util.c / util.h              # String helpers, time calculations, URL parsing
│   │   ├── config.c / config.h          # alink.conf + txprofiles.conf parsing
│   │   ├── hardware.c / hardware.h      # WiFi adapter, power tables, camera/video
│   │   ├── command.c / command.h        # Template substitution, timeout execution via fork/exec
│   │   ├── profile.c / profile.h        # Profile application only (selection moved to GS)
│   │   ├── osd.c / osd.h               # OSD string assembly, display thread
│   │   ├── keyframe.c / keyframe.h      # Keyframe request deduplication
│   │   ├── rssi_monitor.c / rssi_monitor.h  # Drone antenna RSSI queue + thread
│   │   ├── tx_monitor.c / tx_monitor.h      # TX drop monitoring thread
│   │   ├── message.c / message.h        # UDP heartbeat message parsing
│   │   ├── fallback.c / fallback.h      # Message count / fallback thread
│   │   └── http_client.c / http_client.h  # Native HTTP GET requests (no curl)
│   └── test/
│       ├── test_util.c                  # Unity tests for C utilities
│       └── unity/                       # Unity test framework
│
├── ground-station/
│   ├── alink_gs                         # Ground station script (Python 3, ~600 lines)
│   └── test/
│       ├── test_dynamic_profile.py      # Python tests for GS dynamic mode
│       ├── test_feature_engineering.py  # Python tests for ML feature engineering
│       └── test_telemetry_logger.py     # Python tests for telemetry logging
│
├── config/
│   ├── alink.conf                       # Drone daemon configuration (simplified)
│   ├── alink_gs.conf                    # Ground station configuration (scoring + selection)
│   └── wlan_adapters.yaml               # WiFi adapter power tables and capabilities
│
├── profiles/
│   ├── default.conf                     # Default TX profile mapping
│   ├── safe-9mbps.conf                  # Conservative long-range
│   ├── af1-40mhz-26mbps.conf            # High-performance AF1
│   ├── af1-eu2-17mbps.conf              # EU2 17 Mbps
│   ├── af1-eu2-30mbps.conf              # EU2 30 Mbps
│   └── eu2-20mhz-20.8mbps.conf          # EU2 20.8 Mbps
│
├── scripts/
│   └── install.sh                       # Installation script for both drone and GS
│
├── docs/
│   ├── ARCHITECTURE.md                  # This file
│   └── FLOW.md                          # Data flow documentation
│
├── Makefile                             # Top-level build configuration
├── LICENSE
├── README.md
├── CLAUDE.md
└── CONTRIBUTING.md
```

---

## Core Architecture

```
                          UDP (10.5.0.10:9999)
  ┌─────────────────┐  ──────────────────────────────────────────>  ┌──────────────────────┐
  │  Ground Station  │    profile index + all parameters           │    Drone Daemon      │
  │   (alink_gs)     │    (P: message with GI, MCS, FEC, etc.)     │   (alink_drone)      │
  │                  │                                               │                      │
  │  Python 3        │                                               │  C + pthreads        │
  │  └─ Scoring      │                                               │  └─ Apply profiles   │
  │  └─ Selection    │                                               │     via templates    │
  └────────┬─────────┘                                               └──────────┬───────────┘
           │                                                                    │
    TCP to wfb-ng                                                    Executes commands:
    JSON stats port                                                ├── wfb_tx_cmd (MCS, FEC)
    (127.0.0.1:8103)                                               ├── HTTP client (camera API)
                                                                    └── iw (TX power)
```

**One-directional link:** The ground station continuously pushes finalized profile selections to the drone. There is no drone-to-GS feedback channel in this system.

---

## Key Components

### Drone Daemon (multi-module C)

A multi-module, multi-threaded C daemon (12 source files in `drone/src/`). Core responsibilities:

1. **UDP listener** - Non-blocking receive loop (`select()` with 50ms timeout) for profile messages from GS
2. **Profile application** - Receives finalized profile parameters from GS and applies them via `profile_apply_direct()`
3. **Async command executor** - Dispatches profile changes to a background worker thread, applying shell commands with template substitution without blocking the main loop
4. **Monitoring** - TX drop detection, antenna RSSI parsing, OSD updates
5. **HTTP client** - Native socket-based HTTP GET for camera API (replaces curl)

**Note:** Profile selection logic (scoring, smoothing, hysteresis, confidence gating) has been removed from the drone and moved to the GS.

### Ground Station (alink_gs)

A Python 3 script that:

1. Connects to wfb-ng's JSON statistics TCP port
2. Extracts per-antenna RSSI, SNR, FEC recovery, and packet loss metrics
3. Calculates multi-factor score (RF quality, loss rate, FEC pressure, antenna diversity)
4. Runs dual EMA smoothing with predictive trend detection
5. Applies hysteresis and confidence gating for stable transitions
6. Selects the appropriate profile from `profiles/*.conf` OR computes dynamically (dynamic mode)
7. Sends UDP messages with finalized profile parameters to the drone (~10 Hz)
8. Generates keyframe request codes on packet loss events

### Profile System

Profiles in `profiles/*.conf` define transmission parameter sets mapped to score ranges:

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

Profile 0 is the **fallback profile** used when GS heartbeats are lost.

Lower scores map to conservative profiles (low MCS, low bitrate, high FEC redundancy).
Higher scores map to aggressive profiles (high MCS, high bitrate, less FEC).

### Dynamic Profile Calculation Mode

When `dynamic_mode = True` in `alink_gs.conf`, the GS computes profile parameters from real-time link metrics instead of table lookup. This mode uses 802.11n reference tables:

**802.11n MCS Reference Tables:**
```python
# Minimum SNR (dB) required for each MCS index
MCS_SNR_THRESHOLDS = [5, 8, 11, 14, 17, 20, 23, 26]

# PHY data rates in Mbps: [long_gi, short_gi]
PHY_RATES_20MHZ = [
    (6.5, 7.2),   (13.0, 14.4), (19.5, 21.7), (26.0, 28.9),
    (39.0, 43.3), (52.0, 57.8), (58.5, 65.0), (65.0, 72.2)
]

PHY_RATES_40MHZ = [
    (13.5, 15.0), (27.0, 30.0), (40.5, 45.0), (54.0, 60.0),
    (81.0, 90.0), (108.0, 120.0), (121.5, 135.0), (135.0, 150.0)
]

# Default FEC (fec_k, fec_n) per MCS level
FEC_TABLE = [
    (2, 3), (2, 3), (4, 6), (6, 9), (8, 12), (8, 12), (10, 12), (10, 12)
]
```

**MCS Selection:**
```
1. Compute dynamic safety margin:
   margin = snr_safety_margin + 
            loss_rate * loss_margin_weight + 
            fec_pressure * fec_margin_weight

2. Select highest MCS where SNR >= threshold + margin:
   for mcs in range(max_mcs, -1, -1):
       if snr_ema >= MCS_SNR_THRESHOLDS[mcs] + margin:
           selected_mcs = mcs
           break
```

**Guard Interval Selection:**
```
1. Calculate SNR above threshold:
   snr_above = snr_ema - MCS_SNR_THRESHOLDS[selected_mcs]

2. Use short GI when comfortable margin exists:
   use_short_gi = (snr_above >= short_gi_snr_margin and
                   loss_rate < 0.02 and fec_pressure < 0.3)
```

**FEC Adjustment:**
```
1. Base FEC from MCS table:
   fec_k, fec_n = FEC_TABLE[mcs]

2. Increase redundancy under loss:
   if loss_rate > loss_threshold_for_fec_downgrade:
       fec_k = max(2, fec_k - 2)
```

**Bitrate Computation:**
```
1. Get PHY rate for bandwidth and GI:
   phy_rates = PHY_RATES_40MHZ if bandwidth == 40 else PHY_RATES_20MHZ
   phy_rate = phy_rates[mcs][1 if short_gi else 0]

2. Apply FEC efficiency and utilization factor:
   bitrate = phy_rate * 1000 * (fec_k / fec_n) * utilization_factor

3. Clamp to configured range:
   bitrate = max(min_bitrate, min(max_bitrate, bitrate))
```

**Power Scaling:**
```
TX power inversely scaled with MCS level for link stability:
- MCS 0-1:   max_power (highest for stability)
- MCS 2-3:   max_power - range/3
- MCS 4-5:   min_power + range/3
- MCS 6-7:   min_power (lowest for efficiency)
```

### Command Template System

Settings are applied via shell command templates defined in `config/alink.conf`. Templates use `{placeholder}` syntax:

```ini
powerCommandTemplate = iw dev wlan0 set txpower fixed {power}
mcsCommandTemplate   = wfb_tx_cmd 8000 set_radio -B {bandwidth} -G {gi} -S {stbc} -L {ldpc} -M {mcs}
apiCommandTemplate   = curl -s 'http://localhost/api/v1/set?video0.bitrate={bitrate}&video0.gop={gop}&video0.qpDelta={qpDelta}&video0.roiQp={roiQp}'
fecCommandTemplate   = wfb_tx_cmd 8000 set_fec -k {fecK} -n {fecN}
idrCommandTemplate   = http_get localhost 80 /request/idr
```

**API batching:** The `apiCommandTemplate` batches multiple camera parameters (qpDelta, bitrate, gop, roiQp) into a single HTTP request for efficiency.

**Native HTTP client:** HTTP requests use `http_client.c` (socket-based) instead of curl, with `cmd_exec_with_timeout()` providing millisecond-precision timeout via fork/exec.

**Command execution:** All shell commands use `cmd_exec_with_timeout()` for execution. The legacy `cmd_exec()` and `cmd_exec_noquote()` functions have been removed. Commands that exceed the timeout (default 500ms) are killed and return -1.

This decouples the control logic from hardware-specific commands, making it adaptable to different camera/radio stacks.

---

## Data Flow

### Signal Flow

```
wfb-ng RX stats ──TCP──> alink_gs ──UDP (P: message)──> alink_drone ──system()/http_get()──> wfb_tx_cmd / camera API / iw
     │                      │
  RSSI, SNR,          Multi-factor scoring +
  FEC, loss           Profile selection (table or dynamic)
```

### Multi-Factor Scoring (Ground Station)

The GS computes a composite score from four weighted factors:

```
1. RF Quality Score (50% weight):
   snr_norm  = clamp((snr - SNR_MIN) / (SNR_MAX - SNR_MIN), 0, 1)
   rssi_norm = clamp((rssi - RSSI_MIN) / (RSSI_MAX - RSSI_MIN), 0, 1)
   rf_score  = snr_norm * 0.5 + rssi_norm * 0.5

2. Packet Loss Score (25% weight):
   loss_rate = lost_packets / all_packets (since last update)
   loss_score = 1.0 - loss_rate

3. FEC Pressure Score (15% weight):
   redundancy = fec_n - fec_k
   weighted_fec = fec_rec * (6.0 / (1 + redundancy))
   fec_score = 1.0 - min(weighted_fec / all_packets, max_loss_rate)

4. Antenna Diversity Score (10% weight):
   rssi_spread = max_rssi - min_rssi
   diversity_score = 1.0 - (rssi_spread / max_rssi_spread)

5. Combined Score:
   score = 1000 + (rf_weight*rf_score + loss_weight*loss_score +
                   fec_weight*fec_score + diversity_weight*diversity_score) * 1000
```

### Profile Selection Algorithm (Ground Station)

The `ProfileSelector` class implements the full selection pipeline:

```
1. RAW SCORE COMPUTATION
   Calculate multi-factor score from RF, loss, FEC, and diversity metrics

2. SCORE CAPPING
   raw_score = min(raw_score, limit_max_score_to)   // default: 2000

3. DUAL EMA SMOOTHING
   ema_fast = ema_fast_alpha * raw_score + (1 - ema_fast_alpha) * ema_fast    // α=0.5
   ema_slow = ema_slow_alpha * raw_score + (1 - ema_slow_alpha) * ema_slow    // α=0.15

4. PREDICTIVE SCORE (when degrading: fast < slow)
   gap = ema_slow - ema_fast
   effective = min(smoothed, ema_fast - gap * predict_multi)   // predict_multi=1.0

5. RATE LIMITING
   Skip if elapsed_ms < min_between_changes_ms   // default: 200ms

6. HYSTERESIS CHECK
   pct_change = |effective - last_sent| / last_sent * 100
   threshold  = (improving) ? hysteresis_percent : hysteresis_percent_down
   Skip if pct_change < threshold                 // default: 5% both directions

7. PROFILE LOOKUP (table mode) OR DYNAMIC COMPUTATION (dynamic mode)
   - Table mode: Match effective score against profiles/*.conf range boundaries
   - Dynamic mode: Compute profile from SNR EMA, loss rate, FEC pressure

8. ASYMMETRIC STEPPING
   - Downgrade (fast_downgrade=True): bypass hold timer, apply immediately
   - Upgrade: require upward_confidence_loops (default 3) consecutive
     evaluations selecting same higher profile
   - Leaving fallback (profile 0): wait hold_fallback_mode_ms   // default: 1000ms

9. SEND TO DRONE
   Send P: message with profile index and all parameters
```

### Profile Application (Drone)

The drone receives `P:` messages and applies profiles via `profile_apply_direct()`:

```
1. Parse P: message (profile_idx, gi, mcs, fec_k, fec_n, bitrate, gop, power, roi_qp, bandwidth, qp_delta, timestamp)

2. Store as lastAppliedProfile (for re-application via command)

3. Skip if profile_index == currentProfile (GS sends current profile every tick for reliability)

4. Update tracking state:
   previousProfile = currentProfile
   currentProfile = profile_index
   prevTimeStamp = now_ms()

5. Dispatch to async worker thread (non-blocking)
   - Apply commands in order: QpDelta → FPS → Power → GOP → MCS → FEC/Bitrate → ROI → IDR
   - Execute via cmd_exec_with_timeout() (500ms default timeout)
   - Pace execution with configurable delays
   - Use native HTTP client for camera API requests
```

---

## Communication Protocols

### UDP Messages (GS to Drone)

```
Wire format: <4-byte length (network byte order)><payload string>

Profile message (P:):
P:<profile_idx>:<gi>:<mcs>:<fec_k>:<fec_n>:<bitrate>:<gop>:<power>:<roi_qp>:<bandwidth>:<qp_delta>:<timestamp>[:<keyframe_code>]

Example: P:5:long:5:8:12:6000:10:58:0,0,0,0:20:0:1700000000:abcd
```

The GS sends the current profile on every tick for UDP reliability. The drone skips application if the profile index matches the current profile.

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
| Main | `drone/src/main.c` | `main()` | 50ms select timeout / per UDP packet | Non-blocking receive, parse profile messages |
| Profile Worker | `drone/src/profile.c` | `profile_worker_func()` | Per job signal | Execute profile commands asynchronously (system() calls) |
| RSSI Monitor | `drone/src/rssi_monitor.c` | `rssi_thread_func()` | Per queue item | Parse drone antenna RSSI, detect weak antennas (>20dB spread) |
| Fallback | `drone/src/fallback.c` | `fallback_thread_func()` | Every `fallback_ms` | Count heartbeats; trigger fallback on GS silence |
| TX Drop Monitor | `drone/src/tx_monitor.c` | `txmon_thread_func()` | Every `check_xtx_period_ms` | Read `/sys/class/net/wlan0/statistics/tx_dropped`, reduce bitrate on drops |
| OSD Updater | `drone/src/osd.c` | `osd_thread_func()` | Every 1s | Generate and write OSD string |

**Removed:** Cmd Server thread (`cmd_server.c`) - Unix socket IPC removed in commit e2e81e6.

---

## Concurrency & Shared State

| Mutex | Owner | Protects | Writers | Readers |
|-------|-------|----------|---------|---------|
| `count_mutex` | `main.c` | `message_count` | Main thread (+1 per msg) | Fallback thread (read + reset) |
| `pause_mutex` | `main.c` | `paused` flag | Keyframe (special msgs) | Message processing, fallback |
| `tx_power_mutex` | `main.c` | `power_level_0_to_4` | Profile worker (read) | Profile worker (read) |
| `worker_mutex` | `profile.c` | `pending_job`, `job_pending` | Main thread (dispatch) | Profile worker (consume + execute) |
| `worker_cond` | `profile.c` | — | Main thread (signal) | Profile worker (wait) |
| `keyframe_state_t.mutex` | `keyframe.c` | `KeyframeRequest codes[]` | Main thread (add codes) | Main thread (check duplicates) |
| `rssi_state_t.lock` | `rssi_monitor.c` | RSSI circular queue | Main thread (enqueue) | RSSI thread (dequeue) |

The first three mutexes are allocated in `alink_daemon_t` (main.c) and passed by pointer to thread modules. The worker mutex/cond are in `profile_state_t`. The last two are fully encapsulated within their respective module structs.

**Removed:** Cmd server mutex removed with cmd_server module.

---

## Configuration Reference

### config/alink.conf (Drone) - Key Parameters

| Category | Parameter | Default | Description |
|----------|-----------|---------|-------------|
| Power | `allow_set_power` | 1 | Enable TX power control |
| Power | `use_0_to_4_txpower` | 1 | Use power table (vs raw multiplication) |
| Power | `power_level_0_to_4` | 0 | Power index into adapter table |
| Fallback | `fallback_ms` | 1000 | Timeout before entering fallback profile |
| Fallback | `fallback_gi/mcs/fec_k/fec_n/bitrate/gop/power/roi_qp/bandwidth/qp_delta` | — | Fallback profile parameters |
| Keyframe | `allow_request_keyframe` | 1 | Allow IDR frame requests |
| Keyframe | `idr_every_change` | 0 | Request IDR on every profile change |
| TX Monitor | `allow_xtx_reduce_bitrate` | 1 | Reduce bitrate on TX drops |
| TX Monitor | `xtx_reduce_bitrate_factor` | 0.8 | Bitrate reduction factor |
| OSD | `osd_level` | 0 | OSD verbosity (0-6) |
| Misc | `get_card_info_from_yaml` | 1 | Load adapter info from wlan_adapters.yaml |
| Command | `apiCommandTemplate` | — | Batched camera API URL template |
| Command | `http_timeout_ms` | 500 | HTTP request timeout (ms) |

**Removed parameters (moved to GS):** `rssi_weight`, `snr_weight`, `hold_fallback_mode_s`, `hold_modes_down_s`, `min_between_changes_ms`, `hysteresis_percent`, `exp_smoothing_factor`, `allow_dynamic_fec`, `allow_spike_fix_fps`

### config/alink_gs.conf (Ground Station) - Key Parameters

| Section | Parameter | Default | Description |
|---------|-----------|---------|-------------|
| outgoing | `udp_ip` | 10.5.0.10 | Drone IP address |
| outgoing | `udp_port` | 9999 | Drone listen port |
| json | `HOST` | 127.0.0.1 | wfb-ng stats host |
| json | `PORT` | 8103 | wfb-ng stats port |
| weights | `snr_weight` | 0.5 | SNR contribution to RF score |
| weights | `rssi_weight` | 0.5 | RSSI contribution to RF score |
| ranges | `SNR_MIN/MAX` | 12/38 | SNR normalization bounds |
| ranges | `RSSI_MIN/MAX` | -80/-30 | RSSI normalization bounds (dBm) |
| keyframe | `allow_idr` | True | Generate keyframe request codes |
| keyframe | `idr_max_messages` | 20 | Messages to include keyframe code |
| dynamic refinement | `allow_penalty` | False | Apply noise penalty to score |
| noise | `min_noise` | 0.01 | Minimum noise threshold |
| noise | `max_noise` | 0.1 | Maximum noise threshold |
| noise | `deduction_exponent` | 0.5 | Penalty curve exponent |
| error estimation | `kalman_estimate` | 0.005 | Initial Kalman estimate |
| error estimation | `kalman_error_estimate` | 0.1 | Initial Kalman error |
| error estimation | `process_variance` | 1e-5 | Kalman process variance |
| error estimation | `measurement_variance` | 0.01 | Kalman measurement variance |
| scoring | `rf_weight` | 0.5 | RF quality weight |
| scoring | `loss_weight` | 0.25 | Packet loss weight |
| scoring | `fec_weight` | 0.15 | FEC pressure weight |
| scoring | `diversity_weight` | 0.1 | Antenna diversity weight |
| scoring | `max_loss_rate` | 0.1 | Max loss rate for scoring |
| scoring | `max_rssi_spread` | 20 | Max RSSI spread for scoring |
| profile selection | `txprofiles_file` | /etc/txprofiles.conf | Profile file path |
| profile selection | `hold_fallback_mode_ms` | 1000 | Hold time after leaving fallback |
| profile selection | `hold_modes_down_ms` | 3000 | Hold time before upgrades |
| profile selection | `min_between_changes_ms` | 200 | Minimum interval between changes |
| profile selection | `hysteresis_percent` | 5 | Hysteresis for upgrades (%) |
| profile selection | `hysteresis_percent_down` | 5 | Hysteresis for downgrades (%) |
| profile selection | `ema_fast_alpha` | 0.5 | Fast EMA weight |
| profile selection | `ema_slow_alpha` | 0.15 | Slow EMA weight |
| profile selection | `predict_multi` | 1.0 | Predictive multiplier (0 = disabled) |
| profile selection | `fast_downgrade` | True | Immediate downgrades |
| profile selection | `upward_confidence_loops` | 3 | Consecutive evaluations for upgrade |
| profile selection | `limit_max_score_to` | 2000 | Maximum score cap |
| profile selection | `dynamic_mode` | False | Enable dynamic profile calculation |
| **dynamic** | `snr_safety_margin` | 3 | SNR safety margin (dB) |
| **dynamic** | `snr_ema_alpha` | 0.3 | SNR EMA smoothing factor |
| **dynamic** | `loss_margin_weight` | 20 | Loss rate margin multiplier |
| **dynamic** | `fec_margin_weight` | 5 | FEC pressure margin multiplier |
| **dynamic** | `max_mcs` | 7 | Maximum allowed MCS index |
| **dynamic** | `short_gi_snr_margin` | 5 | SNR margin required for short GI |
| **dynamic** | `loss_threshold_for_fec_downgrade` | 0.05 | Loss rate threshold for FEC increase |
| **dynamic** | `utilization_factor` | 0.45 | PHY rate utilization factor |
| **dynamic** | `max_bitrate` | 30000 | Maximum bitrate (kbps) |
| **dynamic** | `min_bitrate` | 2000 | Minimum bitrate (kbps) |
| **dynamic** | `max_power` | 45 | Maximum TX power (dBm) |
| **dynamic** | `min_power` | 30 | Minimum TX power (dBm) |
| **dynamic** | `bandwidth` | 20 | Channel bandwidth (MHz) |
| **dynamic** | `gop` | 10 | Group of pictures |
| **dynamic** | `qp_delta` | -12 | Quantization parameter delta |
| **dynamic** | `roi_qp` | 0,0,0,0 | Region of interest QP values |

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

The ground station script (`ground-station/alink_gs`) is interpreted Python 3 and requires no build step.

### Testing

**C Unit Tests (Unity framework):**
```bash
make test         # Run all Unity tests
make clean        # Clean test artifacts
```

Tests are in `drone/test/test_util.c` covering URL parsing, command formatting, and utility functions.

**Python Tests:**
```bash
python3 -m pytest ground-station/test/test_dynamic_profile.py -v
```

Tests cover MCS selection, guard interval logic, FEC parameters, bitrate computation, power scaling, and integration tests.

### Running Locally (Development)

**Drone daemon:**
```bash
cd drone && make
./alink_drone [listen_ip] [listen_port] [gs_ip]
# Defaults: 10.5.0.10 9999 10.5.0.1
```

Requires:
- `config/alink.conf` (or `/etc/alink.conf`)
- `profiles/default.conf` (or `/etc/txprofiles.conf`)
- `config/wlan_adapters.yaml` (if `get_card_info_from_yaml=1`)

**Ground station:**
```bash
python3 ground-station/alink_gs [--udp-ip IP] [--udp-port PORT] [--verbose]
```

Requires:
- `config/alink_gs.conf` (or `/etc/alink_gs.conf`)
- `profiles/default.conf` (or `/etc/txprofiles.conf`, unless dynamic_mode)
- wfb-ng running with JSON stats enabled on the configured port

---

## Deployment

The `scripts/install.sh` script handles installation on both sides:

### Ground Station
```bash
sudo ./scripts/install.sh gs install    # Fresh install
sudo ./scripts/install.sh gs update     # Update binary only
sudo ./scripts/install.sh gs remove     # Uninstall
```
- Copies `alink_gs` to `/usr/local/bin/`
- Creates systemd service (`alink_gs.service`)
- Configures wfb-ng log interval for stats output

### Drone
```bash
sudo ./scripts/install.sh drone install
sudo ./scripts/install.sh drone update
sudo ./scripts/install.sh drone remove
```
- Copies `alink_drone` to `/usr/bin/`
- Copies configs to `/etc/`
- Adds startup entry to `/etc/rc.local`
- Configures Majestic camera settings (qpDelta, noise level)
- Enables tunnel mode in `datalink.conf`

---

## Technical Debt & Observations

### Architecture

1. **Command execution via fork/exec:** All shell commands use `cmd_exec_with_timeout()` with fork/exec and millisecond-precision timeout via select(). Commands that exceed the timeout (default 500ms) are killed. The legacy `cmd_exec()` and `cmd_exec_noquote()` functions have been removed.

2. **One-directional communication:** The GS sends profiles to the drone, but the drone has no channel to send feedback to the GS (e.g., confirming profile changes, reporting TX drops). This limits coordination.

3. **No structured logging:** Diagnostic output goes to stdout/stderr and OSD. No log levels, no structured format, no rotation.

4. **GS profile selection state:** The GS maintains selection state (EMA values, confidence counters, timing) in Python. If the GS process restarts, this state is lost and must be re-initialized.

5. **Cmd server removed:** The Unix socket IPC (`cmd_server.c`) was removed in commit e2e81e6. External control via socket is no longer available.

### Code Quality

6. **Automated tests added:** Unity framework for C tests (`drone/test/`) and pytest for Python (`ground-station/test/`). Test coverage is improving but not yet comprehensive.

7. **Duplicate config parameters:** Some parameters exist in both `alink.conf` and `alink_gs.conf` (e.g., fallback settings). The GS version is authoritative for selection; the drone version is for local fallback application.

### Robustness

8. **No message authentication:** UDP messages between GS and drone have no authentication or integrity checking. A malicious actor on the network could inject fake profile commands.

9. **Fallback profile timing:** If the GS process restarts, the drone enters fallback mode after `fallback_ms` (default 1000ms) without heartbeats.

10. **No graceful shutdown:** The daemon runs until killed. No signal handlers for cleanup (closing sockets, resetting hardware state).

11. **GS as single point of failure:** With profile selection moved to the GS, the GS process must remain running for adaptive link to function. If it crashes, the drone falls back to the static fallback profile.