#!/usr/bin/env python3
"""Offline telemetry analysis and visualization for adaptive link data.

Dev-only tool — loads Phase 0 JSONL telemetry, computes derived features,
and produces diagnostic plots and summary reports.

Usage:
    python3 analyze_telemetry.py --input /var/log/alink --output ./analysis-output
"""

import argparse
import os
import sys

import matplotlib
matplotlib.use('Agg')  # Non-interactive backend for headless use
import matplotlib.pyplot as plt
import numpy as np
import pandas as pd

sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..'))
from ml.feature_engineering import (
    compute_all_features,
    join_outcomes,
    load_telemetry,
)


def plot_score_distributions(ticks_df, output_dir):
    """Generate score distribution histograms per adapter."""
    adapters = ticks_df['adapter'].unique()
    fig, axes = plt.subplots(1, len(adapters), figsize=(6 * len(adapters), 4),
                             squeeze=False)
    for i, adapter in enumerate(adapters):
        ax = axes[0, i]
        data = ticks_df[ticks_df['adapter'] == adapter]['score']
        ax.hist(data, bins=50, edgecolor='black', alpha=0.7)
        ax.set_title(f'Score Distribution: {adapter}')
        ax.set_xlabel('Score')
        ax.set_ylabel('Count')
        ax.axvline(data.mean(), color='red', linestyle='--', label=f'Mean: {data.mean():.0f}')
        ax.legend()

    plt.tight_layout()
    path = os.path.join(output_dir, 'score_distributions.png')
    fig.savefig(path, dpi=150)
    plt.close(fig)
    print(f"  Saved {path}")


def plot_mcs_transitions(ticks_df, output_dir):
    """Generate MCS transition matrix heatmap and time-series plot."""
    if 'mcs' not in ticks_df.columns:
        print("  Skipping MCS transitions — no 'mcs' column")
        return

    mcs_series = ticks_df['mcs'].dropna().astype(int)

    # Time-series plot
    fig, ax = plt.subplots(figsize=(12, 3))
    ax.plot(range(len(mcs_series)), mcs_series, linewidth=0.5, alpha=0.8)
    ax.set_xlabel('Tick')
    ax.set_ylabel('MCS Level')
    ax.set_title('MCS Level Over Time')
    ax.set_yticks(range(8))
    plt.tight_layout()
    path = os.path.join(output_dir, 'mcs_timeseries.png')
    fig.savefig(path, dpi=150)
    plt.close(fig)
    print(f"  Saved {path}")

    # Transition matrix
    from_mcs = mcs_series.iloc[:-1].values
    to_mcs = mcs_series.iloc[1:].values
    matrix = np.zeros((8, 8), dtype=int)
    for f, t in zip(from_mcs, to_mcs):
        if 0 <= f <= 7 and 0 <= t <= 7:
            matrix[f, t] += 1

    fig, ax = plt.subplots(figsize=(6, 5))
    im = ax.imshow(matrix, cmap='YlOrRd', aspect='auto')
    ax.set_xlabel('To MCS')
    ax.set_ylabel('From MCS')
    ax.set_title('MCS Transition Matrix')
    ax.set_xticks(range(8))
    ax.set_yticks(range(8))
    for i in range(8):
        for j in range(8):
            if matrix[i, j] > 0:
                ax.text(j, i, str(matrix[i, j]), ha='center', va='center', fontsize=7)
    fig.colorbar(im, ax=ax, label='Count')
    plt.tight_layout()
    path = os.path.join(output_dir, 'mcs_transition_matrix.png')
    fig.savefig(path, dpi=150)
    plt.close(fig)
    print(f"  Saved {path}")


def plot_feature_outcome_correlation(ticks_df, output_dir):
    """Generate feature-outcome correlation matrix heatmap."""
    if 'outcome_label' not in ticks_df.columns:
        print("  Skipping feature-outcome correlation — no outcome labels")
        return

    labeled = ticks_df[ticks_df['outcome_label'].notna()].copy()
    if labeled.empty:
        print("  Skipping feature-outcome correlation — no outcome labels present")
        return

    # Binary target: 1=good, 0=bad/marginal
    labeled['outcome_good'] = (labeled['outcome_label'] == 'good').astype(int)

    feature_cols = [
        'rssi', 'snr', 'snr_ema', 'loss_rate', 'fec_pressure',
        'rf_score', 'loss_score', 'fec_score', 'div_score', 'score',
        'ema_fast', 'ema_slow',
    ]
    derived_cols = [
        'snr_roc', 'loss_accel', 'fec_saturation',
        'score_volatility', 'link_budget_margin', 'time_since_change',
    ]
    all_cols = [c for c in feature_cols + derived_cols if c in labeled.columns]
    all_cols.append('outcome_good')

    corr = labeled[all_cols].corr()

    fig, ax = plt.subplots(figsize=(10, 8))
    im = ax.imshow(corr.values, cmap='RdBu_r', vmin=-1, vmax=1, aspect='auto')
    ax.set_xticks(range(len(all_cols)))
    ax.set_yticks(range(len(all_cols)))
    ax.set_xticklabels(all_cols, rotation=45, ha='right', fontsize=7)
    ax.set_yticklabels(all_cols, fontsize=7)
    ax.set_title('Feature-Outcome Correlation Matrix')
    fig.colorbar(im, ax=ax, label='Pearson r')
    plt.tight_layout()
    path = os.path.join(output_dir, 'feature_outcome_correlation.png')
    fig.savefig(path, dpi=150)
    plt.close(fig)
    print(f"  Saved {path}")


