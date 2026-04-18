# FEC Calculation in Adaptive-Link

This document explains how Forward Error Correction (FEC) parameters are computed and applied in the adaptive-link system, and how to tune them for your setup.

## How wfb-ng FEC works

wfb-ng uses **Reed-Solomon erasure coding** (Vandermonde matrix). Two parameters control it:

- **K** (fec_k): Number of data packets per FEC block
- **N** (fec_n): Total packets per block (K data + N-K parity)

Any K packets out of N are sufficient to reconstruct the block. So up to **N-K packet losses per block** can be recovered.

Both K and N are `uint8_t` in the wfb-ng session header, so the maximum value is **255**.

### TX side (drone)

Each incoming UDP packet from the video encoder is placed into the current FEC block and **injected over the air immediately** — there is no buffering until the block is full. After K data packets accumulate, N-K parity packets are generated via Reed-Solomon encoding and sent.

```
Packet 1 → inject immediately, fragment_idx=0
Packet 2 → inject immediately, fragment_idx=1
...
Packet K → inject immediately, fragment_idx=K-1
         → generate N-K parity packets
Parity 1 → inject, fragment_idx=K
...
Parity N-K → inject, fragment_idx=N-1
         → block complete, reset for next block
```

**Key point:** FEC blocks are formed **purely sequentially**. There is no awareness of video frame boundaries. Block boundaries are determined solely by the K counter, not by application-layer framing.

### RX side (ground station)

Data packets are **forwarded to the video decoder immediately** as they arrive, as long as there are no gaps from the start of the block. FEC recovery only kicks in when:

1. Gaps exist in the data portion of a block (some data packets were lost)
2. K total fragments (data + parity) have been received for that block

This means **FEC adds zero latency when no packets are lost**. Recovery latency only occurs when packets are actually missing and FEC must reconstruct them.

If fewer than K fragments arrive for a block, FEC cannot recover the missing packets. Any data packets that did arrive are still delivered to the application (partial delivery).

### FEC parameter changes

The drone applies new FEC settings via `wfb_tx_cmd 8000 set_fec -k {K} -n {N}`. This triggers a new wfb-ng session (new session key). The ground station confirms the change by observing the updated `session.fec_k` and `session.fec_n` in the wfb-ng RX stats on the next tick.

## GS FEC computation pipeline

The ground station computes FEC parameters as part of the profile selection loop (~10 Hz):

```
wfb-ng RX stats (JSON via port 8103)
  |
  v
handle_video_rx_stats()
  Extract: all_packets, lost_packets, fec_rec, session fec_k/fec_n, SNR
  |
  v
evaluate_link()
  Compute: loss_rate, fec_pressure, SNR EMA, SNR slope
  |
  v
select()                         -- two-channel gate (SNR margin + emergency)
  |
  v
_compute_profile()
  1. MCS selection (SNR vs threshold + dynamic margin)
  2. PHY rate lookup -> link_bandwidth_bps
  3. target_bitrate = min(link_bw * (1 - fec_redundancy_ratio), max_bitrate)
  4. _compute_fec_from_bitrate(target_bitrate)
       K = ceil(target_bitrate / (fps * 8 * MTU))
       N = ceil(K / (1 - fec_redundancy_ratio))
  5. Adjustments:
       - Loss-triggered: K -= 1 if loss_rate > threshold
       - max_fec_redundancy: raise K if (N-K)/N > limit
       - max_fec_n: clamp N, adjust K proportionally
  6. bitrate = link_bw * K/N
  |
  v
generate_profile_message()
  P:<idx>:<gi>:<mcs>:<K>:<N>:<bitrate>:<gop>:<power>:<bw>:<ts>:<rtt>
  |
  v
send_udp() -> drone -> wfb_tx_cmd set_fec
```

### FEC pressure

FEC pressure measures how much of the available FEC recovery budget is being used:

```
fec_redundancy = N - K                              (parity packets per block)
fec_capacity   = fec_redundancy * (all_packets / N)  (total parity budget this interval)
fec_pressure   = fec_recovered / fec_capacity         (fraction of budget consumed)
```

