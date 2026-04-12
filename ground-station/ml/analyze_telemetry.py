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
    required_cols = ['ant', 'rssi', 'rssi_min']
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

    # Plot 2: RSSI spread distribution
    ax = axes[1]
    ax.hist(data['rssi_spread'], bins=40, edgecolor='black', alpha=0.7, color='steelblue')
    ax.set_xlabel('RSSI Spread (dB)')
    ax.set_ylabel('Count')
    ax.set_title('RSSI Spread Distribution (best - worst antenna)')
    ax.axvline(data['rssi_spread'].mean(), color='red', linestyle='--',
               label=f'Mean: {data["rssi_spread"].mean():.1f} dB')
    ax.legend()

    # Plot 3: Antenna count and RSSI spread over time
    ax = axes[2]
    ax2 = ax.twinx()
    ax.plot(data.index, data['ant'], 'b-', alpha=0.7, label='Antenna Count')
    ax2.plot(data.index, data['rssi_spread'], 'r-', alpha=0.7, label='RSSI Spread')
    ax.set_xlabel('Tick')
    ax.set_ylabel('Antenna Count', color='b')
    ax2.set_ylabel('RSSI Spread (dB)', color='r')
    ax.set_title('Antenna Count and RSSI Spread Over Time')
    ax.tick_params(axis='y', labelcolor='b')
    ax2.tick_params(axis='y', labelcolor='r')

    plt.tight_layout()
    path = os.path.join(output_dir, 'antenna_diversity_analysis.png')
    fig.savefig(path, dpi=150)
    plt.close(fig)
    print(f"  Saved {path}")


def plot_gate_analysis(ticks_df, output_dir):
    """Generate two-channel gate analysis: SNR margin, slope, emergency flags."""
    required_cols = ['margin_cur', 'margin_tgt', 'snr_slope', 'emergency']
    if not all(col in ticks_df.columns for col in required_cols):
        print("  Skipping gate analysis — missing columns")
        return

    data = ticks_df[required_cols + ['snr_ema']].dropna()
    if data.empty:
        print("  Skipping gate analysis — no valid data")
        return

    fig, axes = plt.subplots(2, 2, figsize=(16, 10))

    # Plot 1: Current vs target margin over time
    ax = axes[0, 0]
    ax.plot(data.index, data['margin_cur'], 'b-', alpha=0.7, label='Current MCS margin')
    ax.plot(data.index, data['margin_tgt'], 'g-', alpha=0.7, label='Target MCS margin')
    ax.axhline(0, color='black', linestyle='--', linewidth=0.5)
    ax.set_xlabel('Tick')
    ax.set_ylabel('SNR Margin (dB)')
    ax.set_title('SNR Margin Over Time')
    ax.legend(loc='lower right')

    # Plot 2: Current margin distribution
    ax = axes[0, 1]
    ax.hist(data['margin_cur'], bins=50, edgecolor='black', alpha=0.7, color='steelblue')
    ax.axvline(0, color='red', linestyle='--', label='Threshold (0 dB)')
    ax.axvline(data['margin_cur'].mean(), color='green', linestyle='--',
               label=f'Mean: {data["margin_cur"].mean():.1f} dB')
    ax.set_xlabel('SNR Margin (dB)')
    ax.set_ylabel('Count')
    ax.set_title('Current MCS Margin Distribution')
    ax.legend()

    # Plot 3: SNR slope over time
    ax = axes[1, 0]
    ax.plot(data.index, data['snr_slope'], 'purple', alpha=0.7)
    ax.axhline(0, color='black', linestyle='--', linewidth=0.5)
    ax.set_xlabel('Tick')
    ax.set_ylabel('SNR slope (dB/tick)')
    ax.set_title('SNR Trend (EMA of Δsnr_ema)')

    # Plot 4: Emergency flag timeline
    ax = axes[1, 1]
    ax.fill_between(data.index, 0, data['emergency'].astype(int),
                    alpha=0.5, color='red', step='pre')
    ax.set_ylim(-0.1, 1.1)
    ax.set_yticks([0, 1])
    ax.set_yticklabels(['normal', 'EMERGENCY'])
    ax.set_xlabel('Tick')
    ax.set_title(f'Emergency Events ({int(data["emergency"].sum())} ticks)')

    plt.tight_layout()
    path = os.path.join(output_dir, 'gate_analysis.png')
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


