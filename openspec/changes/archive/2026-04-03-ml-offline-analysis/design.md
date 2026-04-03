## Context

Phase 0 telemetry logging is complete — `TelemetryLogger` in `ground-station/alink_gs` writes append-only JSONL at ~10Hz with raw metrics (RSSI, SNR, packets, FEC), computed sub-scores, profile parameters, and outcome labels (good/bad/marginal). The JSONL uses short keys (`ts`, `rssi`, `snr`, `rssi_min`, `ant`, `pkt_all`, `pkt_lost`, `pkt_fec`, `fec_k`, `fec_n`, `loss_rate`, `fec_pressure`, `rf_score`, `loss_score`, `fec_score`, `div_score`, `score`, `ema_fast`, `ema_slow`, `snr_ema`, `mcs`, `gi`, `sel_fec_k`, `sel_fec_n`, `bitrate`, `power`, `changed`, `adapter`) plus outcome records (`type: "outcome"`, `change_ts`, `avg_loss`, `max_loss`, `label`, `ticks`).

Phase 1 needs to process this data offline to understand the feature space and identify where the current heuristic algorithm fails, producing the derived feature pipeline used by all subsequent ML phases.

## Goals / Non-Goals

**Goals:**
- Compute derived features from consecutive telemetry records that capture temporal dynamics (rates of change, volatility, saturation proximity)
- Provide analysis tools to visualize score distributions, MCS transitions, and feature-outcome correlations across adapters
- Produce reusable feature computation code that Phase 2+ can import directly
- Identify failure modes in the current heuristic scoring algorithm

**Non-Goals:**
- Training ML models (Phase 2+)
- Modifying deployed ground station or drone code
- Real-time or embedded feature computation (dev-machine only, numpy/pandas OK)
- Building a full data pipeline or database — files are read directly from JSONL

## Decisions

### 1. Feature engineering as a standalone module with pure functions

Feature computation lives in `ground-station/ml/feature_engineering.py` as stateless functions operating on pandas DataFrames. Each derived feature is a separate function that takes a DataFrame and returns a Series, making features independently testable and composable.

**Alternative considered:** Class-based feature pipeline with streaming state. Rejected because offline analysis doesn't need streaming, and stateless functions are simpler to test and reuse in Phase 2's replay simulator.

### 2. JSONL loading with record type separation

The telemetry JSONL interleaves tick records and outcome records (distinguished by `type: "outcome"`). The loader separates these into two DataFrames — ticks and outcomes — then joins outcomes back to their triggering tick via `change_ts` matching to `ts`.

**Alternative considered:** Single DataFrame with outcome columns sparse-filled. Rejected because it complicates analysis — most ticks have no outcome, and outcome records have different schemas.

### 3. Analysis script produces static PNG/HTML output

`analyze_telemetry.py` generates a set of diagnostic plots saved to an output directory rather than requiring an interactive notebook or dashboard. This keeps the tool simple and CI-friendly.

**Alternative considered:** Jupyter notebook. Viable for exploration but harder to version and automate. The analysis script can still be called from a notebook if needed.

### 4. MCS SNR thresholds reused from alink_gs

Link budget margin computation needs the `MCS_SNR_THRESHOLDS` table. Rather than duplicating it, `feature_engineering.py` defines it as a module constant matching the values in `alink_gs`. This is acceptable since the thresholds are 802.11n physical constants that don't change.

## Risks / Trade-offs

- **[Insufficient telemetry data]** → Analysis quality depends on collecting diverse flight data across adapters and conditions. Mitigation: document minimum data requirements, provide synthetic data generation for testing.
- **[Feature relevance uncertainty]** → Derived features are hypothesized to be useful but unvalidated until Phase 2. Mitigation: compute all candidate features and let correlation analysis identify which carry signal.
- **[JSONL schema drift]** → If Phase 0 logger fields change, the loader breaks. Mitigation: loader validates expected columns and raises clear errors on missing fields.
