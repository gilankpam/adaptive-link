# CLAUDE.md

This file provides guidance to AI Agents when working with code in this repository.

## Project Overview

OpenIPC Adaptive-Link is an adaptive wireless link profile selector for OpenIPC FPV drone systems. It dynamically adjusts video bitrate, MCS (link speed), FEC, and TX power based on real-time signal quality from the ground station.

Two-component system:
- **Drone side (multi-module C daemon):** Listens for ground station heartbeats containing finalized profile selections, applies transmission parameters via command templates
- **Ground station (`alink_gs`):** Python 3 script that monitors wfb-ng signal metrics, runs a two-channel gate on physical link quantities (SNR margin + emergency loss/FEC triggers), selects transmission profiles, and sends profile parameters to the drone

## Build Commands

```bash
make              # Build alink_drone binary
make test         # Run Unity tests
make clean        # Remove build artifacts
```

The build compiles 12 C source files into a single binary (in `drone/src/`). No external library dependencies beyond libc, pthreads, and libm. Cross-compilation for OpenIPC targets uses `CC` and `OPT` variables passed to make. Compiler flags include `-Wall -Wextra -Werror`.

The project includes:
- **Unity test framework** for C unit tests (`make test`)
- **Python test suite** for ground station (`ground-station/test/`)
- **ML offline analysis tools** (`ground-station/ml/`)

## Architecture

### Signal Flow

1. Ground station (`alink_gs`) reads RSSI/SNR from wfb-ng JSON stats
2. GS updates per-tick link state (SNR EMA, SNR slope, loss rate, FEC pressure) via `ProfileSelector.evaluate_link()`
3. GS runs the two-channel gate: Channel A (SNR margin in dB with up/down hysteresis + slope prediction) drives normal upgrades/downgrades, Channel B (loss-rate/FEC-pressure emergency triggers) forces immediate downgrades
4. GS sends finalized profile parameters as UDP message to drone
5. Drone (`alink_drone`) applies the profile via command templates

### Drone Daemon Modules & Threads

The drone daemon is split into 12 modules (in `drone/src/`). Key threads:

| Thread | Module | Entry Point | Frequency | Purpose |
|--------|--------|-------------|-----------|---------|
| Main | `main.c` | `main()` | 50ms select timeout | Non-blocking UDP recv loop, parse profile messages |
| Profile Worker | `profile.c` | `profile_worker_func()` | Per job signal | Async command execution for profile changes |
| RSSI Monitor | `rssi_monitor.c` | `rssi_thread_func()` | Per queue item | Drone antenna RSSI parsing, weak antenna detection |
| Fallback | `fallback.c` | `fallback_thread_func()` | Every `fallback_ms` | GS heartbeat timeout detection, fallback profile |
| TX Monitor | `tx_monitor.c` | `txmon_thread_func()` | Every 200ms | TX drop detection, bitrate reduction, keyframe requests |
| OSD | `osd.c` | `osd_thread_func()` | Every 200ms | On-screen display updates (conditional write) |

Module structure:
- `alink_types.h` — shared types and constants (header-only)
- `util.c` — string/time helpers, URL parsing (`util_now_ms()`, `util_parse_url()`)
- `config.c` — alink.conf parsing (`alink_config_t`)
- `hardware.c` — WiFi adapter info, camera/video queries (`hw_state_t`)
- `command.c` — template substitution, timeout execution via fork/exec (`cmd_ctx_t`)
- `profile.c` — async profile application, ROI QP computation (`profile_state_t`)
- `osd.c` — OSD string assembly, channel caching, UDP output (`osd_state_t`)
- `keyframe.c` — keyframe request deduplication, pause/resume (`keyframe_state_t`)
- `rssi_monitor.c` — RSSI queue and antenna thread (`rssi_state_t`)
- `message.c` — UDP message parsing, inter-arrival jitter measurement (`msg_state_t`)
- `tx_monitor.c`, `fallback.c` — thread modules
- `http_client.c` — native socket-based HTTP GET (no curl dependency)

