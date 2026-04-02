## Why

Phase 0 (telemetry logging) is complete — the ground station now captures per-tick JSONL records with all link metrics, sub-scores, profile decisions, and outcome labels. Before building any ML models (Phases 2-4), we need to understand the feature space: which metrics actually predict good/bad outcomes, where the current heuristic scoring fails, and what derived features (rate-of-change, volatility, saturation proximity) carry signal. This offline analysis runs on dev machines only and produces the feature pipeline that all subsequent ML phases depend on.

## What Changes

- Add a **feature engineering module** that computes derived features from raw telemetry: SNR rate of change, loss rate acceleration, FEC saturation proximity, score volatility, link budget margin, and time since last profile change
- Add an **analysis/visualization script** that loads telemetry JSONL and produces: score distributions per adapter, MCS transition diagrams, feature-vs-outcome correlation matrices, and failure mode breakdowns
- These are **dev-only tools** (not deployed to drone or ground station hardware) — they use numpy/pandas/matplotlib

## Capabilities

### New Capabilities
- `feature-engineering`: Compute derived features from consecutive telemetry records (SNR derivatives, loss acceleration, FEC saturation, score volatility, link budget margin, time-since-change)
- `telemetry-analysis`: Load and visualize telemetry data — score distributions, MCS transitions, feature-outcome correlations, failure mode identification

### Modified Capabilities

None — this phase adds dev-only tooling and does not modify any deployed components.

## Impact

- **New files**: `ground-station/ml/feature_engineering.py`, `ground-station/ml/analyze_telemetry.py`
- **Dependencies**: numpy, pandas, matplotlib (dev-only, not required on target hardware)
- **No production code changes**: `alink_gs`, config files, and drone daemon are unaffected
- **Data dependency**: Consumes JSONL files produced by the Phase 0 TelemetryLogger
