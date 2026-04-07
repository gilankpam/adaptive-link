## ADDED Requirements

### Requirement: Parameter space definition with constraints
The `ParameterSpace` class SHALL define all 37 tunable parameters with their types (float, int, categorical) and valid ranges. Scoring weight groups MUST be constrained to sum to 1.0 by deriving the last weight from the others.

#### Scenario: Scoring weights sum to 1.0
- **WHEN** a trial generates scoring weight values
- **THEN** rf_weight + loss_weight + fec_weight + diversity_weight SHALL equal 1.0

#### Scenario: RF normalization weights sum to 1.0
- **WHEN** a trial generates RF normalization weight values
- **THEN** snr_weight + rssi_weight SHALL equal 1.0

#### Scenario: Per-adapter MCS constraint
- **WHEN** optimizing for an adapter with max MCS capability from `wlan_adapters.yaml`
- **THEN** the `max_mcs` parameter SHALL be bounded by the adapter's capability

#### Scenario: Generated config is valid
- **WHEN** a trial generates parameter values
- **THEN** the resulting ConfigParser object SHALL create a valid `ProfileSelector` instance

### Requirement: Bayesian optimization per adapter
The `AdapterOptimizer` SHALL run optuna's TPE sampler to minimize negative fitness over the parameter space, separately for each adapter type. The objective function SHALL run `ReplaySimulator` on telemetry data filtered to the target adapter.

#### Scenario: Per-adapter optimization
- **WHEN** optimizing for adapter "bl-r8812af1"
- **THEN** only telemetry ticks with `adapter == "bl-r8812af1"` SHALL be used

#### Scenario: Multi-file aggregation
- **WHEN** multiple telemetry files exist for an adapter
- **THEN** fitness SHALL be averaged across all files to prevent overfitting to one flight

#### Scenario: Reproducible results
- **WHEN** optimization runs with the same seed
- **THEN** the trial sequence and best parameters SHALL be identical

### Requirement: Optimized config file output
The optimizer SHALL write optimized parameters as standard INI config files at `config/alink_gs.{adapter_id}.conf`, directly loadable by the existing ground station `load_configuration()` function.

#### Scenario: Valid INI output
- **WHEN** optimization completes
- **THEN** the output file SHALL be parseable by `configparser.ConfigParser`

#### Scenario: All sections present
- **WHEN** the output config is read
- **THEN** it SHALL contain all sections from `DEFAULT_CONFIG` (scoring, weights, ranges, profile selection, dynamic, noise, error estimation)

#### Scenario: Metadata header
- **WHEN** the output config is written
- **THEN** it SHALL include header comments with optimization date, adapter ID, trial count, and fitness comparison (default vs. optimized)

### Requirement: CLI interface
The optimizer SHALL provide a command-line interface with arguments for adapter selection, telemetry directory, output directory, trial count, and random seed.

#### Scenario: Single adapter optimization
- **WHEN** `--adapter bl-r8812af1` is specified
- **THEN** optimization SHALL run only for that adapter

#### Scenario: Comparison report
- **WHEN** optimization completes
- **THEN** the CLI SHALL print a comparison of default vs. optimized fitness with component breakdown
