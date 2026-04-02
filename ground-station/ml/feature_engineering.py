"""Offline feature engineering for adaptive link telemetry data.

Loads Phase 0 JSONL telemetry, computes derived features for ML analysis.
Dev-only — requires numpy and pandas (not deployed to target hardware).
"""

import json
import glob
import os

import numpy as np
import pandas as pd

# 802.11n MCS SNR thresholds (dB) — same values as in alink_gs
MCS_SNR_THRESHOLDS = [5, 8, 11, 14, 17, 20, 23, 26]

REQUIRED_TICK_FIELDS = [
    'ts', 'rssi', 'snr', 'rssi_min', 'ant', 'pkt_all', 'pkt_lost',
    'pkt_fec', 'fec_k', 'fec_n', 'loss_rate', 'fec_pressure',
    'rf_score', 'loss_score', 'fec_score', 'div_score', 'score',
    'ema_fast', 'ema_slow', 'snr_ema', 'changed', 'adapter',
]


def load_telemetry(directory):
    """Load telemetry JSONL files, returning (ticks_df, outcomes_df).

    Separates tick records from outcome records (identified by type=="outcome").
    Validates that required fields are present in tick records.

    Args:
        directory: Path to directory containing telemetry_*.jsonl files.

    Returns:
        Tuple of (ticks_df, outcomes_df) DataFrames.

    Raises:
        FileNotFoundError: If directory doesn't exist or has no telemetry files.
        ValueError: If required fields are missing from tick records.
    """
    if not os.path.isdir(directory):
        raise FileNotFoundError(f"Directory not found: {directory}")

    files = sorted(glob.glob(os.path.join(directory, 'telemetry_*.jsonl')))
    if not files:
        raise FileNotFoundError(f"No telemetry_*.jsonl files found in {directory}")

    ticks = []
    outcomes = []

    for filepath in files:
        with open(filepath, 'r') as f:
            for line in f:
                line = line.strip()
                if not line:
                    continue
                record = json.loads(line)
                if record.get('type') == 'outcome':
                    outcomes.append(record)
                else:
                    ticks.append(record)

    if not ticks:
        raise ValueError("No tick records found in telemetry files")

    ticks_df = pd.DataFrame(ticks)
    outcomes_df = pd.DataFrame(outcomes) if outcomes else pd.DataFrame(
        columns=['type', 'change_ts', 'avg_loss', 'max_loss', 'label', 'ticks']
    )

    # Validate required fields
    missing = [f for f in REQUIRED_TICK_FIELDS if f not in ticks_df.columns]
    if missing:
        raise ValueError(f"Missing required fields in tick records: {missing}")

    return ticks_df, outcomes_df


def join_outcomes(ticks_df, outcomes_df):
    """Join outcome labels back to the ticks that triggered profile changes.

    Matches outcome records to ticks via change_ts -> ts, adding
    outcome_label, outcome_avg_loss, and outcome_max_loss columns.

    Args:
        ticks_df: DataFrame of tick records.
        outcomes_df: DataFrame of outcome records.

    Returns:
        ticks_df with outcome columns added (NaN for non-change ticks).
    """
    df = ticks_df.copy()
    df['outcome_label'] = pd.Series([None] * len(df), dtype=object)
    df['outcome_avg_loss'] = np.nan
    df['outcome_max_loss'] = np.nan

    if outcomes_df.empty or 'change_ts' not in outcomes_df.columns:
        return df

    outcome_map = {}
    for _, row in outcomes_df.iterrows():
        outcome_map[row['change_ts']] = row

    for idx, tick in df.iterrows():
        ts = tick['ts']
        if ts in outcome_map:
            outcome = outcome_map[ts]
            df.at[idx, 'outcome_label'] = outcome['label']
            df.at[idx, 'outcome_avg_loss'] = outcome['avg_loss']
            df.at[idx, 'outcome_max_loss'] = outcome['max_loss']

    return df


def compute_snr_roc(df):
    """Compute SNR rate of change from consecutive snr_ema values.

    Returns a Series with the difference between consecutive snr_ema values.
    First tick gets 0.0.
    """
    return df['snr_ema'].diff().fillna(0.0)


def compute_loss_accel(df):
    """Compute loss rate acceleration (second derivative of loss_rate).

    Returns a Series. Ticks without enough history get 0.0.
    """
    first_deriv = df['loss_rate'].diff()
    second_deriv = first_deriv.diff()
    # Need at least 3 data points for a meaningful second derivative
    result = second_deriv.fillna(0.0)
    if len(result) > 0:
        result.iloc[0] = 0.0
    if len(result) > 1:
        result.iloc[1] = 0.0
    return result


def compute_fec_saturation(df):
    """Compute FEC saturation proximity from fec_pressure.

    Direct passthrough of fec_pressure (already 0.0-1.0 range).
    """
    return df['fec_pressure'].clip(0.0, 1.0)


def compute_score_volatility(df, window=20):
    """Compute rolling standard deviation of score over a trailing window.

    Uses min_periods=1 so early ticks use all available data.
    """
    return df['score'].rolling(window=window, min_periods=1).std().fillna(0.0)


def compute_link_budget_margin(df):
    """Compute link budget margin: snr_ema - MCS_SNR_THRESHOLD[current_mcs].

    Requires 'mcs' column. Returns NaN for ticks without MCS data.
    """
    if 'mcs' not in df.columns:
        return pd.Series(np.nan, index=df.index)

    thresholds = df['mcs'].map(
        lambda m: MCS_SNR_THRESHOLDS[int(m)] if pd.notna(m) and 0 <= int(m) <= 7 else np.nan
    )
    return df['snr_ema'] - thresholds


def compute_time_since_change(df):
    """Compute elapsed milliseconds since the last profile change.

    Returns NaN for ticks before the first profile change.
    """
    result = pd.Series(np.nan, index=df.index)
    last_change_ts = None

    for idx in df.index:
        if df.at[idx, 'changed']:
            last_change_ts = df.at[idx, 'ts']
        if last_change_ts is not None:
            result.at[idx] = df.at[idx, 'ts'] - last_change_ts

    return result


def compute_all_features(df):
    """Add all derived feature columns to a ticks DataFrame.

    Returns a new DataFrame with columns added:
        snr_roc, loss_accel, fec_saturation, score_volatility,
        link_budget_margin, time_since_change
    """
    out = df.copy()
    out['snr_roc'] = compute_snr_roc(out)
    out['loss_accel'] = compute_loss_accel(out)
    out['fec_saturation'] = compute_fec_saturation(out)
    out['score_volatility'] = compute_score_volatility(out)
    out['link_budget_margin'] = compute_link_budget_margin(out)
    out['time_since_change'] = compute_time_since_change(out)
    return out
