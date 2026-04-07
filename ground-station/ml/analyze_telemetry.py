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


def plot_rssi_snr_relationship(ticks_df, output_dir):
    """Generate RSSI vs SNR relationship visualizations."""
    if 'rssi' not in ticks_df.columns or 'snr' not in ticks_df.columns:
        print("  Skipping RSSI-SNR relationship — missing columns")
        return

    data = ticks_df[['rssi', 'snr']].dropna()
    if data.empty:
        print("  Skipping RSSI-SNR relationship — no valid data")
        return

    fig, axes = plt.subplots(1, 3, figsize=(18, 5))

    # Plot 1: Scatter plot with color by SNR
    ax = axes[0]
    scatter = ax.scatter(data['rssi'], data['snr'], c=data['snr'],
                         cmap='viridis', alpha=0.5, s=10)
    ax.set_xlabel('RSSI (dBm)')
    ax.set_ylabel('SNR (dB)')
    ax.set_title('RSSI vs SNR Scatter')
    fig.colorbar(scatter, ax=ax, label='SNR (dB)')

    # Plot 2: 2D histogram (density heatmap)
    ax = axes[1]
    h = ax.hist2d(data['rssi'], data['snr'], bins=30, cmap='YlOrRd')
    ax.set_xlabel('RSSI (dBm)')
    ax.set_ylabel('SNR (dB)')
    ax.set_title('RSSI-SNR Density')
    fig.colorbar(h[3], ax=ax, label='Count')

    # Plot 3: Time series with dual y-axis
    ax1 = axes[2]
    ax2 = ax1.twinx()
    ax1.plot(data.index, data['rssi'], 'b-', alpha=0.7, label='RSSI')
    ax2.plot(data.index, data['snr'], 'r-', alpha=0.7, label='SNR')
    ax1.set_xlabel('Tick')
    ax1.set_ylabel('RSSI (dBm)', color='b')
    ax2.set_ylabel('SNR (dB)', color='r')
    ax1.set_title('RSSI and SNR Over Time')
    ax1.tick_params(axis='y', labelcolor='b')
    ax2.tick_params(axis='y', labelcolor='r')

    plt.tight_layout()
    path = os.path.join(output_dir, 'rssi_snr_relationship.png')
    fig.savefig(path, dpi=150)
    plt.close(fig)
    print(f"  Saved {path}")


def plot_loss_rate_fec_relationship(ticks_df, output_dir):
    """Generate loss rate vs FEC pressure relationship visualizations."""
    if 'loss_rate' not in ticks_df.columns or 'fec_pressure' not in ticks_df.columns:
        print("  Skipping loss rate-FEC relationship — missing columns")
        return

    data = ticks_df[['loss_rate', 'fec_pressure']].dropna()
    if data.empty:
        print("  Skipping loss rate-FEC relationship — no valid data")
        return

    fig, axes = plt.subplots(1, 3, figsize=(18, 5))

    # Plot 1: Scatter plot with color by FEC pressure
    ax = axes[0]
    scatter = ax.scatter(data['loss_rate'], data['fec_pressure'],
                         c=data['fec_pressure'], cmap='YlOrRd', alpha=0.5, s=10)
    ax.set_xlabel('Loss Rate')
    ax.set_ylabel('FEC Pressure')
    ax.set_title('Loss Rate vs FEC Pressure Scatter')
    fig.colorbar(scatter, ax=ax, label='FEC Pressure')

    # Plot 2: 2D histogram (density heatmap)
    ax = axes[1]
    h = ax.hist2d(data['loss_rate'], data['fec_pressure'], bins=30, cmap='YlOrRd')
    ax.set_xlabel('Loss Rate')
    ax.set_ylabel('FEC Pressure')
    ax.set_title('Loss Rate - FEC Density')
    fig.colorbar(h[3], ax=ax, label='Count')

    # Plot 3: Time series with dual y-axis
    ax1 = axes[2]
    ax2 = ax1.twinx()
    ax1.plot(data.index, data['loss_rate'], 'r-', alpha=0.7, label='Loss Rate')
    ax2.plot(data.index, data['fec_pressure'], 'b-', alpha=0.7, label='FEC Pressure')
    ax1.set_xlabel('Tick')
    ax1.set_ylabel('Loss Rate', color='r')
    ax2.set_ylabel('FEC Pressure', color='b')
    ax1.set_title('Loss Rate and FEC Pressure Over Time')
    ax1.tick_params(axis='y', labelcolor='r')
    ax2.tick_params(axis='y', labelcolor='b')

    plt.tight_layout()
    path = os.path.join(output_dir, 'loss_rate_fec_relationship.png')
    fig.savefig(path, dpi=150)
    plt.close(fig)
    print(f"  Saved {path}")