def analyze_failure_modes(ticks_df, output_dir):
    """Identify and summarize failure patterns (bad outcomes)."""
    if 'outcome_label' not in ticks_df.columns:
        print("  Skipping failure mode analysis — no outcome labels")
        return

    bad_indices = ticks_df.index[ticks_df['outcome_label'] == 'bad'].tolist()
    if not bad_indices:
        print("  No 'bad' outcomes found — skipping failure mode analysis")
        return

    feature_cols = [
        'snr_ema', 'loss_rate', 'fec_pressure', 'score',
        'snr_roc', 'loss_accel', 'score_volatility', 'link_budget_margin',
    ]
    available = [c for c in feature_cols if c in ticks_df.columns]

    # Collect stats from 5 ticks preceding each bad outcome
    pre_failure_rows = []
    for idx in bad_indices:
        pos = ticks_df.index.get_loc(idx)
        start = max(0, pos - 5)
        pre_failure_rows.append(ticks_df.iloc[start:pos][available])

    if pre_failure_rows:
        pre_failure = pd.concat(pre_failure_rows)
        summary = pre_failure.describe().loc[['mean', 'std']]
    else:
        summary = pd.DataFrame()

    mcs_at_failure = ticks_df.loc[bad_indices, 'mcs'] if 'mcs' in ticks_df.columns else None

    lines = [
        '# Failure Mode Analysis\n',
        f'Total bad outcomes: {len(bad_indices)}\n',
    ]
    if mcs_at_failure is not None:
        mode = mcs_at_failure.mode()
        mode_val = mode.iloc[0] if not mode.empty else 'N/A'
        lines.append(f'Most common MCS at failure: {mode_val}\n')
        lines.append(f'MCS distribution at failure:\n{mcs_at_failure.value_counts().to_string()}\n')

    lines.append('\n## Pre-failure Feature Statistics (5 ticks before)\n')
    if not summary.empty:
        lines.append(summary.to_string())
    lines.append('\n')

    path = os.path.join(output_dir, 'failure_modes.md')
    with open(path, 'w') as f:
        f.write('\n'.join(lines))
    print(f"  Saved {path}")


def generate_summary_report(ticks_df, outcomes_df, output_dir):
    """Generate a markdown summary statistics report."""
    total_ticks = len(ticks_df)
    total_changes = ticks_df['changed'].sum()
    adapters = ticks_df['adapter'].unique()

    lines = [
        '# Telemetry Analysis Summary Report\n',
        f'Total ticks: {total_ticks}',
        f'Total profile changes: {total_changes}',
        f'Adapters: {", ".join(str(a) for a in adapters)}',
        '',
    ]

    # Outcome distribution
    if not outcomes_df.empty and 'label' in outcomes_df.columns:
        counts = outcomes_df['label'].value_counts()
        total_outcomes = len(outcomes_df)
        lines.append('## Outcome Distribution\n')
        for label in ['good', 'marginal', 'bad']:
            c = counts.get(label, 0)
            pct = 100 * c / total_outcomes if total_outcomes > 0 else 0
            lines.append(f'- {label}: {c} ({pct:.1f}%)')
        lines.append('')

    # Feature statistics
    feature_cols = [
        'rssi', 'snr', 'snr_ema', 'loss_rate', 'fec_pressure',
        'score', 'ema_fast', 'ema_slow',
    ]
    derived = ['snr_roc', 'loss_accel', 'fec_saturation',
               'score_volatility', 'link_budget_margin', 'time_since_change']
    all_cols = [c for c in feature_cols + derived if c in ticks_df.columns]

    lines.append('## Feature Statistics\n')
    stats = ticks_df[all_cols].describe().loc[['mean', 'std', 'min', 'max']]
    lines.append(stats.to_string())
    lines.append('')

    # Per-adapter breakdown
    if len(adapters) > 1:
        lines.append('## Per-Adapter Breakdown\n')
        for adapter in adapters:
            subset = ticks_df[ticks_df['adapter'] == adapter]
            lines.append(f'### {adapter}\n')
            lines.append(f'Ticks: {len(subset)}, Changes: {subset["changed"].sum()}')
            lines.append(f'Score: mean={subset["score"].mean():.1f}, std={subset["score"].std():.1f}')
            lines.append('')

    path = os.path.join(output_dir, 'summary_report.md')
    with open(path, 'w') as f:
        f.write('\n'.join(lines))
    print(f"  Saved {path}")


def main():
    parser = argparse.ArgumentParser(
        description='Analyze adaptive link telemetry data')
    parser.add_argument('--input', required=True,
                        help='Directory containing telemetry_*.jsonl files')
    parser.add_argument('--output', required=True,
                        help='Directory to save analysis output')
    args = parser.parse_args()

    if not os.path.isdir(args.input):
        print(f"Error: Input directory not found: {args.input}", file=sys.stderr)
        sys.exit(1)

    os.makedirs(args.output, exist_ok=True)

    print(f"Loading telemetry from {args.input}...")
    ticks_df, outcomes_df = load_telemetry(args.input)
    print(f"  Loaded {len(ticks_df)} ticks, {len(outcomes_df)} outcomes")

    print("Computing derived features...")
    ticks_df = compute_all_features(ticks_df)

    print("Joining outcomes...")
    ticks_df = join_outcomes(ticks_df, outcomes_df)

    print("Generating analyses...")
    plot_score_distributions(ticks_df, args.output)
    plot_mcs_transitions(ticks_df, args.output)
    plot_feature_outcome_correlation(ticks_df, args.output)
    analyze_failure_modes(ticks_df, args.output)
    generate_summary_report(ticks_df, outcomes_df, args.output)

    print(f"\nDone. Output saved to {args.output}/")


if __name__ == '__main__':
    main()