- **0.0**: No packets recovered — link is clean
- **0.3**: Moderate recovery activity
- **0.7+**: Heavy recovery — link is stressed, triggers emergency downgrade if >= `emergency_fec_pressure`

FEC pressure also widens the SNR safety margin via `fec_margin_weight`, making profile upgrades harder when FEC is working hard.

## MTU assumption and wfb-ng mlink mode

`_compute_fec_from_bitrate()` reads `mtu_payload_bytes` from the `[dynamic]` section of `alink_gs.conf`. The default is **1446 bytes**, which matches a standard 1500-byte Ethernet MTU minus IP/UDP headers — the normal OpenIPC setup where Majestic emits ~1400-byte UDP packets to `wfb_tx`.

**If you run Majestic with wfb-ng mlink and a larger MTU**, set `mtu_payload_bytes` to match. A typical mlink value is **3893 bytes**. wfb-ng itself tolerates payloads up to `MAX_FEC_PAYLOAD ≈ 3996` bytes; larger values are truncated by `wfb_tx`. alink clamps `mtu_payload_bytes` at 3900 with a startup warning.

### Why it matters

K is `ceil(bitrate / (fps × 8 × mtu))`. With the default 1446-byte assumption but actual 3893-byte packets, `packets_per_frame` is computed as ~2.7× larger than reality, so K (and therefore N) is scaled up by the same factor. The consequences are all latency:

- **Recovery / reorder stall doubles-to-triples.** Stall = `K × packet_period`. Inflated K × unchanged per-packet wall time = 2.5–3× longer stalls whenever FEC has to recover a lost packet.
- **`max_fec_n` tuning stops working.** The natural N at high MCS drops from ~44 to ~17 at MTU=3893, so the `max_fec_n=25-30` recommendation further down in this doc becomes non-binding. Operators following that guide on mlink setups won't see the behavior the guide describes.

### Worked example at MTU=3893 (90fps, 20MHz, long GI, utilization 0.7, redundancy 0.25)

| MCS | PHY (Mbps) | video (Mbps) | K (MTU=1446) | Stall (1446) | K (MTU=3893) | Stall (3893) |
|-----|-----------|--------------|--------------|--------------|--------------|--------------|
| 0   | 6.5       | 3.4          | 4            | 37 ms        | 2 (floor)    | 18 ms        |
| 2   | 19.5      | 10.2         | 10           | 31 ms        | 4            | 13 ms        |
| 4   | 39.0      | 20.5         | 20           | 31 ms        | 8            | 12 ms        |
| 5   | 52.0      | 27.3         | 26           | 30 ms        | 10           | 12 ms        |
| 7   | 65.0      | 34.1         | 33           | 31 ms        | 13           | 12 ms        |

At 90fps (11.1 ms per frame), the 30+ ms stall at the default MTU is ~3 frames of video hold — perceptible as stutter whenever the link is lossy. After the fix the stall drops to 12–18 ms (~1–2 frames) and is usually invisible.

### Caveat: burst tolerance also drops

The fixed K/N pairs at mid-high MCS (MCS 4–7) go from 6–10 parity packets per block down to 3–4. A single lost burst can still be recovered, but less slack. **If you see new stutter at mid-MCS after setting `mtu_payload_bytes` correctly, raise `fec_redundancy_ratio` from 0.25 to ~0.33** — a config-only retune. The `K=2` floor at MCS 0 means low-MCS behavior is effectively unchanged.

## Block sizing: why K ~ packets_per_frame

`_compute_fec_from_bitrate()` sets K to `ceil(bitrate / (fps * 8 * MTU))`, which equals the expected number of UDP packets per video frame at the given bitrate. This is **frame-proportional block sizing** — a heuristic for scaling block size with data rate:

- **At low MCS / low bitrate:** Small K (e.g., 4-7) means small blocks. FEC recovery completes quickly when needed. Appropriate for bandwidth-constrained links.
- **At high MCS / high bitrate:** Large K (e.g., 21-35) means large blocks with more parity packets per block. Better burst-loss tolerance. Appropriate for high-throughput links where burst errors are the main threat.

