# FEC Calculation in Adaptive-Link

This document explains how Forward Error Correction (FEC) parameters are computed and applied in the adaptive-link system, and how to tune them for your setup.

## How wfb-ng FEC works

wfb-ng uses **Reed-Solomon erasure coding** (Vandermonde matrix). Two parameters control it:

- **K** (fec_k): Number of data packets per FEC block
- **N** (fec_n): Total packets per block (K data + N-K parity)

Any K packets out of N are sufficient to reconstruct the block. So up to **N-K packet losses per block** can be recovered.

Both K and N are `uint8_t` in the wfb-ng session header, so the maximum value is **255**.

### TX side (drone)

Each source UDP packet from the video encoder is **encrypted and injected over the air immediately on arrival** — the in-memory block buffer is only a scratchpad used later to compute parity (see `wfb-ng/src/tx.cpp::send_packet`, around L651-710). After K source packets accumulate, N-K parity packets are generated via Reed-Solomon encoding and sent:

```
Packet 1 → injected over the air, fragment_idx=0
Packet 2 → injected over the air, fragment_idx=1
...
Packet K → injected over the air, fragment_idx=K-1
         → compute N-K parity packets in-memory
Parity 1 → injected, fragment_idx=K
...
Parity N-K → injected, fragment_idx=N-1
         → block complete, reset for next block
```

**Key point 1:** FEC blocks are formed **purely sequentially**. There is no awareness of video frame boundaries.

**Key point 2:** Source packets do **not** wait for the block to fill. Only parity is delayed until K arrivals accumulate. This means **on a clean link, K has no latency cost at all.**

### RX side (ground station)

Source packets are **forwarded to the video decoder the instant they arrive in-order** (`wfb-ng/src/rx.cpp` around L774-792). FEC recovery only kicks in when:

1. A gap exists in the source portion of a block (some source packets were lost)
2. K total fragments (source + parity) have been received for that block, allowing `apply_fec()` to reconstruct the missing ones

Until the block can be decoded, every packet that arrived after the gap sits in the RX ring buffer. **The only FEC-induced latency is this post-loss recovery wait**, and it is bounded by how long the TX took to complete the block.

If fewer than K fragments ever arrive for a block, FEC cannot recover. Any data packets that did arrive are still delivered to the application (partial delivery).

### FEC parameter changes

The drone applies new FEC settings via the wfb-ng control port (default 8000) with a `SET_FEC` request. This triggers a new wfb-ng session (new session key). The ground station confirms the change by observing the updated `session.fec_k` / `session.fec_n` in the wfb-ng RX stats on the next tick.

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
       packets_per_sec = bitrate / (8 * MTU)
       k_by_latency    = round(block_latency_budget_ms/1000 * packets_per_sec)
       k_by_frame      = round(packets_per_sec / fps)   -- one-block-per-frame cap
       K = max(2, min(k_by_latency, k_by_frame))
       N = max(K+1, ceil(K / (1 - fec_redundancy_ratio)))
  5. Adjustments:
       - Loss-triggered: N += 1 if loss_rate > threshold
       - max_fec_redundancy: raise K if (N-K)/N > limit
       - max_fec_n: clamp N, adjust K proportionally
  6. bitrate = link_bw * K/N
  |
  v
generate_profile_message()
  P:<idx>:<gi>:<mcs>:<K>:<N>:<bitrate>:<gop>:<power>:<bw>:<ts>:<rtt>
  |
  v
send_udp() -> drone -> cmd_wfb_set_fec(K, N)
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

## Block sizing: latency-targeted, not frame-proportional

The size of K is chosen so that **block-completion time stays constant across MCS**. This directly controls the only FEC-induced latency that exists — the RX wait after a loss for the TX block to complete so FEC can decode.

```
t_block = K × packet_interval = K × (8 × MTU / bitrate)
```

Using a latency target:

```
K ≈ block_latency_budget_ms/1000 × packets_per_sec
```

