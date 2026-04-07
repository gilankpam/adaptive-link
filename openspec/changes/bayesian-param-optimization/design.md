## Context

The ground station `alink_gs` has a `ProfileSelector` class (line 369) with 37 parameters across 8 config sections. Phase 0 telemetry logging collects JSONL records at ~10Hz during flights. Phase 1 feature engineering provides `load_telemetry()` for data loading. This phase adds dev-only tooling to optimize those 37 parameters per adapter.

## Goals / Non-Goals

**Goals:**
- Find per-adapter optimal parameter values using Bayesian optimization
- Deterministic replay of telemetry data for parameter evaluation
- Output standard INI config files loadable by existing `load_configuration()`
- Multi-objective fitness: throughput, reliability, stability, safety

**Non-Goals:**
- Changing any production algorithm logic (Phase 3+)
- Real-time or online optimization (Phase 5)
- Deploying ML models to target hardware
- Optimizing the profile table (`txprofiles.conf`) — only dynamic mode parameters

## Decisions

### 1. Counterfactual replay via SNR-based link model

**Decision:** Use recorded SNR/RSSI as ground truth (they're independent of our TX choices) and estimate counterfactual packet loss using a sigmoid function centered on `MCS_SNR_THRESHOLDS`.

**Alternatives considered:**
- Direct replay with original packet metrics — invalid because different parameters would produce different decisions, and packet metrics depend on those decisions
- Full wireless channel simulator — too complex, requires channel models we don't have
- Outcome-only evaluation (only score on ticks with outcome labels) — too sparse, labels exist only after profile changes

**Rationale:** SNR/RSSI at the ground station depend on the drone's TX path and environment, not on our parameter choices. The sigmoid PER model captures the essential 802.11n behavior: near-zero loss above threshold, sharp cliff below. This is conservative and physically grounded.

### 2. Optuna TPE sampler over scipy.optimize

**Decision:** Use optuna with Tree-structured Parzen Estimator.

**Alternatives considered:**
- `scipy.optimize.minimize` — requires continuous parameters, poor with mixed int/float/categorical
- Random search — too sample-inefficient in 35 dimensions
- Custom Gaussian Process — engineering overhead, optuna does this well

**Rationale:** 35 effective dimensions with mixed types (float, int, boolean, categorical). TPE handles this natively, supports pruning of unpromising trials, and has built-in visualization.

### 3. Time injection via _now_ms override

**Decision:** Override `ProfileSelector._now_ms()` on the instance to return tick timestamps.

**Rationale:** `select()` uses `_now_ms()` for rate limiting and hold timers. Without time injection, all ticks would appear to arrive simultaneously (wall clock barely advances during replay), making hold timers meaningless. The method is a simple instance method at line 464, easily overridden without subclassing.

### 4. Fitness function with crash penalty dominance

**Decision:** `fitness = 0.3 * throughput + 0.3 * reliability + 0.1 * stability - 5.0 * crash_rate`

**Rationale:** A parameter set causing even 10% crash events (loss > 15%) must score worse than conservative MCS 3 with zero crashes. The 5.0 weight on crash penalty ensures safety always dominates throughput optimization. This matches the project's design philosophy: "safety >> stability > throughput".

### 5. Weight constraints via derivation

**Decision:** Optimize N-1 weights and derive the Nth to enforce sum-to-1 constraints.

**Rationale:** The 4 scoring weights (rf, loss, fec, diversity) must sum to 1.0. Rather than adding a constraint to the optimizer (which TPE doesn't natively support), optimize 3 weights and compute `diversity_weight = 1.0 - rf_weight - loss_weight - fec_weight`, clamping to [0, 1]. Same approach for snr_weight/rssi_weight.

## Risks / Trade-offs

**[Link model fidelity]** → The sigmoid PER model is an approximation. Real wireless channels have fading, interference, and antenna-specific behavior not captured by a smooth curve. **Mitigation:** Use conservative steepness parameter; validate model against actual outcome labels from telemetry; model is easily swappable if real PER data becomes available.

**[Overfitting to specific flights]** → Optimizing on limited telemetry data may produce parameters that work well for recorded conditions but poorly for novel situations. **Mitigation:** Aggregate fitness across all available telemetry files for an adapter; use median pruner to avoid outlier sensitivity; compare optimized vs. default fitness to ensure improvement is meaningful.

**[Cumulative counter simulation]** → `compute_score()` uses packet count deltas from cumulative counters. The simulator must maintain synthetic cumulative counters that grow correctly. **Mitigation:** Tested explicitly; straightforward bookkeeping.

**[Dynamic mode only]** → Optimization targets `dynamic_mode=True`. Table-mode profile lookup parameters are still tuned but the MCS/bitrate/FEC selection uses dynamic computation. **Mitigation:** This aligns with the project direction; table mode parameters (hold timers, hysteresis) still get optimized as they affect profile change timing.