**This is not frame alignment.** wfb-ng blocks are sequential and have no awareness of video frame boundaries. Even when K equals the average packets per frame, block boundaries and frame boundaries drift independently. The benefit is purely about **block size scaling** — matching the FEC block size to the data rate so that the resilience/latency tradeoff is appropriate for the current link conditions.

## The adjustment chain

After the baseline K and N are computed, three adjustments are applied in order:

### 1. Loss-triggered K reduction

```python
if loss_rate > loss_threshold_for_fec_downgrade:
    fec_k = max(2, fec_k - 1)
```

When packet loss exceeds the threshold, K is decreased by 1 while N stays constant. This increases the parity ratio (N-K)/N, giving more redundancy per block. The tradeoff is slightly lower video bitrate (since bitrate = link_bw * K/N).

### 2. Maximum redundancy enforcement

```python
actual_redundancy = (fec_n - fec_k) / fec_n
if actual_redundancy > max_fec_redundancy:
    fec_k = ceil(fec_n * (1 - max_fec_redundancy))
```

Prevents the parity ratio from exceeding `max_fec_redundancy` (default 0.5). This is a safety rail — at 50% redundancy, half of all transmitted packets are parity, which is wasteful. If the loss-triggered reduction pushed redundancy too high, K is raised back.

### 3. max_fec_n cap

```python
if fec_n > max_fec_n:
    original_redundancy_ratio = 1 - (fec_k / fec_n)
    fec_n = max_fec_n
    fec_k = ceil(fec_n * (1 - original_redundancy_ratio))
```

Limits the total block size. When N exceeds the cap, both N and K are shrunk while preserving the redundancy ratio. The configured redundancy percentage is maintained, but the absolute number of parity packets per block decreases.

## Impact of max_fec_n on burst tolerance

This is the most commonly misconfigured parameter. At the same redundancy ratio, **larger blocks tolerate more burst losses**:

| K/N   | Redundancy | Parity/block | Max recoverable burst |
|-------|------------|-------------|----------------------|
| 4/5   | 20%        | 1           | 1 packet             |
| 8/10  | 20%        | 2           | 2 packets            |
| 12/15 | 20%        | 3           | 3 packets            |
| 20/25 | 20%        | 5           | 5 packets            |
| 28/35 | 20%        | 7           | 7 packets            |

When `max_fec_n` forces small blocks at high MCS, you get the same 20% redundancy but only 3 parity packets per block instead of 7. A burst of 4 consecutive lost packets — common on noisy RF links — will cause an irrecoverable block at K/N=12/15 but would be recovered at K/N=28/35.

### Worked example: 90fps, 20MHz bandwidth, 0.7 utilization, 0.2 redundancy

| MCS | PHY (Mbps) | Natural K | Natural N | max_fec_n=15 K | max_fec_n=15 N | Parity lost |
|-----|-----------|-----------|-----------|----------------|----------------|-------------|
| 0   | 6.5       | 4         | 5         | 4              | 5              | none        |
| 1   | 13.0      | 7         | 9         | 7              | 9              | none        |
| 2   | 19.5      | 11        | 14        | 11             | 14             | none        |
| 3   | 26.0      | 14        | 18        | **12**         | **15**         | 18->15 = -3 |
| 4   | 39.0      | 21        | 27        | **12**         | **15**         | 27->15 = -12|
| 5   | 52.0      | 28        | 35        | **12**         | **15**         | 35->15 = -20|
| 7   | 65.0      | 35        | 44        | **12**         | **15**         | 44->15 = -29|

At MCS 3+, `max_fec_n=15` collapses all block sizes to K=12/N=15 with only 3 parity packets, regardless of how much bandwidth is available. This is the likely cause of video stutter at higher MCS levels — burst losses that would be recovered with natural-sized blocks become irrecoverable.

### Recovery latency tradeoff

Larger blocks mean FEC recovery (when needed) takes longer — the receiver must accumulate K fragments before it can reconstruct missing data. At MCS 4 with K=21, N=27:

```
Time to receive 27 packets = 27 * 1446 bytes * 8 bits / 39 Mbps = 0.8 ms
```

This is well under one frame period (11.1 ms at 90fps), so recovery latency from larger blocks is negligible in practice. The burst-tolerance benefit far outweighs the sub-millisecond recovery delay.

