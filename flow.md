# Adaptive-Link Drone Daemon - Program Flow

## Overview

This document explains the step-by-step flow of the `main.c` program, which is the entry point for the **alink_drone** daemon. This daemon manages adaptive video link settings for FPV drones by receiving telemetry data via UDP and making real-time adjustments to optimize the video transmission.

---

## Data Structures

The program centers around the `alink_daemon_t` structure that holds all daemon state:

```c
typedef struct {
    alink_config_t cfg;           // Configuration settings
    hw_state_t hw;                // Hardware state
    cmd_ctx_t cmd;                // Command context
    profile_state_t ps;           // Profile state (bitrate, channel settings)
    keyframe_state_t ks;          // Keyframe/I-frame handling
    rssi_state_t rs;              // RSSI monitoring state
    msg_state_t ms;               // Message processing state
    osd_state_t osd;              // On-Screen Display state
    osd_udp_config_t osd_udp;     // OSD UDP output configuration
    
    // Thread synchronization
    pthread_mutex_t count_mutex;  // Protects message_count
    pthread_mutex_t pause_mutex;  // Protects paused flag
    pthread_mutex_t tx_power_mutex; // Protects TX power changes
    
    volatile bool paused;         // Whether adaptive adjustments are paused
    volatile bool initialized;    // Whether daemon has received first message
    volatile int message_count;   // Total messages received
    
    // Network
    int sockfd;                   // Main UDP socket
} alink_daemon_t;
```

---

## Step-by-Step Flow

### Phase 1: Subsystem Initialization

```
main() starts
    │
    ├─► config_set_defaults(&daemon.cfg)
    │   └─► Sets default configuration values
    │
    ├─► hw_init(&daemon.hw)
    │   └─► Initializes hardware state structure
    │
    ├─► keyframe_init(&daemon.ks)
    │   └─► Initializes keyframe handling state
    │
    └─► osd_init(&daemon.osd)
        └─► Initializes OSD (On-Screen Display) state
```

**Mutex and Flag Initialization:**
- Initialize 3 pthread mutexes for thread synchronization
- Set `paused = false`, `initialized = false`, `message_count = 0`

---

### Phase 2: Configuration Loading

```
Load Configuration Files
    │
    ├─► config_load(&daemon.cfg, CONFIG_FILE)
    │   └─► Loads settings from alink.conf
    │
    └─► config_load_profiles(&daemon.cfg, PROFILE_FILE)
        └─► Loads TX profiles from txprofiles/*.conf
```

---

### Phase 3: Command-Line Argument Parsing

The program parses these arguments:

| Argument | Description | Default |
|----------|-------------|---------|
| `--ip` | IP address to bind to | `127.0.0.1` |
| `--port` | UDP port to listen on | `2525` |
| `--verbose` | Enable verbose output | `false` |
| `--pace-exec` | Command execution pacing (ms) | `100` |
| `--osd2udp` | Enable OSD output via UDP (`ip:port`) | Disabled |

**Example:**
```bash
./alink_drone --port 2525 --verbose --pace-exec 100 --osd2udp 192.168.1.100:5601
```

---

### Phase 4: Subsystem Initialization (With Dependencies)

```
Initialize Subsystems
    │
    ├─► cmd_init(&daemon.cmd, pace_exec, verbose_mode)
    │   └─► Sets up command execution context
    │
    ├─► profile_init(&daemon.ps, &cfg, &hw, &cmd)
    │   └─► Initializes profile management
    │
    ├─► rssi_init(&daemon.rs, verbose_mode)
    │   └─► Sets up RSSI monitoring
    │
    └─► msg_init(&daemon.ms, &ps, &ks, &osd, &cfg, &rs, ...)
        └─► Initializes message processor with all dependencies
```

---

### Phase 5: Network Setup

```
Create UDP Socket
    │
    ├─► socket(AF_INET, SOCK_DGRAM, 0)
    │   └─► Creates datagram socket
    │
    ├─► bind(sockfd, server_addr, sizeof(server_addr))
    │   └─► Binds to specified IP:port
    │
    └─► Print "Listening on UDP port X, IP: Y..."
```

**Error Handling:** If socket creation or bind fails, the program prints an error and exits (this typically indicates the wfb tunnel is not working).

---

### Phase 6: Thread Creation

Six worker threads are created to handle different tasks concurrently:

```
Thread Creation
    │
    ├─► cmdsrv_thread_func (Command Listener)
    │   └─► Listens for air_mate commands via socket
    │
    ├─► rssi_thread_func (RSSI Monitor)
    │   └─► Periodically reads and processes RSSI values
    │
    ├─► fallback_thread_func (Fallback Counter)
    │   └─► Counts messages and triggers fallback if needed
    │
    ├─► osd_thread_func (OSD Updater)
    │   └─► Periodically updates OSD with link statistics
    │
    ├─► profile_worker_func (Profile Worker)
    │   └─► Handles async profile change operations
    │
    └─► txmon_thread_func (TX Dropped Monitor)
        └─► Monitors packet loss and triggers adjustments
```

---