makes `t_block ≈ block_latency_budget_ms` regardless of MCS. An earlier implementation sized K to one-block-per-frame (`K = bitrate / (fps × 8 × MTU)`), which mathematically pinned `t_block` at one frame period — a **~17 ms recovery penalty at 60 fps on every packet loss, regardless of MCS**. The latency-targeted formula collapses that to the configured budget (default 5 ms) and makes post-loss recovery largely invisible.

A second cap (`K ≤ packets_per_frame`) prevents an absurdly generous latency budget from oversizing the block beyond a single video frame — there's no benefit to parity that straddles frames for FEC decoding purposes.

At the K=2 floor (very low bitrates), block-completion time is set by the floor instead of the budget — at 4 Mbps / 60 fps / 1400-byte packets, two packets takes ~5.6 ms to arrive, close to the 5 ms budget anyway.

### What changes across MCS (default 5 ms budget, MTU=1400, 60 fps)

| MCS | PHY (Mbps) | Target bitrate | packets/sec | K   | N  | t_block |
|-----|-----------:|---------------:|------------:|----:|---:|--------:|
| 0   |  6.5       |     4.2 Mbps   |     375     | 2   | 3  | ~5.3 ms |
| 3   | 26.0       |    15.0 Mbps   |    1339     | 7   | 9  | ~5.2 ms |
| 5   | 52.0       |    24.0 Mbps   |    2143     | 11  | 14 | ~5.1 ms |
| 7   | 65.0       |    34.1 Mbps   |    3047     | 15  | 20 | ~4.9 ms |

The **tradeoff** for flat recovery latency is burst-loss tolerance *per block*: at MCS 0, K=2/N=3 only recovers one lost packet per block, vs. one-block-per-frame's K=7/N=9 recovering two. In exchange, blocks close ~3× more often, so a burst that used to hit a single big block now spans multiple small blocks, each with its own parity. For the typical far-range case of isolated single-packet loss, the latency-targeted formula is strictly better; for dense correlated bursts, consider raising `fec_redundancy_ratio` from the default 0.2 to ~0.33 (see Parameter tuning below).

## MTU assumption and wfb-ng mlink mode

`_compute_fec_from_bitrate()` reads `mtu_payload_bytes` from the `[dynamic]` section of `alink_gs.conf`. The default is **1446 bytes**, which matches a standard 1500-byte Ethernet MTU minus IP/UDP headers — the normal OpenIPC setup where Majestic emits ~1400-byte UDP packets to `wfb_tx`.

**If you run Majestic with wfb-ng mlink and a larger UDP payload**, set `mtu_payload_bytes` to match the **actual packet size the encoder emits**. A typical mlink value is 1400-3893 bytes — but note that Majestic's `outgoingSize` is what matters here, not wfb-ng's `MAX_FEC_PAYLOAD` ceiling. If the encoder is capped at 1400-byte packets even on an mlink setup, `mtu_payload_bytes` must be 1400 for K sizing to be correct. alink clamps the config at 3900 with a startup warning.

### Why it matters

`packets_per_sec = bitrate / (8 × MTU)` drives K directly. A too-large MTU in the config makes the GS size K for ~2-3× fewer packets/sec than actually hit the wire, producing a K that is too small and a parity rate (N-K)/N that rounds up to 33%+ instead of the configured 20-25% — quietly eating airtime that the encoder needed.

## The adjustment chain

After the baseline K and N are computed, three adjustments are applied in order:

### 1. Loss-triggered parity increase

```python
if loss_rate > loss_threshold_for_fec_downgrade:
    fec_n = fec_n + 1
```

When packet loss exceeds the threshold, N is incremented (one more parity packet per block) while K stays constant. This adds one recoverable loss per block while keeping `t_block` at the operator's `block_latency_budget_ms`. An earlier version decremented K instead, which also added one recoverable loss per block but simultaneously shortened `t_block` and cut the encoder bitrate harder (K/N drops ~25% vs ~14% for N+=1). Bumping N is the gentler choice — the loss reaction only touches the redundancy knob, leaving the latency and bitrate knobs alone.