## Parity burst problem at high MCS (large blocks near the drone)

There is an important **upper bound** on useful block size. wfb-ng generates parity packets **synchronously and in a burst** (`tx.cpp:684-709`): after K data packets accumulate, `send_packet()` blocks while it:

1. Computes Reed-Solomon parity — CPU-bound, cost proportional to K*(N-K)
2. Injects all N-K parity packets back-to-back in a tight loop
3. Does not return until all parity packets are sent

During this stall, the video encoder continues producing UDP packets that queue in the kernel socket buffer. At high MCS (drone close, high bitrate), the packet rate is highest and the timing is tightest.

### Why large blocks cause smearing near the drone

With K=32, N=40 at MCS 7 (90fps, ~29 packets/frame, ~2600 packets/sec):

- **RS encoding cost:** proportional to 32*8=256 operations
- **Parity burst:** 8 packets injected back-to-back
- **Data arrival rate:** one packet every ~0.38ms
- **Stall duration:** encoding + 8 injections = 2-3ms on a constrained SoC
- During a 2-3ms stall, 6-8 data packets queue in the kernel buffer
- Under I-frame bursts (5-10x normal frame size), multiple block closings happen in rapid succession — the queue can overflow, silently dropping data packets
- Dropped data packets appear as video smearing/artifacts

With K=12, N=15 at the same MCS:

- **RS encoding cost:** proportional to 12*3=36 (7x less CPU)
- **Parity burst:** only 3 packets
- **Stall duration:** sub-millisecond — kernel buffer easily absorbs queued packets

### The tradeoff

| | Small max_fec_n (15) | Medium max_fec_n (25-30) | Large max_fec_n (40+) |
|-|---------------------|-------------------------|----------------------|
| **Near (high MCS)** | Safe TX path, no smearing | Moderate parity bursts | Parity bursts cause TX drops/smearing |
| **Far (low MCS)** | Poor burst tolerance (3 parity) | Good burst tolerance (4-6 parity) | Best burst tolerance (7+ parity) |
| **Encoding CPU** | Low | Moderate | High — may stall data path on constrained SoCs |

## Parameter tuning guide

### fec_redundancy_ratio (default: 0.25)

Fraction of air capacity reserved for FEC parity. Higher = more resilient but lower video bitrate.

| Value | Parity overhead | Use case |
|-------|----------------|----------|
| 0.15  | ~18%           | Clean links, maximum bitrate |
| 0.20  | 20%            | Good links with occasional interference |
| 0.25  | 25%            | General purpose (default) |
| 0.33  | 33%            | Noisy environments, long range |

### max_fec_n (default: 50)

Maximum FEC block size. This parameter has **both a lower and upper bound** for good performance:

- **Too low:** At high MCS, blocks are tiny with few parity packets — poor burst tolerance at distance
- **Too high:** At high MCS, parity bursts overwhelm the drone's TX path — smearing when near
- **Sweet spot:** Large enough for adequate burst tolerance, small enough to avoid TX stalls

Use this table to find the **minimum** N needed per MCS level:

| FPS | Bandwidth | Redundancy | MCS 3 needs N >= | MCS 5 needs N >= | MCS 7 needs N >= |
|-----|-----------|------------|------------------|------------------|------------------|
| 60  | 20 MHz    | 0.25       | 12               | 24               | 30               |
| 90  | 20 MHz    | 0.20       | 18               | 35               | 44               |
| 90  | 20 MHz    | 0.25       | 19               | 38               | 47               |
| 120 | 20 MHz    | 0.25       | 14               | 28               | 36               |

**Recommendation:** For 90fps at 20MHz, `max_fec_n = 25-30` is a good balance — enough burst tolerance at MCS 3-4 (distance) without causing parity burst issues at MCS 5+ (near). If you see smearing when close, lower it. If you see stutter at distance, raise it. The default of 50 may be too high for constrained SoCs at high MCS.

### max_fec_redundancy (default: 0.5)

Safety cap on the parity ratio (N-K)/N. The default of 0.5 (50%) is appropriate for most setups. Only relevant when loss-triggered K reduction pushes redundancy very high.