def analyze_failure_modes(ticks_df, output_dir):
    """Identify and summarize failure patterns (bad outcomes) with emergency drill-down."""
    if 'outcome_label' not in ticks_df.columns:
        print("  Skipping failure mode analysis — no outcome labels")
        return

    bad_indices = ticks_df.index[ticks_df['outcome_label'] == 'bad'].tolist()
    if not bad_indices:
        print("  No 'bad' outcomes found — skipping failure mode analysis")
        return

    feature_cols = [
        'snr_ema', 'snr_slope', 'loss_rate', 'fec_pressure', 'margin_cur',
        'snr_roc', 'loss_accel', 'snr_margin_volatility', 'link_budget_margin',
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

    # --- Emergency drill-down ---
    has_emergency = ('emergency' in ticks_df.columns and
                     'loss_rate' in ticks_df.columns and
                     'fec_pressure' in ticks_df.columns)
    if has_emergency:
        emerg = ticks_df[ticks_df['emergency'] == True]
        n_emerg = len(emerg)
        lines.append(f'\n## Emergency Drill-down ({n_emerg} ticks)\n')

        if n_emerg > 0:
            # Classify trigger type using the thresholds from the data:
            # We infer thresholds from which channel is active
            loss_triggered = emerg['loss_rate'] >= emerg['loss_rate'].quantile(0.5)
            fec_triggered = emerg['fec_pressure'] >= emerg['fec_pressure'].quantile(0.5)
            # More robust: just check which metric is elevated
            loss_only = (emerg['loss_rate'] > 0) & (emerg['fec_pressure'] == 0)
            fec_only = (emerg['loss_rate'] == 0) & (emerg['fec_pressure'] > 0)
            both = (emerg['loss_rate'] > 0) & (emerg['fec_pressure'] > 0)
            neither = (emerg['loss_rate'] == 0) & (emerg['fec_pressure'] == 0)

            lines.append('### Trigger breakdown\n')
            lines.append(f'- Loss only: {loss_only.sum()}')
            lines.append(f'- FEC only: {fec_only.sum()}')
            lines.append(f'- Both: {both.sum()}')
            lines.append(f'- Neither (SNR dropout): {neither.sum()}')
            lines.append('')

            lines.append('### Metrics during emergency\n')
            lines.append(f'- loss_rate: mean={emerg["loss_rate"].mean():.4f}, '
                         f'max={emerg["loss_rate"].max():.4f}')
            lines.append(f'- fec_pressure: mean={emerg["fec_pressure"].mean():.4f}, '
                         f'max={emerg["fec_pressure"].max():.4f}')
            lines.append(f'- margin_cur: mean={emerg["margin_cur"].mean():.2f} dB')
            lines.append('')

            if 'mcs' in emerg.columns:
                lines.append('### MCS during emergency\n')
                lines.append(emerg['mcs'].value_counts().sort_index().to_string())
                lines.append('')

            # Emergency → bad outcome linkage
            if 'outcome_label' in ticks_df.columns:
                bad_set = set(bad_indices)
                # Check 10-tick window after each emergency for bad outcomes
                emerg_led_to_bad = 0
                for idx in emerg.index:
                    pos = ticks_df.index.get_loc(idx)
                    window = ticks_df.iloc[pos:min(pos + 10, len(ticks_df))]
                    if window.index.isin(bad_set).any():
                        emerg_led_to_bad += 1
                emerg_resolved = n_emerg - emerg_led_to_bad
                lines.append('### Emergency outcomes\n')
                lines.append(f'- Resolved (no bad outcome within 10 ticks): {emerg_resolved}')
                lines.append(f'- Led to bad outcome: {emerg_led_to_bad}')
                lines.append('')

    path = os.path.join(output_dir, 'failure_modes.md')
    with open(path, 'w') as f:
        f.write('\n'.join(lines))
    print(f"  Saved {path}")


def plot_failure_timelines(ticks_df, output_dir):
    """Plot tick-by-tick metrics for the 20 ticks before each bad outcome."""
    if 'outcome_label' not in ticks_df.columns:
        return

    bad_indices = ticks_df.index[ticks_df['outcome_label'] == 'bad'].tolist()
    if not bad_indices:
        return

    # Cap at 12 failures to keep the plot readable
    bad_indices = bad_indices[:12]
    n_failures = len(bad_indices)
    window = 20

    fig, axes = plt.subplots(n_failures, 1, figsize=(14, 3 * n_failures),
                             squeeze=False)

    for i, idx in enumerate(bad_indices):
        pos = ticks_df.index.get_loc(idx)
        start = max(0, pos - window)
        # Include the bad tick itself
        chunk = ticks_df.iloc[start:pos + 1].copy()
        ticks = range(len(chunk))
        ax = axes[i, 0]

        # Margin (left y-axis)
        if 'margin_cur' in chunk.columns:
            ax.plot(ticks, chunk['margin_cur'], 'b-', linewidth=1.5,
                    label='margin', zorder=3)
            ax.axhline(0, color='black', linestyle='--', linewidth=0.5)

        # MCS (right y-axis)
        ax2 = ax.twinx()
        if 'mcs' in chunk.columns:
            ax2.step(ticks, chunk['mcs'], 'g-', alpha=0.7, linewidth=1.2,
                     label='MCS', where='post')
            ax2.set_ylim(-0.5, 7.5)
            ax2.set_ylabel('MCS', color='g', fontsize=8)
            ax2.tick_params(axis='y', labelcolor='g', labelsize=7)

        # Loss rate (bar, scaled)
        if 'loss_rate' in chunk.columns:
            loss_vals = chunk['loss_rate'].fillna(0)
            ax.bar(ticks, -loss_vals * 20, width=0.6, color='red', alpha=0.4,
                   label='loss (scaled)', zorder=2)

        # FEC pressure (bar, scaled)
        if 'fec_pressure' in chunk.columns:
            fec_vals = chunk['fec_pressure'].fillna(0)
            ax.bar(ticks, -fec_vals * 10, width=0.3, color='orange', alpha=0.5,
                   label='FEC (scaled)', zorder=2)

        # Emergency ticks
        if 'emergency' in chunk.columns:
            emerg_ticks = [t for t, e in zip(ticks, chunk['emergency']) if e]
            for et in emerg_ticks:
                ax.axvspan(et - 0.4, et + 0.4, color='red', alpha=0.1, zorder=1)

        # Mark the bad-outcome tick
        ax.axvline(len(chunk) - 1, color='red', linewidth=2, linestyle='-',
                   zorder=4)

        fail_mcs = chunk['mcs'].iloc[-1] if 'mcs' in chunk.columns else '?'
        ax.set_title(f'Failure #{i+1}: MCS {fail_mcs:.0f} @ tick {idx}',
                     fontsize=9, fontweight='bold')
        ax.set_ylabel('Margin (dB)', fontsize=8)
        ax.tick_params(axis='both', labelsize=7)

        if i == 0:
            ax.legend(loc='upper left', fontsize=7)
        if i == n_failures - 1:
            ax.set_xlabel('Ticks before failure')

    plt.tight_layout()
    path = os.path.join(output_dir, 'failure_timelines.png')
    fig.savefig(path, dpi=150)
    plt.close(fig)
    print(f"  Saved {path}")


def generate_summary_report(ticks_df, outcomes_df, output_dir):
    """Generate a markdown summary statistics report."""
    total_ticks = len(ticks_df)
    total_changes = ticks_df['changed'].sum()
    adapters = ticks_df['adapter'].unique()

    lines = [
        '# Telemetry Analysis Summary Report\n',
        f'Total ticks: {total_ticks}',
        f'Total config changes: {total_changes}',
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
        'rssi', 'snr', 'snr_ema', 'snr_slope', 'loss_rate', 'fec_pressure',
        'margin_cur', 'margin_tgt',
    ]
    derived = ['snr_roc', 'loss_accel', 'fec_saturation',
               'snr_margin_volatility', 'link_budget_margin', 'time_since_change']
    all_cols = [c for c in feature_cols + derived if c in ticks_df.columns]

    lines.append('## Feature Statistics\n')
    stats = ticks_df[all_cols].describe().loc[['mean', 'std', 'min', 'max']]
    lines.append(stats.to_string())
    lines.append('')

    # Emergency event summary
    if 'emergency' in ticks_df.columns:
        n_emergency = int(ticks_df['emergency'].sum())
        pct = 100 * n_emergency / total_ticks if total_ticks > 0 else 0
        lines.append(f'## Emergency Events\n')
        lines.append(f'- Emergency ticks: {n_emergency} ({pct:.2f}%)')
        lines.append('')

    # Per-adapter breakdown
    if len(adapters) > 1:
        lines.append('## Per-Adapter Breakdown\n')
        for adapter in adapters:
            subset = ticks_df[ticks_df['adapter'] == adapter]
            lines.append(f'### {adapter}\n')
            lines.append(f'Ticks: {len(subset)}, Changes: {subset["changed"].sum()}')
            if 'margin_cur' in subset.columns:
                m = subset['margin_cur'].dropna()
                if not m.empty:
                    lines.append(f'Margin (dB): mean={m.mean():.1f}, std={m.std():.1f}')
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
    plot_loss_rate_fec_relationship(ticks_df, args.output)
    plot_mcs_snr_analysis(ticks_df, args.output)
    plot_antenna_diversity_analysis(ticks_df, args.output)
    plot_gate_analysis(ticks_df, args.output)
    plot_mcs_transitions(ticks_df, args.output)
    analyze_failure_modes(ticks_df, args.output)
    plot_failure_timelines(ticks_df, args.output)
    generate_summary_report(ticks_df, outcomes_df, args.output)

    print(f"\nDone. Output saved to {args.output}/")


if __name__ == '__main__':
    main()
