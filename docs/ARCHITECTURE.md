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
  - [Two-Channel Gate (Ground Station)](#two-channel-gate-ground-station)
  - [Profile Selection Algorithm (Ground Station)](#profile-selection-algorithm-ground-station)
  - [Profile Application (Drone)](#profile-application-drone)
  - [Jitter Measurement](#jitter-measurement)
- [Communication Protocols](#communication-protocols)
- [Threading Model](#threading-model)
  - [Performance Optimizations](#performance-optimizations)
- [Concurrency & Shared State](#concurrency--shared-state)
- [Configuration Reference](#configuration-reference)
- [Build, Test & Run](#build-test--run)
- [Deployment](#deployment)
- [Technical Debt & Observations](#technical-debt--observations)

---

## Project Summary

OpenIPC Adaptive-Link is an adaptive wireless link profile selector for OpenIPC FPV drone systems. It dynamically adjusts video bitrate, MCS (modulation/coding scheme), FEC (forward error correction), TX power, and other transmission parameters based on real-time signal quality from the ground station.

**Architecture change (commit a6fcf7b):** Profile selection logic has been offloaded from the drone to the ground station. The GS runs the full selection algorithm — a two-channel gate on SNR margin (dB) plus emergency loss/FEC triggers — and sends finalized profile parameters to the drone. The drone simply applies received profiles.

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
│       ├── test_message.c               # Unity tests for message parsing, handshake, jitter
│       └── unity/                       # Unity test framework
│
├── ground-station/
│   ├── alink_gs                         # Ground station script (Python 3, ~990 lines)
│   ├── ml/                              # ML offline analysis tools
│   │   ├── analyze_telemetry.py         # Telemetry analysis and visualization
│   │   ├── feature_engineering.py       # Feature computation for ML
│   │   ├── optimize_params.py           # Bayesian parameter optimization
│   │   ├── replay_simulator.py          # Offline profile selection simulation
│   │   └── __init__.py
│   └── test/
│       ├── test_dynamic_profile.py      # Python tests for GS dynamic mode
│       ├── test_feature_engineering.py  # Python tests for ML feature engineering
│       ├── test_telemetry_logger.py     # Python tests for telemetry logging
│       ├── test_handshake.py            # Python tests for handshake protocol and session tracking
│       ├── test_replay_simulator.py     # Python tests for replay simulator
│       └── test_optimize_params.py      # Python tests for parameter optimization
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

**Note:** Profile selection logic (two-channel gate, hysteresis, confidence gating) lives entirely on the GS. The drone just applies received profiles.

### Ground Station (alink_gs)

A Python 3 script that:

1. Connects to wfb-ng's JSON statistics TCP port
2. Extracts per-antenna RSSI, SNR, FEC recovery, and packet loss metrics
3. Updates per-tick link state in `ProfileSelector.evaluate_link()`: SNR EMA, SNR slope EMA, loss rate, FEC pressure (from packet-counter deltas)
4. Runs the two-channel gate in `ProfileSelector.select()`: Channel A (SNR margin with up/down dB hysteresis + slope-based prediction) for normal transitions, Channel B (emergency loss/FEC triggers) for immediate fail-safe downgrades
5. Applies temporal gating: confidence loops, hold timers, rate limiting, max MCS step-up
6. Computes profile parameters dynamically via `_compute_profile()` (MCS from SNR margin, FEC from bitrate, bitrate from PHY rate × utilization × K/N, power from inverse MCS scaling)
7. Sends UDP messages with finalized profile parameters to the drone (~10 Hz)
8. Generates keyframe request codes on packet loss events
9. Logs telemetry data for ML training (optional, JSONL format)

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
```

**Constants:**
```python
VIDEO_FPS = 60              # Fixed video frame rate
MTU_PAYLOAD_BYTES = 1446    # WFB-NG MTU payload byte size
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

**FEC Calculation (Frame-Aligned Algorithm):**
```
1. Packets per frame:
   packets_per_frame = bitrate_bps / (VIDEO_FPS * 8 * MTU_PAYLOAD_BYTES)

2. K = ceil(packets_per_frame) for minimal latency:
   fec_k = max(2, ceil(packets_per_frame))

3. Apply configurable redundancy ratio:
   fec_n = ceil(fec_k / (1 - fec_redundancy_ratio))
   # For 33% redundancy: N = K / 0.67 ≈ K * 1.5

4. Increase redundancy under loss:
   if loss_rate > loss_threshold_for_fec_downgrade:
       fec_k = max(2, fec_k - 1)

5. Enforce max_fec_redundancy:
   actual_redundancy = (fec_n - fec_k) / fec_n
   if actual_redundancy > max_fec_redundancy:
       fec_k = ceil(fec_n * (1 - max_fec_redundancy))

6. Enforce max_fec_n cap:
   if fec_n > max_fec_n:
       original_ratio = 1 - (fec_k / fec_n)
       fec_n = max_fec_n
       fec_k = ceil(fec_n * original_ratio)
```

**Bitrate Computation:**
```
1. Get PHY rate for bandwidth and GI:
   phy_rates = PHY_RATES_40MHZ if bandwidth == 40 else PHY_RATES_20MHZ
   phy_rate = phy_rates[mcs][1 if short_gi else 0]

2. Raw air capacity (PHY rate × utilization factor):
   link_bandwidth_bps = phy_rate * 1000 * 1000 * utilization_factor

3. Target video bitrate (reserve redundancy_ratio for FEC overhead):
   target_bitrate_bps = link_bandwidth_bps * (1 - fec_redundancy_ratio)

4. Compute FEC from target bitrate (frame-aligned):
   fec_k, fec_n = _compute_fec_from_bitrate(target_bitrate_bps)

5. Final bitrate accounts for actual FEC overhead:
   bitrate = link_bandwidth_bps * (fec_k / fec_n) / 1000

6. Clamp to configured range:
   bitrate = max(min_bitrate, min(max_bitrate, bitrate))
```

**Power Scaling (Linear):**
```
TX power linearly scaled inversely with MCS level:
   t = clamp(mcs / max_mcs, 0.0, 1.0)
   power = max_power - t * (max_power - min_power)
```

**MCS Step Limiting:**
```
Prevent rapid upward transitions that cause power-coupling oscillation:
   if (new_idx > current_idx and max_mcs_step_up > 0):
       new_idx = min(new_idx, current_idx + max_mcs_step_up)
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

**Trust model:** `cmd_format()` substitutes placeholder values unescaped into the resulting command line, which is then fed to `/bin/sh -c`. This is safe because placeholder values originate from `_compute_profile()` and reference tables on the GS — not from user input — and `/etc/alink.conf` itself is operator-controlled. A malicious config file is equivalent to shell access. The one place config values *do* flow into a runtime format string is `customOSD`, which is validated by `customosd_format_is_safe()` in `config.c`: only `%d`/`%i`/`%%` are accepted and the conversion-specifier count is capped at 2, closing format-string injection at the OSD `snprintf` site. `replace_placeholder()` compares its post-substitution length to `MAX_COMMAND_SIZE` and logs an error + aborts instead of silently truncating an oversized command.

This decouples the control logic from hardware-specific commands, making it adaptable to different camera/radio stacks.

---

## Data Flow

### Signal Flow

```
wfb-ng RX stats ──TCP──> alink_gs ──UDP (P: message)──> alink_drone ──system()/http_get()──> wfb_tx_cmd / camera API / iw
     │                      │
  RSSI, SNR,          Two-channel gate (SNR margin + emergency) +
  FEC, loss           Dynamic profile computation
```

### Two-Channel Gate (Ground Station)

The GS gates profile changes on physical link quantities — no synthetic score. Per tick:

```
evaluate_link(best_snr, all_pkts, lost_pkts, fec_rec, fec_k, fec_n):
  # Packet-counter deltas
  loss_rate    = Δlost / max(Δall, 1)
  fec_pressure = Δfec_rec / fec_capacity                       # clamped [0, 1]
  snr_ema      = α_snr * best_snr + (1-α_snr) * snr_ema
  _snr_slope   = α_slope * Δsnr_ema + (1-α_slope) * _snr_slope
```

The margin helper reuses the exact stress-widened formula from `_compute_profile()`:

```
_margin(mcs) = snr_ema
               - MCS_SNR_THRESHOLDS[mcs]
               - (snr_safety_margin
                  + loss_rate * loss_margin_weight
                  + fec_pressure * fec_margin_weight)
```

### Profile Selection Algorithm (Ground Station)

The `ProfileSelector.select()` method runs the two-channel gate:

```
1. CHANNEL B: EMERGENCY DETECTION
   emergency = (loss_rate >= emergency_loss_rate) OR
               (fec_pressure >= emergency_fec_pressure)

2. RATE LIMITING (emergencies bypass)
   Skip if not emergency and elapsed < min_between_changes_ms

3. CANDIDATE PROFILE
   new_profile = _compute_profile()
   new_idx     = new_profile.mcs

4. EMERGENCY CLAMP
   if emergency and current_idx > 0:
       new_idx = min(new_idx, current_idx - 1)       # force one-step down
       new_profile = _compute_profile(mcs_override=new_idx)

5. STEP-LIMIT UPGRADE (max_mcs_step_up)
   Cap upgrade to current + max_mcs_step_up

6. CHANNEL A: SNR-MARGIN GATE (skipped when emergency)
   If upgrading:
     predicted = _margin(new_idx) + _snr_slope * snr_predict_horizon_ticks
     Reject unless _margin(new_idx) >= hysteresis_up_db AND predicted >= 0
   If downgrading:
     Reject unless _margin(current_idx) <= -hysteresis_down_db

7. SAME-MCS PATH
   If new_idx == current_idx: update params only (FEC/bitrate/GI/power)

8. CONFIDENCE GATING (upgrades)
   Require upward_confidence_loops consecutive ticks selecting same higher MCS

9. HOLD TIMERS (use last_mcs_change_time_ms — param-only updates don't reset it)
   - hold_fallback_mode_ms when leaving MCS 0
   - hold_modes_down_ms before applying a new upgrade

10. APPLY + SEND
    Update last_change_time_ms; if MCS changed, update last_mcs_change_time_ms.
    Send P: message with profile index and all parameters.
```

### Profile Application (Drone)

The drone receives `P:` messages and applies profiles via `profile_apply_direct()`:

```
1. Parse P: message (profile_idx, gi, mcs, fec_k, fec_n, bitrate, gop, power, bandwidth, gs_ts_ms, rtt_ms)

2. Skip if profile_index == currentProfile (GS sends current profile every tick for reliability)

3. Update tracking state:
   previousProfile = currentProfile
   currentProfile = profile_index
   prevTimeStamp = now_ms()

4. Compute ROI QP on drone side (if not provided by GS):
   - calc_roiQp_from_bitrate() computes ROI QP based on bitrate and resolution
   - Higher bitrate = lower ROI QP (better quality in center region)

5. Dispatch to async worker thread (non-blocking)
   - Apply commands in order: QpDelta → FPS → Power → GOP → MCS → FEC/Bitrate → ROI → IDR
   - Execute via cmd_exec_with_timeout() (500ms default timeout)
   - Pace execution with configurable delays
   - Use native HTTP client for camera API requests
   - Latest-job-wins: If new job arrives during execution, pick it up immediately
```

### Jitter Measurement

The drone measures **inter-arrival jitter** between GS messages without requiring clock synchronization. This is distinct from RTT (see [Handshake Mechanism](#handshake-mechanism)) — jitter captures delivery-interval variability, RTT captures on-wire round-trip time.

```
1. For each P: message, extract the GS-side timestamp from the trailing field.

2. Compute paired deltas on independent clocks so the GS↔drone offset cancels:
   drone_delta = drone_recv_ms[i] - drone_recv_ms[i-1]    (drone clock)
   gs_delta    = gs_send_ms[i]    - gs_send_ms[i-1]       (GS clock)
   jitter      = |drone_delta - gs_delta|

3. Smooth with an EMA (alpha = 0.1) into avg_jitter_ms.

4. Display on OSD line 6 as "jit:Xms" alongside rtt (if available) and resolution.

Benefits:
- No clock sync required between GS and drone (only a same-clock delta from each side)
- Detects network congestion, GS CPU spikes, radio retries
```

**One-way latency** is mathematically impossible without a shared time source. Instead the handshake carries a clock-offset-cancelling RTT (see [Handshake Mechanism](#handshake-mechanism)) which is the only honest latency number available between unsynchronized clocks.

---

## Communication Protocols

### UDP Messages (GS to Drone)

```
Wire format: <4-byte length (network byte order)><payload string>

Profile message (P:):
P:<profile_idx>:<gi>:<mcs>:<fec_k>:<fec_n>:<bitrate>:<gop>:<power>:<bandwidth>:<gs_ts_ms>:<rtt_ms>

Example: P:5:long:5:8:12:6000:10:3000:20:1700000000:8

  - gs_ts_ms: GS wall-clock timestamp in ms, used by the drone for paired-delta jitter
  - rtt_ms:   GS-computed handshake RTT echoed back for drone OSD display; -1 = no sample yet.
              Added as a trailing field; the drone parser's default: case makes this
              backward-compatible with older GS builds.

Handshake (H:):
H:<t1>                              # GS → drone: t1 on GS clock (ms)

Handshake reply (I:):
I:<t1>:<t2>:<t3>:<fps>:<x_res>:<y_res>:<session_id>
                                    # drone → GS: echo + drone-clock t2/t3 + video params + session ID
  - session_id: Random uint32 generated once at daemon startup. Lets the GS detect
                drone restarts and rotate telemetry logs. Backward-compatible trailing
                field — old GS versions ignore it.

Keyframe request (K:):
K:                                     # GS → drone: trigger keyframe request

Frame bound check:
The drone main.c recv loop rejects datagrams shorter than the 4-byte prefix,
rejects msg_length==0 or msg_length > (received - 4), and null-terminates the
buffer at the end of the DECLARED payload so downstream strtok_r/strncmp can't
run past a truncated frame. msg_process_profile takes an explicit msg_len and
uses strndup(msg, msg_len) rather than strdup, so a payload with no interior
NUL can't over-read. Defensive only — wfb-ng is trusted — but catches buggy
senders cleanly.
```

The GS sends the current profile on every tick for UDP reliability. The drone skips application if the profile index matches the current profile.

### Handshake Mechanism

The handshake is a thin probe that fetches current camera parameters, measures on-wire RTT without requiring clock sync, and carries a session ID for drone restart detection.

**Flow:**

1. **GS sends `H:<t1>`**: `t1` is the GS clock at send time.
2. **Drone replies `I:<t1>:<t2>:<t3>:<fps>:<x_res>:<y_res>:<session_id>`**:
   - `t1`: echoed back for freshness matching on the GS side
   - `t2`: stamped *first* in `msg_handle_hello` (before any parsing work)
   - `t3`: stamped *just before* `sendto` (after `hw_refresh_camera_info`)
   - `fps`, `x_res`, `y_res`: re-queried from the camera API on each hello (via `hw_refresh_camera_info()` with a 2-second TTL cache to coalesce fast-retry bursts)
   - `session_id`: random `uint32_t` generated once at daemon startup from `/dev/urandom` (fallback: `getpid() ^ time(NULL)`). When `HandshakeClient` sees a different session_id than the previous handshake, it sets a `session_changed` flag. The main loop consumes this via `pop_session_changed()` and calls `TelemetryLogger.new_session()` to rotate the telemetry log, giving each drone session its own clean file for ML training. Backward-compatible: old GS versions ignore the extra field; new GS with old drone gets no session_id and never triggers rotation.

**Two-stage timer (`HandshakeClient` in `alink_gs`):**

- Happy path fires one hello every `resync_interval_s` (default 30 s).
- On send, the client stores `_pending_t1` and starts a `fast_retry_ms` timer (default 500 ms). If the timer expires without a matching reply, it retries up to `max_fast_retries` times at the short interval, then backs off to the next long-resync slot.
- Replies are matched by echoed `t1`; mismatched/duplicate replies bump `unmatched_replies` and are dropped — this catches stale datagrams drained from the kernel buffer that correspond to an earlier hello in the same burst.
- `_drain_handshake_replies` runs unconditionally from the main loop (not gated on `receiving_video`), so replies don't rot in the kernel buffer during a video stall.

A single lost hello or reply now recovers in <1 s instead of 30 s.

**Clock-offset-cancelling RTT:**

The drone and GS clocks are **never** synchronized (no NTP, independent field boots). Any naive difference like `t2 - t1` leaks the unbounded offset. RTT is instead computed on the GS using only same-clock subtractions:

```
rtt_ms = (gs_recv_ms - t1) - (t3 - t2)
          └── GS clock ──┘    └─ drone clock ─┘
```

`(gs_recv_ms - t1)` is the wall round trip on the GS clock. `(t3 - t2)` is drone-side processing on the drone clock. Subtracting leaves the on-wire RTT with no offset term. `HandshakeClient._update_rtt()` keeps `last_rtt_ms` and an EMA `avg_rtt_ms` (alpha 0.2); negative samples are dropped into `rtt_invalid_count`.

RTT is surfaced three ways:
- `rtt_ms` field in the JSONL telemetry record (per tick).
- Verbose GS console line suffix `| rtt:Xms`.
- Echoed on the trailing field of every `P:` profile message so the **drone** can render `rtt:Xms` on OSD line 6 (alongside jitter and resolution). The drone does **not** use RTT for any decisions — display only.

Sanity: sub-millisecond on wired loopback, single-digit ms on a healthy wfb-ng link. Hundreds of ms on a working link means clocks got mixed somewhere; treat as a bug.

**Note:** Handshake messages are NOT counted as heartbeats for fallback detection.

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
| Main | `drone/src/main.c` | `main()` | 50ms select timeout | Non-blocking UDP recv loop, parse profile messages |
| Profile Worker | `drone/src/profile.c` | `profile_worker_func()` | Per job signal | Async command execution for profile changes |
| RSSI Monitor | `drone/src/rssi_monitor.c` | `rssi_thread_func()` | Per queue item | Drone antenna RSSI parsing, weak antenna detection |
| Fallback | `drone/src/fallback.c` | `fallback_thread_func()` | Every `fallback_ms` (default 1000ms) | GS heartbeat timeout detection, fallback profile |
| TX Monitor | `drone/src/tx_monitor.c` | `txmon_thread_func()` | Every 200ms | TX drop detection, bitrate reduction, keyframe requests |
| OSD | `drone/src/osd.c` | `osd_thread_func()` | Every 200ms | On-screen display updates (conditional write) |

**Removed:** Cmd Server thread (`cmd_server.c`) - Unix socket IPC removed in commit e2e81e6.

### Performance Optimizations

The drone daemon includes several performance optimizations to reduce CPU and I/O overhead:

1. **Conditional OSD writes**: The OSD thread only writes to `/tmp/MSPOSD.msg` when content changes. This avoids unnecessary disk I/O during stable operation when profile parameters haven't changed.

2. **Channel caching**: WiFi channel information is cached for 5 seconds before re-querying via `iw dev wlan0 info`. This reduces system call overhead since channel changes are rare during normal operation.

3. **Latest-job-wins**: The profile worker thread checks for new jobs before and during command execution. If a new profile arrives while executing commands, the worker immediately switches to the new profile, preventing stale parameter application.

4. **Native HTTP client**: Socket-based HTTP requests (`http_client.c`) replace curl subprocess calls, eliminating process spawn overhead and reducing latency for camera API requests.

5. **API batching**: Multiple camera parameters (qpDelta, bitrate, gop, roiQp) are batched into a single HTTP request via `profile_apply_api_batch()`, reducing the number of network round-trips.

6. **Jitter measurement**: Inter-arrival jitter is computed from message timestamps without requiring clock synchronization between GS and drone. The drone-side component uses `CLOCK_MONOTONIC` so wall-clock steps can't poison the delta.

7. **O(1) RSSI rolling average**: `rssi_monitor.c` keeps a running sum per antenna and updates it incrementally (subtract the sample being evicted from the ring, add the new one) instead of re-summing `RSSI_HISTORY_SIZE = 20` entries on every `RX_ANT` line. At 4 antennas × ~10 Hz this trims ~800 adds/sec to ~80.

8. **Edge-triggered weak-antenna log**: The OSD renders the weak-antenna warning every tick (5 Hz), but the stdout `INFO_LOG` line only fires when the boolean flips — rising edge logs "Weak drone antenna detected!", falling edge logs "Drone antenna recovered." No more 5 identical lines/second while the condition holds.

9. **Table-driven config loader**: `config_load()` uses a static `CONFIG_KEYS[]` dispatch table (key name → struct offset + type tag) instead of a hand-written `else if` chain. Load-time cost is trivially different; the win is that adding a config field is a one-line table insert, not a new branch arm.

10. **Collapsed previous-value tracking**: `profile_state_t` keeps a single `Profile prevApplied` struct instead of 8 scattered `prevSet*` fields. Delta detection in every `apply_*_step` compares directly against `ps->prevApplied.field`, and adding a new profile field automatically gets a "previous" slot. No semantic change — one source of truth.

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
| keyframe | `allow_idr` | True | Generate keyframe request codes |
| keyframe | `idr_max_messages` | 4 | Messages to include keyframe code |
| keyframe | `idr_send_interval_ms` | 20 | Interval between keyframe messages |
| profile selection | `hold_fallback_mode_ms` | 1000 | Hold time after leaving fallback |
| profile selection | `hold_modes_down_ms` | 3000 | Hold time before upgrades |
| profile selection | `min_between_changes_ms` | 200 | Minimum interval between changes |
| profile selection | `fast_downgrade` | True | Immediate downgrades |
| profile selection | `upward_confidence_loops` | 3 | Consecutive ticks required for an upgrade |
| **gate** | `hysteresis_up_db` | 2.5 | dB headroom above threshold before upgrade fires |
| **gate** | `hysteresis_down_db` | 1.0 | dB below threshold before downgrade fires |
| **gate** | `snr_slope_alpha` | 0.3 | EMA α for Δsnr_ema trend tracking |
| **gate** | `snr_predict_horizon_ticks` | 3 | Lookahead ticks for predictive upgrade gate |
| **gate** | `emergency_loss_rate` | 0.15 | Loss rate that forces immediate one-step downgrade |
| **gate** | `emergency_fec_pressure` | 0.75 | FEC pressure that forces immediate one-step downgrade |
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
| **dynamic** | `fec_redundancy_ratio` | 0.33 | FEC redundancy ratio (N-K)/N |
| **dynamic** | `max_fec_redundancy` | 0.5 | Maximum allowed FEC redundancy |
| **dynamic** | `max_fec_n` | 50 | Maximum FEC block size |
| **dynamic** | `max_mcs_step_up` | 1 | Maximum MCS steps per upgrade |
| **dynamic** | `video_fps_default` | 90 | Default FPS when handshake not synced |
| **handshake** | `resync_interval_s` | 30 | Seconds between successful handshake probes (long cadence) |
| **handshake** | `fast_retry_ms` | 500 | Fast-retry timer when a hello has no matching reply |
| **handshake** | `max_fast_retries` | 3 | Max fast retries before backing off to long cadence |
| **telemetry** | `log_enabled` | True | Enable telemetry logging |
| **telemetry** | `log_dir` | /var/log/alink | Log directory path |
| **telemetry** | `log_rotate_mb` | 50 | Rotate logs at size (MB) |
| **telemetry** | `outcome_window_ticks` | 10 | Ticks to observe after profile change |
| **telemetry** | `adapter_id` | default | Adapter identifier for multi-adapter support |
| **optimizer** | `skip_optimize_params` | (empty) | Comma-separated params to freeze during optimization |

---

## ML Offline Analysis Tools

The `ground-station/ml/` directory contains offline analysis tools for ML-driven optimization:

### analyze_telemetry.py
Loads telemetry JSONL files and generates diagnostic plots:
- RSSI vs SNR relationship (scatter, density, time series)
- Loss rate vs FEC pressure relationship
- MCS vs SNR analysis with box plots
- Antenna diversity analysis
- SNR margin time series
- MCS transition matrix heatmap
- Feature-outcome correlation matrix
- Failure mode analysis

### feature_engineering.py
Computes derived features from telemetry data:
- `snr_roc`: SNR rate of change (first derivative)
- `loss_accel`: Loss rate acceleration (second derivative)
- `fec_saturation`: FEC pressure proximity to saturation
- `snr_margin_volatility`: Rolling standard deviation of SNR margin
- `link_budget_margin`: SNR above MCS threshold
- `time_since_change`: Elapsed time since last profile change

### optimize_params.py
Bayesian parameter optimization for GS configuration parameters.

### replay_simulator.py
Offline profile selection simulation using historical telemetry data.

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
python3 -m pytest ground-station/test/ -v
```

Tests cover:
- `test_dynamic_profile.py`: MCS selection, guard interval logic, FEC parameters, bitrate computation, power scaling
- `test_feature_engineering.py`: ML feature computation (SNR ROC, loss acceleration, FEC saturation, etc.)
- `test_telemetry_logger.py`: Telemetry logging, rotation, outcome tracking
- `test_replay_simulator.py`: Offline profile selection simulation
- `test_optimize_params.py`: Bayesian parameter optimization

**ML Tools (requires numpy, pandas, matplotlib):**
```bash
# Analyze telemetry data
python3 ground-station/ml/analyze_telemetry.py --input /var/log/alink --output ./analysis-output
```

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