### Phase 7: Hardware Configuration

```
Hardware Setup
    │
    ├─► hw_determine_tx_factor() OR hw_load_tx_power_table()
    │   └─► Determines how TX power levels map to hardware
    │
    ├─► hw_load_vtx_info() (if enabled)
    │   └─► Loads LDPC and STBC settings from wfb.yaml
    │
    ├─► hw_get_camera_bin()
    │   └─► Gets camera binning mode from majestic
    │
    ├─► hw_get_resolution()
    │   └─► Gets video resolution (x_res, y_res)
    │
    ├─► osd_adjust_font_size()
    │   └─► Adjusts OSD font based on resolution
    │
    ├─► hw_get_video_fps()
    │   └─► Gets current video FPS from majestic
    │
    └─► hw_setup_roi() (if roi_focus_mode enabled)
        └─► Sets up Region of Interest focus regions
```

---

### Phase 8: Main Event Loop

The core of the program - an infinite loop that processes incoming UDP messages:

```
while (1) {
    │
    ├─► select() with 50ms timeout
    │   └─► Waits for data on UDP socket
    │
    ├─► If data received:
    │   │
    │   ├─► recvfrom() - Receive UDP packet
    │   │
    │   ├─► daemon.initialized = true
    │   │   └─► Signal that daemon is receiving data
    │   │
    │   ├─► pthread_mutex_lock(&count_mutex)
    │   ├─► message_count++
    │   └─► pthread_mutex_unlock(&count_mutex)
    │       └─► Track total messages received
    │
    ├─► Extract message length (first 4 bytes, network byte order)
    │
    ├─► Check message type:
    │   │
    │   ├─► If message starts with "special:"
    │   │   └─► keyframe_handle_special()
    │   │       └─► Handle keyframe/I-frame commands
    │   │
    │   └─► Otherwise
    │       └─► msg_process()
    │           └─► Process regular telemetry message
    │
    └─► If select() error → break loop
}
```

**Message Format:**
```
[4 bytes: length][variable: message data]
```

---

### Phase 9: Cleanup

When the loop exits (typically on error):

```
Cleanup
    │
    ├─► close(daemon.sockfd)
    │   └─► Close main UDP socket
    │
    └─► if (osd_udp.udp_out_sock != -1)
        └─► close(daemon.osd_udp.udp_out_sock)
            └─► Close outgoing OSD socket if open
```

---

## Thread Interaction Diagram

```
┌─────────────────────────────────────────────────────────────┐
│                      Main Thread                             │
│  (UDP Receive Loop - Processes incoming telemetry)          │
└────────────────────┬────────────────────────────────────────┘
                     │
         ┌───────────┼───────────┐
         │           │           │
         ▼           ▼           ▼
    ┌────────┐ ┌────────┐ ┌────────────┐
    │ msg_   │ │keyframe│ │   Mutexes  │
    │process │ │handle  │ │  (shared)  │
    └───┬────┘ └───┬────┘ └─────┬──────┘
        │          │            │
        └──────────┴────────────┘
             │
    ┌────────┴────────┐
    ▼                 ▼
┌──────────┐     ┌──────────┐
│   ps     │     │    ks    │
│ (profile)│     │(keyframe)│
└──────────┘     └──────────┘
     │                │
     └────────┬───────┘
              │
    ┌─────────┴─────────┐
    ▼                   ▼
┌──────────┐      ┌──────────┐
│   osd    │      │   cmd    │
└──────────┘      └──────────┘

Worker Threads:
┌─────────────┐  ┌─────────────┐  ┌─────────────┐
│ cmdsrv_     │  │ rssi_       │  │ fallback_   │
│ thread      │  │ thread      │  │ thread      │
└─────────────┘  └─────────────┘  └─────────────┘

┌─────────────┐  ┌─────────────┐  ┌─────────────┐
│ osd_        │  │ profile_    │  │ txmon_      │
│ thread      │  │ worker      │  │ thread      │
└─────────────┘  └─────────────┘  └─────────────┘
```

---

## Key Functions Called

| Function | Purpose |
|----------|---------|
| `msg_process()` | Parses telemetry messages, extracts link quality metrics |
| `keyframe_handle_special()` | Handles special commands like forcing I-frames |
| `rssi_thread_func()` | Background RSSI monitoring |
| `txmon_thread_func()` | Monitors TX packet drops, triggers profile changes |
| `osd_thread_func()` | Updates OSD with current link statistics |
| `cmdsrv_thread_func()` | Listens for user commands via TCP socket |

---

## Summary

The **alink_drone** daemon is a multi-threaded UDP server that:

1. **Receives** telemetry data from the drone's video transmitter
2. **Processes** messages to extract link quality information (RSSI, packet loss, etc.)
3. **Monitors** signal conditions across multiple threads
4. **Adapts** video settings (bitrate, FEC, etc.) based on link quality
5. **Displays** real-time statistics via OSD
6. **Responds** to user commands for manual control

The main thread handles the UDP receive loop, while worker threads handle monitoring, OSD updates, command processing, and adaptive profile management.