def plot_mcs_snr_analysis(ticks_df, output_dir):
    """Generate MCS vs SNR analysis visualizations."""
    if 'mcs' not in ticks_df.columns or 'snr' not in ticks_df.columns:
        print("  Skipping MCS-SNR analysis — missing columns")
        return

    data = ticks_df[['mcs', 'snr']].dropna()
    if data.empty:
        print("  Skipping MCS-SNR analysis — no valid data")
        return

    data = data.copy()
    data['mcs'] = data['mcs'].astype(int)

    fig, axes = plt.subplots(1, 3, figsize=(18, 5))

    # Plot 1: MCS vs SNR scatter with color by MCS
    ax = axes[0]
    scatter = ax.scatter(data['snr'], data['mcs'], c=data['mcs'],
                         cmap='viridis', alpha=0.5, s=10)
    ax.set_xlabel('SNR (dB)')
    ax.set_ylabel('MCS Level')
    ax.set_title('MCS vs SNR Scatter')
    ax.set_yticks(range(8))
    fig.colorbar(scatter, ax=ax, label='MCS Level')

    # Plot 2: SNR distribution per MCS level (box plot)
    ax = axes[1]
    mcs_boxes = []
    mcs_labels = []
    for mcs in range(8):
        subset = data[data['mcs'] == mcs]['snr']
        if not subset.empty:
            mcs_boxes.append(subset)
            mcs_labels.append(f'MCS {mcs}')
    if mcs_boxes:
        bp = ax.boxplot(mcs_boxes, tick_labels=mcs_labels, patch_artist=True)
        for patch, mcs in zip(bp['boxes'], range(len(mcs_labels))):
            patch.set_facecolor(plt.cm.viridis(mcs / 7))
        ax.set_xlabel('MCS Level')
        ax.set_ylabel('SNR (dB)')
        ax.set_title('SNR Distribution per MCS')
    else:
        ax.text(0.5, 0.5, 'No MCS data available', ha='center', va='center')

    # Plot 3: MCS over time with SNR as background
    ax = axes[2]
    ax2 = ax.twinx()
    ax.fill_between(data.index, data['snr'], alpha=0.3, color='gray', label='SNR')
    ax.plot(range(len(data)), data['mcs'], 'b-', linewidth=0.5, alpha=0.8, label='MCS')
    ax.set_xlabel('Tick')
    ax.set_ylabel('MCS Level', color='b')
    ax2.set_ylabel('SNR (dB)', color='gray')
    ax.set_title('MCS and SNR Over Time')
    ax.set_yticks(range(8))
    ax.tick_params(axis='y', labelcolor='b')
    ax2.tick_params(axis='y', labelcolor='gray')

    plt.tight_layout()
    path = os.path.join(output_dir, 'mcs_snr_analysis.png')
    fig.savefig(path, dpi=150)
    plt.close(fig)
    print(f"  Saved {path}")


def plot_antenna_diversity_analysis(ticks_df, output_dir):
    """Generate antenna diversity analysis visualizations."""
    required_cols = ['ant', 'rssi', 'rssi_min', 'div_score']
    if not all(col in ticks_df.columns for col in required_cols):
        print("  Skipping antenna diversity analysis — missing columns")
        return

    data = ticks_df[required_cols].dropna()
    if data.empty:
        print("  Skipping antenna diversity analysis — no valid data")
        return

    data = data.copy()
    data['rssi_spread'] = data['rssi'] - data['rssi_min']

    fig, axes = plt.subplots(1, 3, figsize=(18, 5))

    # Plot 1: Antenna count distribution
    ax = axes[0]
    ant_counts = data['ant'].value_counts().sort_index()
    ax.bar(ant_counts.index.astype(str), ant_counts.values, color='steelblue')
    ax.set_xlabel('Number of Antennas')
    ax.set_ylabel('Count')
    ax.set_title('Antenna Count Distribution')

    # Plot 2: RSSI spread vs diversity score
    ax = axes[1]
    scatter = ax.scatter(data['rssi_spread'], data['div_score'],
                         c=data['rssi_spread'], cmap='YlOrRd', alpha=0.5, s=10)
    ax.set_xlabel('RSSI Spread (dB)')
    ax.set_ylabel('Diversity Score')
    ax.set_title('RSSI Spread vs Diversity Score')
    fig.colorbar(scatter, ax=ax, label='RSSI Spread (dB)')

    # Plot 3: Antenna count and diversity score over time
    ax = axes[2]
    ax2 = ax.twinx()
    ax.plot(data.index, data['ant'], 'b-', alpha=0.7, label='Antenna Count')
    ax2.plot(data.index, data['div_score'], 'r-', alpha=0.7, label='Diversity Score')
    ax.set_xlabel('Tick')
    ax.set_ylabel('Antenna Count', color='b')
    ax2.set_ylabel('Diversity Score', color='r')
    ax.set_title('Antenna Count and Diversity Score Over Time')
    ax.tick_params(axis='y', labelcolor='b')
    ax2.tick_params(axis='y', labelcolor='r')

    plt.tight_layout()
    path = os.path.join(output_dir, 'antenna_diversity_analysis.png')
    fig.savefig(path, dpi=150)
    plt.close(fig)
    print(f"  Saved {path}")


