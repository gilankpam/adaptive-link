## ADDED Requirements

### Requirement: Load and parse telemetry JSONL files
The module SHALL provide a function to load telemetry JSONL files from a directory, separating tick records from outcome records into distinct DataFrames. Outcome records SHALL be identified by the presence of `type: "outcome"`. The loader SHALL validate that required columns exist and raise a clear error if fields are missing.

#### Scenario: Load valid telemetry directory
- **WHEN** a directory containing one or more `telemetry_*.jsonl` files is provided
- **THEN** the function returns a tuple of (ticks_df, outcomes_df) where ticks_df contains all non-outcome records and outcomes_df contains all outcome records

#### Scenario: Join outcomes to triggering ticks
- **WHEN** the outcomes DataFrame is joined back to ticks
- **THEN** each outcome's `change_ts` matches to the tick record with the corresponding `ts` value, adding `outcome_label`, `outcome_avg_loss`, and `outcome_max_loss` columns

#### Scenario: Missing required fields
- **WHEN** a JSONL file is missing required fields (e.g., `ts`, `snr`, `rssi`)
- **THEN** the loader raises a ValueError with a message listing the missing fields

### Requirement: Compute SNR rate of change
The module SHALL compute the rate of change of SNR EMA between consecutive ticks (dSNR/dt), representing how quickly signal quality is changing.

#### Scenario: Consecutive ticks with changing SNR
- **WHEN** two consecutive ticks have `snr_ema` values of 25.0 and 23.0
- **THEN** the SNR rate of change for the second tick is -2.0

#### Scenario: First tick in sequence
- **WHEN** computing SNR rate of change for the first tick
- **THEN** the value SHALL be 0.0 (no prior tick to diff against)

### Requirement: Compute loss rate acceleration
The module SHALL compute the second derivative of loss rate — the rate at which loss rate itself is changing — to detect rapidly deteriorating conditions.

#### Scenario: Accelerating loss
- **WHEN** three consecutive ticks have loss_rate values of 0.01, 0.03, 0.07
- **THEN** the loss rate derivatives are [0.02, 0.04] and the acceleration for the third tick is 0.02 (positive = worsening faster)

#### Scenario: Insufficient history
- **WHEN** fewer than 3 ticks are available
- **THEN** loss acceleration SHALL be 0.0 for ticks without enough history

### Requirement: Compute FEC saturation proximity
The module SHALL compute how close FEC recovery is to exhausting available redundancy, as a ratio from 0.0 (no pressure) to 1.0 (fully saturated).

#### Scenario: Moderate FEC usage
- **WHEN** a tick has `fec_pressure` of 0.6
- **THEN** the FEC saturation proximity is 0.6

#### Scenario: No FEC recovery
- **WHEN** a tick has `fec_pressure` of 0.0
- **THEN** the FEC saturation proximity is 0.0

### Requirement: Compute score volatility
The module SHALL compute the standard deviation of the combined score over a trailing window (default 20 ticks) to measure link stability.

#### Scenario: Stable link
- **WHEN** the last 20 ticks all have scores within 1500-1510
- **THEN** score volatility is low (stddev < 5)

#### Scenario: Unstable link
- **WHEN** the last 20 ticks have scores oscillating between 1200 and 1800
- **THEN** score volatility is high (stddev > 100)

#### Scenario: Fewer ticks than window size
- **WHEN** fewer than 20 ticks are available (e.g., 5 ticks)
- **THEN** volatility SHALL be computed over all available ticks

### Requirement: Compute link budget margin
The module SHALL compute the difference between current SNR EMA and the MCS SNR threshold for the currently selected MCS level, indicating how much SNR headroom exists.

#### Scenario: Comfortable margin
- **WHEN** `snr_ema` is 30.0 and current MCS is 4 (threshold 20dB)
- **THEN** link budget margin is 10.0 dB

#### Scenario: Marginal link
- **WHEN** `snr_ema` is 21.0 and current MCS is 4 (threshold 20dB)
- **THEN** link budget margin is 1.0 dB

#### Scenario: Below threshold
- **WHEN** `snr_ema` is 18.0 and current MCS is 4 (threshold 20dB)
- **THEN** link budget margin is -2.0 dB (negative = operating below safe threshold)

### Requirement: Compute time since last profile change
The module SHALL compute the elapsed time in milliseconds since the most recent profile change for each tick.

#### Scenario: Recent profile change
- **WHEN** a profile change occurred at ts=1000 and current tick is ts=1500
- **THEN** time since last change is 500 ms

#### Scenario: No prior profile change
- **WHEN** no profile change has occurred in the telemetry data before the current tick
- **THEN** time since last change SHALL be NaN or a sentinel value indicating unknown

### Requirement: Compute all features in batch
The module SHALL provide a single function that takes a ticks DataFrame and returns an augmented DataFrame with all derived feature columns added.

#### Scenario: Full feature computation
- **WHEN** a ticks DataFrame with valid telemetry data is provided
- **THEN** the returned DataFrame contains all original columns plus: `snr_roc`, `loss_accel`, `fec_saturation`, `score_volatility`, `link_budget_margin`, `time_since_change`
