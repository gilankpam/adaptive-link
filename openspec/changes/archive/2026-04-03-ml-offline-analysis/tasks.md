## 1. Project Setup

- [x] 1.1 Create `ground-station/ml/` directory with `__init__.py`
- [x] 1.2 Add `requirements-dev.txt` with numpy, pandas, matplotlib dependencies

## 2. Telemetry Loader

- [x] 2.1 Implement `load_telemetry(directory)` in `feature_engineering.py` — load all `telemetry_*.jsonl` files, separate tick records from outcome records, validate required fields
- [x] 2.2 Implement `join_outcomes(ticks_df, outcomes_df)` — join outcome labels back to triggering ticks via `change_ts` matching

## 3. Derived Feature Functions

- [x] 3.1 Implement `compute_snr_roc(df)` — SNR rate of change from consecutive `snr_ema` values
- [x] 3.2 Implement `compute_loss_accel(df)` — second derivative of `loss_rate`
- [x] 3.3 Implement `compute_fec_saturation(df)` — FEC saturation proximity from `fec_pressure`
- [x] 3.4 Implement `compute_score_volatility(df, window=20)` — rolling stddev of `score`
- [x] 3.5 Implement `compute_link_budget_margin(df)` — `snr_ema` minus `MCS_SNR_THRESHOLDS[mcs]`
- [x] 3.6 Implement `compute_time_since_change(df)` — elapsed ms since last `changed=True` tick
- [x] 3.7 Implement `compute_all_features(df)` — batch function that adds all derived columns

## 4. Feature Engineering Tests

- [x] 4.1 Write tests for `load_telemetry` — valid data, missing fields, outcome separation
- [x] 4.2 Write tests for each derived feature function — edge cases (first tick, insufficient history, boundary values)
- [x] 4.3 Write tests for `compute_all_features` — verify all expected columns present

## 5. Analysis Script

- [x] 5.1 Implement CLI argument parsing in `analyze_telemetry.py` (`--input`, `--output`)
- [x] 5.2 Implement score distribution histograms per adapter
- [x] 5.3 Implement MCS transition matrix heatmap and time-series plot
- [x] 5.4 Implement feature-outcome correlation matrix heatmap
- [x] 5.5 Implement failure mode summary (pre-failure feature statistics)
- [x] 5.6 Implement summary statistics report (markdown output)

## 6. Verification

- [x] 6.1 Run all feature engineering tests with `pytest -v`
- [x] 6.2 Test analysis script end-to-end with sample telemetry data
