# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

OpenIPC Adaptive-Link is an adaptive wireless link profile selector for OpenIPC FPV drone systems. It dynamically adjusts video bitrate, MCS (link speed), FEC, and TX power based on real-time signal quality from the ground station.

Two-component system:
- **Drone side (multi-module C daemon):** Listens for ground station heartbeats containing finalized profile selections, applies transmission parameters via command templates
- **Ground station (`alink_gs`):** Python 3 script that monitors wfb-ng signal metrics, runs multi-factor scoring algorithm, selects transmission profiles, and sends profile parameters to the drone

## Build Commands

```bash
make              # Build alink_drone binary
make clean        # Remove build artifacts
```

The build compiles 13 C source files into a single binary. No external library dependencies beyond libc, pthreads, and libm. Cross-compilation for OpenIPC targets uses `CC` and `OPT` variables passed to make. Compiler flags include `-Wall -Wextra -Werror`.

There are no tests or linting configured in this project.

## Architecture

### Signal Flow

1. Ground station (`alink_gs`) reads RSSI/SNR from wfb-ng JSON stats
2. GS calculates multi-factor score (RF quality, packet loss, FEC pressure, antenna diversity)
3. GS runs profile selection algorithm (dual EMA smoothing, hysteresis, confidence gating)
4. GS sends finalized profile parameters as UDP message to drone
5. Drone (`alink_drone`) applies the profile via command templates

### Drone Daemon Modules & Threads

The drone daemon is split into 13 modules (see source files). Key threads:

| Thread | Module | Entry Point | Purpose |
|--------|--------|-------------|---------|
| Main | `main.c` | `main()` | Non-blocking UDP recv loop (select), parse profile messages |
| Profile Worker | `profile.c` | `profile_worker_func()` | Async command execution for profile changes |
| RSSI Monitor | `rssi_monitor.c` | `rssi_thread_func()` | Drone antenna RSSI parsing |
| Fallback | `fallback.c` | `fallback_thread_func()` | GS heartbeat timeout detection |
| TX Monitor | `tx_monitor.c` | `txmon_thread_func()` | TX drop bitrate reduction |
| OSD | `osd.c` | `osd_thread_func()` | On-screen display updates |
| Cmd Server | `cmd_server.c` | `cmdsrv_thread_func()` | Unix socket command handler |

Module structure:
- `alink_types.h` — shared types and constants
- `util.c` — string/time helpers (includes `util_now_ms()` for millisecond timestamps)
- `config.c` — alink.conf and txprofiles.conf parsing (`alink_config_t`)
- `hardware.c` — WiFi adapter, power tables, camera/video (`hw_state_t`)
- `command.c` — template substitution, system() execution (`cmd_ctx_t`)
- `profile.c` — async profile application only (`profile_state_t`)
- `osd.c` — OSD string assembly and output (`osd_state_t`)
- `keyframe.c` — keyframe request deduplication (`keyframe_state_t`)
- `rssi_monitor.c` — RSSI queue and antenna thread (`rssi_state_t`)
- `message.c` — UDP message parsing (`msg_state_t`)
- `tx_monitor.c`, `fallback.c`, `cmd_server.c` — thread modules

### Profile System

Profiles in `txprofiles.conf` map score ranges to transmission parameters:
```
<score_min> - <score_max> <guard_interval> <mcs> <fecK> <fecN> <bitrate> <gop> <power> <roi_qp> <bandwidth> <qpDelta>
```

Lower scores select conservative long-range profiles (low MCS, low bitrate); higher scores select aggressive short-range profiles (high MCS, high bitrate). Profile 0 is the fallback used when GS heartbeat is lost.

**Profile selection is now performed entirely on the Ground Station.** The drone receives finalized profile parameters and applies them via `profile_apply_direct()`.

### Command Template System

Settings are applied via shell commands with placeholder substitution (`{mcs}`, `{bitrate}`, `{power}`, etc.) defined in `alink.conf`. Templates call into wfb-ng CLI (`wfb_tx_cmd`), the OpenIPC camera API (`curl localhost/api/v1/...`), and `iw` for power control.

### Ground Station Algorithm

The GS implements the full adaptive link algorithm:

- **Multi-factor scoring:** Weights RF quality (RSSI/SNR), packet loss, FEC recovery pressure, and antenna diversity spread
- **Dual EMA smoothing:** Fast EMA (α=0.5) and slow EMA (α=0.15) for trend detection
- **Predictive scoring:** When fast EMA < slow EMA (degrading signal), predicts further decline
- **Hysteresis:** Prevents oscillation when scores hover near profile boundaries
- **Asymmetric stepping:** Fast downgrade (immediate), confidence-gated upgrade (requires 3 consecutive evaluations)
- **Rate limiting:** Minimum interval between profile changes
- **Kalman filtering:** Noise estimation for optional score penalty

### Hardware Abstraction

`wlan_adapters.yaml` defines per-adapter capabilities (MCS support, STBC/LDPC, bandwidth) and TX power tables that map power indices (0-4) to MCS-specific dBm values, abstracting hardware differences across WiFi adapters.

## Key Configuration Files

| File | Deployed to | Purpose |
|------|------------|---------|
| `alink.conf` | `/etc/alink.conf` | Drone daemon settings and command templates |
| `alink_gs.conf` | `/etc/alink_gs.conf` | GS scoring weights, profile selection params, Kalman filter |
| `txprofiles.conf` | `/etc/txprofiles.conf` | Score-to-profile mapping (multiple presets in `txprofiles/`) |
| `wlan_adapters.yaml` | `/etc/wlan_adapters.yaml` | WiFi adapter power tables and capabilities |

## Code Conventions

- Multi-module C daemon (13 `.c` files + headers) with pthreads for concurrency
- State encapsulated in context structs (`alink_config_t`, `hw_state_t`, `profile_state_t`, etc.) passed explicitly
- Naming convention: `modulename_verb_noun()` (e.g., `config_load()`, `profile_apply()`, `hw_get_resolution()`)
- Two mutexes fully encapsulated in their modules (keyframe, rssi_monitor); three shared via `main.c`; one in `profile_state_t` for the async worker
- Functions that can fail return `int` (0 = success) or pointer (`NULL` = failure)
- String operations use bounded `strncpy` with explicit null termination
- System commands executed via `cmd_exec()` / `cmd_format()` after template placeholder substitution
- Compiled with `-Wall -Wextra -Werror`