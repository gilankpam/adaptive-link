## 1. Project Setup

- [x] 1.1 Add `optuna` and `scipy` to `requirements-dev.txt`

## 2. Link Model

- [x] 2.1 Implement `LinkModel` class in `ground-station/ml/replay_simulator.py` with SNR-to-loss sigmoid estimation per MCS level
- [x] 2.2 Implement `estimate_fec_recoveries()` method for FEC recovery count estimation

## 3. Replay Simulator Core

- [x] 3.1 Implement `ReplayResult` dataclass with fitness, throughput, loss, transition count, crash events, MCS distribution, stability score
- [x] 3.2 Implement `ReplaySimulator.__init__()` — accept ticks_df and config, create ProfileSelector, set up time injection
- [x] 3.3 Implement `ReplaySimulator.run()` — iterate ticks, extract RF metrics, use LinkModel for counterfactual packet metrics, feed through compute_score/select, accumulate rewards
- [x] 3.4 Implement fitness aggregation — combine throughput, reliability, stability, crash penalty with configurable weights

## 4. Replay Simulator Tests

- [x] 4.1 Write LinkModel tests — monotonicity, cliff-edge, zero-loss above threshold, FEC recovery bounds
- [x] 4.2 Write determinism tests — same inputs + same params = identical ReplayResult
- [x] 4.3 Write time injection tests — hold timers use tick timestamps
- [x] 4.4 Write fitness function tests — crash penalty dominance, throughput reward, stability bonus
- [x] 4.5 Write edge case tests — single tick, stable conditions, poor conditions

## 5. Parameter Space and Optimizer

- [x] 5.1 Implement `ParameterSpace` class with all 37 parameters, types, ranges, and sum-to-1 constraints
- [x] 5.2 Implement `AdapterOptimizer` with objective function running ReplaySimulator
- [x] 5.3 Implement multi-file telemetry aggregation and adapter filtering
- [x] 5.4 Implement config output writer — valid INI file with metadata header
- [x] 5.5 Implement CLI with --adapter, --telemetry-dir, --output-dir, --n-trials, --seed arguments

## 6. Optimizer Tests

- [x] 6.1 Write parameter space tests — weight constraints, config roundtrip, adapter bounds
- [x] 6.2 Write config output tests — valid INI, all sections present, parseable
- [x] 6.3 Write single-trial integration test — objective function completes without error

## 7. Verification

- [x] 7.1 Run full test suite with pytest
- [x] 7.2 Verify optimized config loads correctly with load_configuration()
