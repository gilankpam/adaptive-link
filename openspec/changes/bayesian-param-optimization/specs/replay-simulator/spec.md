## ADDED Requirements

### Requirement: SNR-based link model estimates packet loss
The `LinkModel` class SHALL estimate packet loss rate from SNR and chosen MCS level using a sigmoid function centered on `MCS_SNR_THRESHOLDS`. Loss MUST be near-zero when SNR is well above the MCS threshold and MUST increase sharply below the threshold.

#### Scenario: Zero loss above threshold
- **WHEN** SNR is 10 dB above the MCS threshold
- **THEN** estimated loss rate SHALL be less than 0.01

#### Scenario: High loss below threshold
- **WHEN** SNR is 5 dB below the MCS threshold
- **THEN** estimated loss rate SHALL be greater than 0.5

#### Scenario: Loss increases monotonically with decreasing SNR
- **WHEN** two SNR values are compared for the same MCS level
- **THEN** the lower SNR SHALL produce equal or higher estimated loss

#### Scenario: Higher MCS requires higher SNR
- **WHEN** two MCS levels are compared at the same SNR
- **THEN** the higher MCS level SHALL produce equal or higher estimated loss

### Requirement: FEC recovery estimation
The `LinkModel` SHALL estimate the number of FEC-recovered packets given a loss rate, FEC parameters (k, n), and packet count. Recovery count MUST NOT exceed the FEC redundancy capacity `(n - k)` per FEC block.

#### Scenario: FEC recovers within capacity
- **WHEN** loss rate is low enough that lost packets per block are within `(n - k)`
- **THEN** estimated recoveries SHALL equal estimated lost packets

#### Scenario: FEC saturated
- **WHEN** lost packets per block exceed `(n - k)`
- **THEN** estimated recoveries SHALL be capped at `(n - k)` per block

### Requirement: Deterministic replay of telemetry
The `ReplaySimulator` SHALL process telemetry tick records in order, feeding RF metrics through a `ProfileSelector` instance with candidate parameters, and MUST produce identical results for identical inputs and parameters.

#### Scenario: Deterministic output
- **WHEN** the same telemetry data and parameters are replayed twice
- **THEN** both runs SHALL produce identical `ReplayResult` values

#### Scenario: RF metrics used as ground truth
- **WHEN** replaying telemetry
- **THEN** SNR, RSSI, RSSI_min, and antenna count from each tick SHALL be fed directly to `ProfileSelector` without modification

### Requirement: Time injection for hold timers
The replay simulator SHALL override `ProfileSelector._now_ms()` to return tick timestamps, ensuring hold timers and rate limiting behave correctly based on telemetry timing rather than wall clock.

#### Scenario: Hold timer respects tick timestamps
- **WHEN** ticks are spaced 100ms apart and `min_between_changes_ms` is 200ms
- **THEN** profile changes SHALL be rate-limited to every 2nd tick at most

#### Scenario: No wall clock dependency
- **WHEN** replay runs faster or slower than real time
- **THEN** results SHALL be identical regardless of replay speed

### Requirement: Counterfactual packet metrics
The simulator SHALL use `LinkModel` to generate synthetic packet loss and FEC statistics based on the SNR and the MCS that `ProfileSelector` would choose with the candidate parameters. Synthetic cumulative counters MUST be maintained for correct delta computation in `compute_score()`.

#### Scenario: Synthetic counters drive scoring
- **WHEN** the simulator generates a tick's packet metrics
- **THEN** `compute_score()` SHALL receive cumulative counters that produce correct deltas

### Requirement: Multi-objective fitness aggregation
`ReplayResult` SHALL aggregate per-tick rewards into a scalar fitness score combining throughput, reliability, stability, and crash penalty components.

#### Scenario: Crash penalty dominates
- **WHEN** a parameter set produces 10% crash events (loss > 15%)
- **THEN** its fitness SHALL be lower than a conservative parameter set with zero crashes but lower throughput

#### Scenario: Higher throughput improves fitness
- **WHEN** two parameter sets have equal reliability and stability
- **THEN** the one with higher mean bitrate SHALL have higher fitness

#### Scenario: Fewer transitions improve fitness
- **WHEN** two parameter sets have equal throughput and reliability
- **THEN** the one with fewer profile transitions SHALL have higher fitness