**Performance optimizations:**
- **Conditional OSD writes**: Only write when content changes (avoids disk I/O)
- **Channel caching**: WiFi channel cached for 5 seconds (reduces `iw` calls)
- **Latest-job-wins**: Profile worker picks up new job immediately if one arrives during execution
- **Native HTTP client**: Socket-based HTTP replaces curl subprocess for camera API
- **Jitter measurement**: Inter-arrival jitter computed without clock sync requirement (uses `CLOCK_MONOTONIC` so wall-clock steps can't poison the delta)
- **O(1) RSSI rolling average**: `rssi_monitor.c` keeps a running sum per antenna and updates it incrementally (subtract evicted sample, add new) instead of re-summing the 20-entry ring on every received `RX_ANT` line
- **Edge-triggered weak-antenna log**: The on-screen warning still renders every OSD tick, but the stdout `INFO_LOG` fires only on rising/falling edges — no more 5 lines/sec while the condition holds

### Profile System

Profiles in `profiles/*.conf` map score ranges to transmission parameters:
```
<score_min> - <score_max> <guard_interval> <mcs> <fecK> <fecN> <bitrate> <gop> <power> <roi_qp> <bandwidth> <qpDelta>
```

Lower scores select conservative long-range profiles (low MCS, low bitrate); higher scores select aggressive short-range profiles (high MCS, high bitrate). Profile 0 is the fallback used when GS heartbeat is lost.

**Profile selection is now performed entirely on the Ground Station.** The drone receives finalized profile parameters and applies them via `profile_apply_direct()`.

#### Dynamic Profile Calculation Mode

When `dynamic_mode = True` in `alink_gs.conf`, the GS computes profile parameters from real-time link metrics instead of using a table lookup:

- **MCS selection:** Uses 802.11n SNR thresholds with configurable safety margin that widens under link stress
- **Guard interval:** Short GI selected when SNR margin is comfortable (>5dB default) and loss/FEC pressure are low
- **FEC adjustment:** Frame-aligned algorithm (`_compute_fec_from_bitrate()`) that sizes FEC blocks to match video frame packet count
- **Bitrate computation:** Derived from PHY rate × utilization factor × FEC efficiency (K/N ratio)
- **Power scaling:** Linear inverse scaling with MCS level for link stability
- **MCS step limiting:** Configurable `max_mcs_step_up` prevents rapid upward transitions that cause power-coupling oscillation

This mode makes `profiles/default.conf` optional and provides finer-grained adaptive control.

### Command Template System

Settings are applied via shell commands with placeholder substitution (`{mcs}`, `{bitrate}`, `{power}`, etc.) defined in `config/alink.conf`. Templates call into wfb-ng CLI (`wfb_tx_cmd`), the OpenIPC camera API (via native HTTP client), and `iw` for power control.

**API batching:** The `apiCommandTemplate` batches multiple camera parameters (qpDelta, bitrate, gop, roiQp) into a single HTTP request for efficiency.

**Timeout execution:** All commands use `cmd_exec_with_timeout()` with millisecond-precision timeout via fork/exec. The legacy `cmd_exec()` and `cmd_exec_noquote()` functions have been removed.

**Trust model:** Substituted values flow unescaped into `/bin/sh -c`. The GS is trusted; placeholder values originate from `_compute_profile()` and reference tables, not user input. Templates must come from operator-controlled `/etc/alink.conf` — a malicious config file is equivalent to shell access. The `customOSD` field is the one exception: it feeds a runtime `snprintf`, so `customosd_format_is_safe()` in `config.c` rejects anything but `%d`/`%i`/`%%` and caps the spec count at 2, closing format-string injection. `replace_placeholder()` checks its post-substitution length against `MAX_COMMAND_SIZE` and logs + aborts on overflow instead of silently truncating.

### Ground Station Algorithm

The GS gates profile changes with a two-channel decision on physical link quantities (no synthetic score):

- **`evaluate_link(...)`:** Per-tick state update. Computes `loss_rate` and `fec_pressure` from packet-counter deltas, updates `snr_ema` and an EMA of `Δsnr_ema` (`_snr_slope`) for trend prediction.
- **Channel A — SNR margin (slow/symmetric):** Margin is `snr_ema - MCS_SNR_THRESHOLDS[mcs] - (snr_safety_margin + loss_rate*loss_margin_weight + fec_pressure*fec_margin_weight)`. Upgrades require `margin(candidate) >= hysteresis_up_db` AND `margin + slope*snr_predict_horizon_ticks >= 0`, confirmed across `upward_confidence_loops` ticks. Downgrades fire when `margin(current) <= -hysteresis_down_db`. Uses the same stress-widened margin formula as `_compute_profile()` — single source of truth.
- **Channel B — Emergency triggers (fast/asymmetric):** `loss_rate >= emergency_loss_rate` or `fec_pressure >= emergency_fec_pressure` forces an immediate one-step MCS downgrade, bypassing rate limiting and confidence gating.
- **Temporal gates (preserved):** `hold_fallback_mode_ms`, `hold_modes_down_ms`, `min_between_changes_ms`, `fast_downgrade`, `upward_confidence_loops`, `max_mcs_step_up`. Hold timers key off `last_mcs_change_time_ms` so param-only updates (FEC/bitrate/GI/power at same MCS) don't restart the upgrade clock.
- **Dynamic mode:** `_compute_profile()` maps current link state to MCS/FEC/bitrate/GI/power using 802.11n reference tables.
- **Telemetry logging:** Append-only JSONL logging with outcome tracking for ML training data. Records include `snr_ema`, `snr_slope`, `margin_cur`, `margin_tgt`, `emergency`.

### Telemetry Logging

The `TelemetryLogger` class provides ML-ready data collection:

- **JSONL format:** One record per tick (~10Hz) with link metrics, SNR margins, and gate state
- **Outcome tracking:** Tracks link quality in ticks following profile changes (good/marginal/bad labels)
- **FEC confirmation:** Waits for FEC parameters to be confirmed before starting outcome window
- **Auto-rotation:** Rotates log files at configurable size (default 50MB)
- **Adapter tagging:** Supports multiple adapters with per-adapter log files

### Hardware Abstraction

`config/wlan_adapters.yaml` defines per-adapter capabilities (MCS support, STBC/LDPC, bandwidth) and TX power tables that map power indices (0-4) to MCS-specific dBm values, abstracting hardware differences across WiFi adapters.

## Key Configuration Files

| File | Deployed to | Purpose |
|------|------------|---------|
| `config/alink.conf` | `/etc/alink.conf` | Drone daemon settings and command templates |
| `config/alink_gs.conf` | `/etc/alink_gs.conf` | GS two-channel gate params, profile selection timers, dynamic profile config |
| `profiles/default.conf` | `/etc/txprofiles.conf` | Score-to-profile mapping (multiple presets in `profiles/`) |
| `config/wlan_adapters.yaml` | `/etc/wlan_adapters.yaml` | WiFi adapter power tables and capabilities |

## Code Conventions

- Multi-module C daemon (12 `.c` files + headers) with pthreads for concurrency
- State encapsulated in context structs (`alink_config_t`, `hw_state_t`, `profile_state_t`, etc.) passed explicitly
- Naming convention: `modulename_verb_noun()` (e.g., `config_load()`, `profile_apply()`, `hw_get_resolution()`)
- Two mutexes fully encapsulated in their modules (keyframe, rssi_monitor); two shared via `main.c`; one in `profile_state_t` for the async worker
- Functions that can fail return `int` (0 = success) or pointer (`NULL` = failure)
- String operations use bounded `strncpy` with explicit null termination
- System commands executed via `cmd_exec_with_timeout()` (fork/exec with timeout) after template placeholder substitution
- Legacy `cmd_exec()` and `cmd_exec_noquote()` removed in favor of timeout-based execution
- HTTP requests use native socket-based client (`http_client.c`) instead of curl
- Config parsing is **table-driven** in `config.c`: `CONFIG_KEYS[]` maps key name → struct offset → type tag (`CT_INT`/`CT_BOOL`/`CT_FLOAT`/`CT_STR`/`CT_LOGLEVEL`/`CT_OSDFMT`), dispatched through `apply_config_entry()`. Adding a new config field is a one-line table insert, not a new `else if` arm
- `profile_state_t.prevApplied` is a full `Profile` struct — delta-detection code in `apply_*_step` compares against `ps->prevApplied.field` directly, and adding a new profile field automatically gets a "previous" slot
- Thread-called tokenizers use `strtok_r` (never `strtok`) to stay reentrant — `config_load`, `message.c` parsers, and URL parsing are all reentrant-safe
- Cross-thread flags are declared `volatile` (not `_Atomic` — C99 doesn't have it); all compound state transitions are serialized under a mutex
- Compiled with `-Wall -Wextra -Werror`
- Unity test framework for C unit tests in `drone/test/`
- Python test suite for GS in `ground-station/test/`
- ML offline analysis tools in `ground-station/ml/` (requires numpy, pandas, matplotlib)

## Handshake Mechanism

The GS and drone exchange handshake messages to obtain current video parameters (fps/res) and measure link RTT without needing clock sync:

- **`H:<t1>`** (GS → drone): GS sends timestamp `t1` (milliseconds since its own clock).
- **`I:<t1>:<t2>:<t3>:<fps>:<x_res>:<y_res>`** (drone → GS): Drone replies with:
  - `t1`: Echo of GS timestamp (freshness check — must match the outstanding hello).
  - `t2`: Drone receive timestamp (stamped first thing in `msg_handle_hello`).
  - `t3`: Drone send timestamp (stamped just before `sendto`).
  - `fps`, `x_res`, `y_res`: Camera video parameters, refreshed on every hello (see below).

### Two-stage timer (fast retry + long resync)

`HandshakeClient` in `alink_gs` runs a two-stage timer:
- **Long resync** (default `resync_interval_s = 30`): the happy-path cadence between full successful handshakes.
- **Fast retry** (`fast_retry_ms`, default 500 ms): if the expected `I:` doesn't arrive within the short window, retry up to `max_fast_retries` (default 3) before backing off to the long interval.

The client tracks `_pending_t1` for the outstanding hello. Only replies whose echoed `t1` matches the latest pending hello are accepted; anything else bumps `unmatched_replies` and is dropped (catches duplicates/stale datagrams). Result: a single lost hello/reply recovers in <1 s instead of 30 s. `_drain_handshake_replies` runs unconditionally from the main loop (not gated on `receiving_video`) so replies don't rot in the kernel buffer during a video stall.

### Camera info refreshed on every hello

The drone re-queries camera FPS/resolution via `hw_refresh_camera_info()` inside `msg_handle_hello` (between the `t2` and `t3` stamps). A 2-second TTL cache coalesces bursts during fast retries. This fixes the silent-staleness bug where a runtime camera reconfig would leave the GS sizing FEC for the boot-time FPS forever.

### RTT (clock-offset-cancelling)

The drone clock is **never** synchronized with the GS clock (no NTP, independent boots). Any naive form like `t2 - t1` mixes clocks and leaks the unbounded offset. Instead the GS computes:

```
rtt_ms = (gs_recv_ms - t1) - (t3 - t2)
          └── GS clock ──┘    └─ drone clock ─┘
```

Each subtraction stays on a single clock, so the clock offset cancels. `(t3 - t2)` accounts for drone-side work (including the camera re-query) and is subtracted from the wall round trip, leaving the on-wire RTT. `HandshakeClient._update_rtt()` keeps `last_rtt_ms` and an EMA `avg_rtt_ms` (alpha 0.2), drops negative samples (`rtt_invalid_count`), and surfaces the number on both the GS console and the drone OSD.

Sanity bounds: sub-millisecond on wired loopback, single-digit ms on a healthy wfb-ng link. Hundreds of ms on a working link means clocks got mixed somewhere — treat as a bug, not a measurement.

### Frame bound check (drone parser)

The drone recv loop in `main.c` validates the 4-byte length prefix before dereferencing: rejects datagrams shorter than the prefix, rejects `msg_length == 0` or `msg_length > received - 4`, and null-terminates at `buffer[sizeof(uint32_t) + msg_length]` so downstream `strtok_r`/`strncmp` can't run past the declared payload. `msg_process_profile` also takes an explicit `msg_len` and uses `strndup(msg, msg_len)` rather than `strdup`, so a malformed payload with no interior NUL can't over-read. Defensive only — wfb-ng is trusted — but catches truncation and buggy senders cleanly.

### RTT echoed on `P:` for drone-side display

`P:` profile messages are append-only; the final field is now `rtt_ms`:

```
P:<idx>:<gi>:<mcs>:<fec_k>:<fec_n>:<bitrate>:<gop>:<power>:<bandwidth>:<gs_timestamp_ms>:<rtt_ms>
```

`-1` is the "no sample yet" sentinel. The drone parser's existing `default: break;` makes this fully backward-compatible. The drone OSD renders `jit:Xms rtt:Yms res:Zp` on line 6 when `rtt_ms >= 0`, otherwise falls back to the old `jit:Xms res:Zp`. RTT is **not** used for drone-side decisions — display only.

## Testing

### C Unit Tests
```bash
make test         # Run all Unity tests
make clean        # Clean test artifacts
```

Tests are in `drone/test/`:
- `test_util.c`: URL parsing, command formatting, utility functions
- `test_message.c`: UDP message parsing, handshake handling, jitter measurement

### Python Tests
```bash
python3 -m pytest ground-station/test/ -v
```

Tests cover:
- `test_dynamic_profile.py`: MCS selection, guard interval logic, FEC parameters, bitrate computation, power scaling
- `test_feature_engineering.py`: ML feature computation (SNR ROC, loss acceleration, FEC saturation, etc.)
- `test_telemetry_logger.py`: Telemetry logging, rotation, outcome tracking
- `test_replay_simulator.py`: Offline profile selection simulation
- `test_optimize_params.py`: Bayesian parameter optimization
- `test_handshake.py`: Handshake message encoding/decoding