def plot_score_components_analysis(ticks_df, output_dir):
    """Generate individual score components analysis visualizations."""
    required_cols = ['rf_score', 'loss_score', 'fec_score', 'div_score', 'score']
    if not all(col in ticks_df.columns for col in required_cols):
        print("  Skipping score components analysis — missing columns")
        return

    data = ticks_df[required_cols].dropna()
    if data.empty:
        print("  Skipping score components analysis — no valid data")
        return

    fig, axes = plt.subplots(2, 2, figsize=(16, 10))

    # Plot 1: Score components time series (stacked)
    ax = axes[0, 0]
    colors = {'rf_score': 'blue', 'loss_score': 'green', 'fec_score': 'orange', 'div_score': 'red'}
    for col in ['rf_score', 'loss_score', 'fec_score', 'div_score']:
        ax.plot(data.index, data[col], label=col, color=colors[col], alpha=0.7)
    ax.set_xlabel('Tick')
    ax.set_ylabel('Score Component (0-1)')
    ax.set_title('Individual Score Components Over Time')
    ax.legend(loc='lower right')
    ax.set_ylim(0, 1.1)

    # Plot 2: Score vs components scatter matrix (simplified)
    ax = axes[0, 1]
    scatter = ax.scatter(data['rf_score'], data['div_score'],
                         c=data['score'], cmap='viridis', alpha=0.5, s=10)
    ax.set_xlabel('RF Score')
    ax.set_ylabel('Diversity Score')
    ax.set_title('RF Score vs Diversity Score')
    fig.colorbar(scatter, ax=ax, label='Total Score')

    # Plot 3: Loss and FEC scores time series
    ax = axes[1, 0]
    ax.plot(data.index, data['loss_score'], 'green', alpha=0.7, label='Loss Score')
    ax.plot(data.index, data['fec_score'], 'orange', alpha=0.7, label='FEC Score')
    ax.set_xlabel('Tick')
    ax.set_ylabel('Score Component (0-1)')
    ax.set_title('Loss and FEC Score Components')
    ax.legend()
    ax.set_ylim(0, 1.1)

    # Plot 4: Total score distribution
    ax = axes[1, 1]
    ax.hist(data['score'], bins=50, edgecolor='black', alpha=0.7, color='steelblue')
    ax.set_xlabel('Total Score')
    ax.set_ylabel('Count')
    ax.set_title('Total Score Distribution')
    ax.axvline(data['score'].mean(), color='red', linestyle='--',
               label=f'Mean: {data["score"].mean():.0f}')
    ax.legend()

    plt.tight_layout()
    path = os.path.join(output_dir, 'score_components_analysis.png')
    fig.savefig(path, dpi=150)
    plt.close(fig)
    print(f"  Saved {path}")


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
    plot_rssi_snr_relationship(ticks_df, args.output)
    plot_loss_rate_fec_relationship(ticks_df, args.output)
    plot_mcs_snr_analysis(ticks_df, args.output)
    plot_antenna_diversity_analysis(ticks_df, args.output)
    plot_score_components_analysis(ticks_df, args.output)
    plot_score_distributions(ticks_df, args.output)
    plot_mcs_transitions(ticks_df, args.output)
    plot_feature_outcome_correlation(ticks_df, args.output)
    analyze_failure_modes(ticks_df, args.output)
    generate_summary_report(ticks_df, outcomes_df, args.output)

    print(f"\nDone. Output saved to {args.output}/")


if __name__ == '__main__':
    main()
