## ADDED Requirements

### Requirement: Generate score distribution analysis per adapter
The analysis tool SHALL produce histograms of the combined score distribution, broken down by adapter ID, to identify per-hardware scoring patterns.

#### Scenario: Multiple adapters in telemetry data
- **WHEN** telemetry contains data from adapters "bl-r8812af1" and "bl-m8812eu2"
- **THEN** the output contains separate score distribution histograms for each adapter, saved as PNG files

#### Scenario: Single adapter
- **WHEN** telemetry contains data from only one adapter
- **THEN** the output contains a single score distribution histogram

### Requirement: Generate MCS transition diagram
The analysis tool SHALL produce a visualization of MCS level transitions over time and as a transition matrix showing the frequency of each MCS-to-MCS change.

#### Scenario: Telemetry with profile changes
- **WHEN** telemetry contains multiple profile changes across different MCS levels
- **THEN** the output includes a transition matrix heatmap showing counts of each (from_mcs, to_mcs) pair and a time-series plot of MCS level

### Requirement: Generate feature-outcome correlation matrix
The analysis tool SHALL compute and visualize correlations between all features (raw + derived) and outcome labels, identifying which features best predict good/bad outcomes.

#### Scenario: Telemetry with outcome labels
- **WHEN** telemetry contains tick records joined with outcome labels
- **THEN** the output includes a correlation heatmap showing Pearson correlation between each numeric feature and a binary outcome variable (1=good, 0=bad)

#### Scenario: No outcome labels available
- **WHEN** telemetry contains no outcome records
- **THEN** the tool SHALL skip outcome correlation analysis and log a warning

### Requirement: Identify failure modes
The analysis tool SHALL identify and summarize failure patterns — sequences where the algorithm made "bad" outcome decisions — grouped by the conditions that preceded the failure.

#### Scenario: Bad outcomes present
- **WHEN** telemetry contains outcome records labeled "bad"
- **THEN** the output includes a summary table showing: count of bad outcomes, mean feature values in the 5 ticks preceding each bad outcome, and the most common MCS level at failure time

### Requirement: Generate summary statistics report
The analysis tool SHALL produce a text/markdown summary report with key statistics: total ticks, total profile changes, outcome distribution (good/marginal/bad counts and percentages), mean and std of each feature, and per-adapter breakdowns.

#### Scenario: Valid telemetry data
- **WHEN** the analysis tool processes a telemetry directory
- **THEN** a summary report file is written containing all listed statistics

### Requirement: Command-line interface
The analysis tool SHALL be invocable from the command line with arguments for input directory (telemetry JSONL location) and output directory (where plots and reports are saved).

#### Scenario: Run analysis from command line
- **WHEN** the user runs `python3 ground-station/ml/analyze_telemetry.py --input /var/log/alink --output ./analysis-output`
- **THEN** the tool loads telemetry from the input directory, runs all analyses, and saves output files to the specified output directory

#### Scenario: Missing input directory
- **WHEN** the specified input directory does not exist or contains no telemetry files
- **THEN** the tool exits with a clear error message
