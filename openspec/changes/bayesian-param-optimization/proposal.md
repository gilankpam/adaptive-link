## Why

The ground station's `ProfileSelector` has 37 hand-tuned parameters (scoring weights, EMA alphas, hysteresis thresholds, hold timers, dynamic mode margins) that require per-hardware adjustment because different WiFi adapters have different SNR/power characteristics. Manual tuning is time-consuming, subjective, and unlikely to find the true optimum in a 37-dimensional space. With Phase 0 telemetry logging now collecting real flight data and Phase 1 feature engineering providing analysis tools, we have the foundation to systematically optimize these parameters using Bayesian optimization.

## What Changes

- Add a **deterministic replay simulator** that replays telemetry data through `ProfileSelector` with candidate parameter sets, using a counterfactual link model to estimate outcomes
- Add a **Bayesian parameter optimizer** that searches the 37-parameter space per adapter type using optuna's TPE sampler, outputting optimized config files
- Add `optuna` and `scipy` to dev dependencies
- Output optimized `config/alink_gs.{adapter_id}.conf` files that are directly loadable by the existing ground station without code changes

## Capabilities

### New Capabilities
- `replay-simulator`: Deterministic replay engine with SNR-based link model for counterfactual evaluation of parameter sets against historical telemetry data
- `parameter-optimizer`: Bayesian optimization over the 37-parameter space with per-adapter constraints, multi-objective fitness scoring, and optimized config file output

### Modified Capabilities

## Impact

- **New files**: `ground-station/ml/replay_simulator.py`, `ground-station/ml/optimize_params.py`, test files
- **Dependencies**: `optuna`, `scipy` added to `requirements-dev.txt` (dev-only, not deployed)
- **No production code changes**: Only new dev-only tooling; ground station code is unchanged
- **Config output**: Generates per-adapter config files in existing INI format