### 2. Maximum redundancy enforcement

```python
actual_redundancy = (fec_n - fec_k) / fec_n
if actual_redundancy > max_fec_redundancy:
    fec_k = ceil(fec_n * (1 - max_fec_redundancy))
```

Prevents the parity ratio from exceeding `max_fec_redundancy` (default 0.5). This is a safety rail — at 50% redundancy, half of all transmitted packets are parity, which is wasteful. If the loss-triggered increase pushed redundancy too high, K is raised back.

### 3. max_fec_n cap

```python
if fec_n > max_fec_n:
    original_redundancy_ratio = 1 - (fec_k / fec_n)
    fec_n = max_fec_n
    fec_k = ceil(fec_n * (1 - original_redundancy_ratio))
```

Limits the total block size. When N exceeds the cap, both N and K are shrunk while preserving the redundancy ratio. With the latency-targeted formula K is already much smaller than in the old frame-proportional path, so this cap rarely triggers at default values. It is still useful as a belt-and-braces guard at extreme bitrates or absurd `block_latency_budget_ms` values.

## Parity burst problem at high MCS

There is still an upper bound on useful block size for a *different* reason: wfb-ng generates parity synchronously (`tx.cpp:686-709`). After K data packets accumulate, `send_packet()` blocks while it:

1. Computes Reed-Solomon parity — CPU-bound, cost proportional to K × (N-K)
2. Injects all N-K parity packets back-to-back
3. Does not return until all parity packets are sent

During this stall, the video encoder continues producing UDP packets that queue in the kernel socket buffer. At high MCS (drone close, high bitrate), the packet rate is highest and the timing is tightest. Under the old frame-proportional K (K up to 35+ at MCS 7), this could create visible smearing. With the latency-targeted K (K around 15 at the same MCS), the encoding burst is far smaller (11×4=44 operations vs 32×8=256) and the issue is much milder. Keeping `max_fec_n` around 50 is a safe default.

## Parameter tuning guide

### block_latency_budget_ms (default: 5)

The cap on post-loss FEC recovery latency. Smaller = faster recovery when a packet is lost; larger = more parity packets grouped into one block (better burst tolerance at the cost of recovery latency).

| Value | Behavior                                                            |
|-------|---------------------------------------------------------------------|
| 3 ms  | Aggressive — very fast recovery, minimal burst tolerance per block  |
| 5 ms  | Balanced (default) — ~half a frame period at 90 fps                 |
| 10 ms | Burst-tolerant — approaches one frame period at 60 fps              |

Do not set above the video frame period (16.7 ms at 60 fps, 11.1 ms at 90 fps). Above that the frame cap kicks in and further raises are no-ops.

### fec_redundancy_ratio (default: 0.2)

Fraction of link capacity reserved for FEC parity. Higher = more resilient but lower video bitrate.

| Value | Parity overhead | Use case |
|-------|----------------|----------|
| 0.15  | ~18%           | Clean links, maximum bitrate |
| 0.20  | 20%            | Good links with occasional interference (default) |
| 0.25  | 25%            | General purpose |
| 0.33  | 33%            | Noisy environments, long range, or compensation for smaller K at low bitrate |

### max_fec_n (default: 50)

Maximum block size. With the latency-targeted K formula this cap only binds at extreme configurations. Keep it at the default unless you have a specific reason to clamp.

### max_fec_redundancy (default: 0.5)

Safety cap on the parity ratio (N-K)/N. The default of 0.5 (50%) is appropriate for most setups. Only relevant when loss-triggered N bumps push redundancy very high.

### loss_threshold_for_fec_downgrade (default: 0.10)

Loss rate threshold that triggers the `N += 1` adjustment (more parity per block). Lower values make the system more reactive to packet loss.

### emergency_fec_pressure (default: 0.75)

FEC pressure threshold for the Channel B emergency downgrade. When FEC is consuming >= 75% of its recovery budget, forces an immediate one-step MCS downgrade regardless of SNR margin or rate limiting.