### loss_threshold_for_fec_downgrade (default: 0.10)

Loss rate threshold that triggers the K-1 adjustment (more parity per block). Lower values make the system more reactive to packet loss; higher values wait for more significant loss before increasing redundancy.

### emergency_fec_pressure (default: 0.75)

FEC pressure threshold for the Channel B emergency downgrade. When FEC is consuming >= 75% of its recovery budget, forces an immediate one-step MCS downgrade regardless of SNR margin or rate limiting.

## Recommended: enable `wfb_tx -T` (fec_timeout)

alink doesn't manage `wfb_tx` process startup, so this tuning lives in the operator's drone service config — but it is the single biggest latency improvement you can make after fixing `mtu_payload_bytes`. By default `wfb_tx` runs with `fec_timeout = 0` (disabled), which means a half-filled FEC block waits indefinitely for the next data packet to close it. During encoder pauses, end-of-GOP lulls, and low-motion scenes, the tail packets of the pending block can stall for **hundreds of milliseconds**.

### What `-T` does

`-T <ms>` enables an idle-triggered block flush. The mechanism (see `wfb-ng/src/tx.cpp` around line 986):

1. wfb-ng tracks `fec_close_ts = (time of last incoming packet) + fec_timeout`.
2. If `fec_close_ts` elapses with no new data, wfb-ng injects a zero-sized pad fragment into the current block via `send_packet(NULL, 0, WFB_PACKET_FEC_ONLY)`.
3. The pad counts as one fragment and bumps `fragment_idx`. If that pushes the block to K, parity is encoded and the block closes normally. Otherwise the block stays open and the timer re-arms for another `fec_timeout` interval.
4. If the block is completely empty (`fragment_idx == 0`), the pad is refused — idle-idle emits nothing, which is correct.

**Net effect:** worst-case tail latency is bounded by `(K − fragment_idx_at_stall) × fec_timeout` instead of "however long until the next packet shows up." This bound is tightest at low MCS where K is small (K=2 at MCS 0 means 1 pad closes the block) and loosens at high MCS where K is larger — but encoder stalls at high MCS are rare, so the common case is well-bounded.

### What to set

| Stream FPS | Recommended `-T` | Rationale                               |
|-----------:|-----------------:|-----------------------------------------|
| 120        |             8 ms | Just above one frame period (8.3 ms)    |
| 90         |            17 ms | Just above one frame period (11.1 ms)   |
| 60         |            25 ms | Just above one frame period (16.7 ms)   |
| 30         |            40 ms | Just above one frame period (33.3 ms)   |

Rule of thumb: slightly longer than one frame period. Too low and the timer fires during normal playback (wasted parity injections, minor overhead). Too high and the bound is loose.

### How to enable

Find your drone's `wfb_tx` invocation — usually in `/etc/systemd/system/wifibroadcast@.service`, a custom init script, or an OpenIPC wrapper. Add `-T 17` (or whatever value matches your fps) to the argument list **before** the positional arguments. Restart `wfb_tx`.

Example (OpenIPC-style):

```
wfb_tx -K /etc/gs.key -M 1 -B 20 -G long -S 1 -L 1 -N 15 -T 17 \
       -k 8 -n 12 -i 7669206 -f data wlan0
```

### How to verify

`wfb_tx` logs `count_p_fec_timeouts` every `log_interval` (default 1 s). Watch the drone stderr or the stats IPC message. You want:

- **Zero or near-zero `count_p_fec_timeouts`** during steady streaming — the timer shouldn't fire during normal playback. If it does, your `-T` is too small for the current inter-packet gap; bump it up.
- **Nonzero `count_p_fec_timeouts`** during encoder pauses (`pkill -STOP majestic; sleep 0.1; pkill -CONT majestic`) or end-of-GOP lulls — this confirms the mechanism is actually running.

### Why this isn't managed by alink

The `-T` value is effectively a function of `fps`, and `fps` doesn't change mid-flight. wfb-ng's control socket only supports `CMD_SET_FEC` and `CMD_SET_RADIO` — there's no runtime command for `fec_timeout` — so runtime retuning would require patching wfb-ng. Static-at-startup is the correct granularity for this parameter.
