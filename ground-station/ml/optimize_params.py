"""Bayesian parameter optimization for alink_gs ProfileSelector.

Uses optuna's TPE sampler to search the 37-parameter space per adapter type.
Evaluates candidate parameter sets using the replay simulator against
historical telemetry data.
"""

import argparse
import configparser
import datetime
import os
import sys
import textwrap

import optuna
import pandas as pd
import yaml

sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..'))
from ml.replay_simulator import DEFAULT_CONFIG, ReplaySimulator
from ml.feature_engineering import load_telemetry


def _load_adapter_constraints(adapters_yaml_path):
    """Load per-adapter constraints from wlan_adapters.yaml.

    Returns dict mapping adapter_id -> {max_mcs: int, bandwidths: list}.
    """
    if not os.path.exists(adapters_yaml_path):
        return {}

    with open(adapters_yaml_path) as f:
        data = yaml.safe_load(f)

    constraints = {}
    for adapter_id, info in data.get('profiles', {}).items():
        mcs_list = info.get('mcs', [0, 1, 2, 3, 4, 5, 6, 7])
        bw_list = info.get('bw', [20])
        constraints[adapter_id] = {
            'max_mcs': max(mcs_list) if mcs_list else 7,
            'bandwidths': bw_list,
        }
    return constraints


class ParameterSpace:
    """Defines the 37 tunable parameters with types, bounds, and constraints."""

    def __init__(self, adapter_constraints=None):
        """Initialize parameter space.

        Args:
            adapter_constraints: Optional dict with 'max_mcs' and 'bandwidths'
                                 for the target adapter.
        """
        self.constraints = adapter_constraints or {
            'max_mcs': 7,
            'bandwidths': [20, 40],
        }

    def define_trial(self, trial):
        """Map optuna trial suggestions to a ConfigParser.

        Handles sum-to-1 constraints by deriving dependent weights.

        Args:
            trial: optuna.Trial instance.

        Returns:
            configparser.ConfigParser with candidate parameters.
        """
        config = configparser.ConfigParser()
        config.read_string(DEFAULT_CONFIG)
        config.set('profile selection', 'dynamic_mode', 'True')

        # --- [scoring] weights: sum to 1.0 ---
        rf_weight = trial.suggest_float('rf_weight', 0.1, 0.8)
        loss_weight = trial.suggest_float('loss_weight', 0.05, 0.5)
        fec_weight = trial.suggest_float('fec_weight', 0.01, 0.4)
        diversity_weight = max(0.0, 1.0 - rf_weight - loss_weight - fec_weight)
        diversity_weight = min(diversity_weight, 0.5)

        config.set('scoring', 'rf_weight', str(rf_weight))
        config.set('scoring', 'loss_weight', str(loss_weight))
        config.set('scoring', 'fec_weight', str(fec_weight))
        config.set('scoring', 'diversity_weight', str(diversity_weight))

        config.set('scoring', 'max_loss_rate',
                   str(trial.suggest_float('max_loss_rate', 0.02, 0.3)))
        config.set('scoring', 'max_rssi_spread',
                   str(trial.suggest_int('max_rssi_spread', 5, 40)))

        # --- [weights] RF normalization: sum to 1.0 ---
        snr_weight = trial.suggest_float('snr_weight', 0.1, 0.9)
        config.set('weights', 'snr_weight', str(snr_weight))
        config.set('weights', 'rssi_weight', str(1.0 - snr_weight))

        # --- [ranges] ---
        config.set('ranges', 'SNR_MIN',
                   str(trial.suggest_int('SNR_MIN', 5, 20)))
        config.set('ranges', 'SNR_MAX',
                   str(trial.suggest_int('SNR_MAX', 25, 45)))
        config.set('ranges', 'RSSI_MIN',
                   str(trial.suggest_int('RSSI_MIN', -95, -65)))
        config.set('ranges', 'RSSI_MAX',
                   str(trial.suggest_int('RSSI_MAX', -50, -20)))

        # --- [profile selection] ---
        ps = 'profile selection'
        config.set(ps, 'ema_fast_alpha',
                   str(trial.suggest_float('ema_fast_alpha', 0.1, 0.9)))
        config.set(ps, 'ema_slow_alpha',
                   str(trial.suggest_float('ema_slow_alpha', 0.01, 0.3)))
        config.set(ps, 'predict_multi',
                   str(trial.suggest_float('predict_multi', 0.0, 3.0)))
        config.set(ps, 'hysteresis_percent',
                   str(trial.suggest_float('hysteresis_percent', 1.0, 15.0)))
        config.set(ps, 'hysteresis_percent_down',
                   str(trial.suggest_float('hysteresis_percent_down', 1.0, 15.0)))
        config.set(ps, 'hold_fallback_mode_ms',
                   str(trial.suggest_int('hold_fallback_mode_ms', 200, 5000)))
        config.set(ps, 'hold_modes_down_ms',
                   str(trial.suggest_int('hold_modes_down_ms', 500, 10000)))
        config.set(ps, 'min_between_changes_ms',
                   str(trial.suggest_int('min_between_changes_ms', 50, 1000)))
        config.set(ps, 'upward_confidence_loops',
                   str(trial.suggest_int('upward_confidence_loops', 1, 10)))
        config.set(ps, 'fast_downgrade',
                   str(trial.suggest_categorical('fast_downgrade', [True, False])))

        # --- [dynamic] ---
        d = 'dynamic'
        config.set(d, 'snr_safety_margin',
                   str(trial.suggest_float('snr_safety_margin', 1.0, 8.0)))
        config.set(d, 'snr_ema_alpha',
                   str(trial.suggest_float('snr_ema_alpha', 0.05, 0.8)))
        config.set(d, 'loss_margin_weight',
                   str(trial.suggest_float('loss_margin_weight', 5.0, 50.0)))
        config.set(d, 'fec_margin_weight',
                   str(trial.suggest_float('fec_margin_weight', 1.0, 15.0)))

        adapter_max_mcs = self.constraints.get('max_mcs', 7)
        config.set(d, 'max_mcs',
                   str(trial.suggest_int('max_mcs', 2, adapter_max_mcs)))

        config.set(d, 'short_gi_snr_margin',
                   str(trial.suggest_float('short_gi_snr_margin', 2.0, 10.0)))
        config.set(d, 'loss_threshold_for_fec_downgrade',
                   str(trial.suggest_float('loss_threshold_for_fec_downgrade', 0.01, 0.15)))
        config.set(d, 'fec_redundancy_ratio',
                   str(trial.suggest_float('fec_redundancy_ratio', 0.15, 0.40)))
        config.set(d, 'utilization_factor',
                   str(trial.suggest_float('utilization_factor', 0.2, 0.7)))
        config.set(d, 'max_bitrate',
                   str(trial.suggest_int('max_bitrate', 15000, 40000)))
        config.set(d, 'min_bitrate',
                   str(trial.suggest_int('min_bitrate', 1000, 5000)))
        config.set(d, 'max_power',
                   str(trial.suggest_int('max_power', 1000, 2900)))
        config.set(d, 'min_power',
                   str(trial.suggest_int('min_power', 50, 1000)))

        bw_choices = self.constraints.get('bandwidths', [20, 40])
        config.set(d, 'bandwidth',
                   str(trial.suggest_categorical('bandwidth', bw_choices)))

        # --- [noise] ---
        config.set('noise', 'min_noise',
                   str(trial.suggest_float('min_noise', 0.001, 0.05)))
        config.set('noise', 'max_noise',
                   str(trial.suggest_float('max_noise', 0.05, 0.5)))
        config.set('noise', 'deduction_exponent',
                   str(trial.suggest_float('deduction_exponent', 0.1, 2.0)))

        # --- [error estimation] ---
        config.set('error estimation', 'kalman_estimate',
                   str(trial.suggest_float('kalman_estimate', 0.001, 0.05)))
        config.set('error estimation', 'kalman_error_estimate',
                   str(trial.suggest_float('kalman_error_estimate', 0.01, 1.0)))
        config.set('error estimation', 'process_variance',
                   str(trial.suggest_float('process_variance', 1e-7, 1e-3, log=True)))
        config.set('error estimation', 'measurement_variance',
                   str(trial.suggest_float('measurement_variance', 0.001, 0.1)))

        return config


class AdapterOptimizer:
    """Runs Bayesian optimization for a single adapter type."""

    def __init__(self, adapter_id, ticks_df, adapter_constraints=None,
                 n_trials=200, seed=42):
        """Initialize optimizer.

        Args:
            adapter_id: Adapter identifier for filtering telemetry.
            ticks_df: DataFrame of telemetry ticks (may contain multiple adapters).
            adapter_constraints: Optional constraints from wlan_adapters.yaml.
            n_trials: Number of optimization trials.
            seed: Random seed for reproducibility.
        """
        self.adapter_id = adapter_id
        self.n_trials = n_trials
        self.seed = seed
        self.param_space = ParameterSpace(adapter_constraints)

        # Filter ticks to this adapter
        if 'adapter' in ticks_df.columns and adapter_id != 'all':
            self.ticks_df = ticks_df[ticks_df['adapter'] == adapter_id].copy()
        else:
            self.ticks_df = ticks_df.copy()

        if len(self.ticks_df) == 0:
            raise ValueError(f"No telemetry data for adapter '{adapter_id}'")

    def objective(self, trial):
        """Optuna objective function. Returns negative fitness (minimized)."""
        config = self.param_space.define_trial(trial)
        sim = ReplaySimulator(self.ticks_df, config)
        result = sim.run()
        return -result.total_fitness

    def optimize(self):
        """Run Bayesian optimization.

        Returns:
            Tuple of (best_config, best_fitness, study).
        """
        optuna.logging.set_verbosity(optuna.logging.WARNING)
        sampler = optuna.samplers.TPESampler(seed=self.seed)
        study = optuna.create_study(direction='minimize', sampler=sampler)
        study.optimize(self.objective, n_trials=self.n_trials)

        # Reconstruct best config
        best_trial = study.best_trial
        best_config = self.param_space.define_trial(best_trial)
        best_fitness = -best_trial.value

        return best_config, best_fitness, study

    def get_default_fitness(self):
        """Run replay with default parameters for comparison."""
        config = configparser.ConfigParser()
        config.read_string(DEFAULT_CONFIG)
        config.set('profile selection', 'dynamic_mode', 'True')
        sim = ReplaySimulator(self.ticks_df, config)
        result = sim.run()
        return result.total_fitness, result


def print_param_importance(study, top_n=20):
    """Print parameter importance ranking from optimization study.
    
    Uses Fanova importance analysis to determine which parameters
    most influence the fitness score.
    
    Args:
        study: Completed optuna study object.
        top_n: Number of top parameters to display.
    """
    try:
        importances = optuna.importance.get_param_importances(
            study,
            evaluator=optuna.importance.FanovaImportanceEvaluator(
                n_trees=64,      # Number of trees for ensemble
                max_depth=32,    # Maximum tree depth
                seed=42          # Random seed for reproducibility
            )
        )
        
        # Sort by importance (descending)
        sorted_importances = sorted(
            importances.items(),
            key=lambda x: x[1],
            reverse=True
        )
        
        print(f"\n  Parameter Importance (top {top_n}):")
        print(f"  {'='*50}")
        
        total_importance = sum(importances.values()) or 1.0
        
        # Categorize importance levels
        high_importance = []
        medium_importance = []
        low_importance = []
        noise_params = []
        
        for param, importance in sorted_importances[:top_n]:
            pct = (importance / total_importance) * 100
            bar = '█' * int(pct / 2)  # Visual bar (max 50 chars)
            
            # Categorize based on relative importance
            if pct >= 5:
                high_importance.append((param, pct))
            elif pct >= 2:
                medium_importance.append((param, pct))
            elif pct >= 0.5:
                low_importance.append((param, pct))
            else:
                noise_params.append((param, pct))
            
            print(f"    {pct:5.1f}% |{bar:<30}| {param}")
        
        # Print categorization summary
        print(f"\n  Importance Categories:")
        print(f"    High (>5%):   {len(high_importance)} params - {', '.join(p[0] for p in high_importance[:5])}{'...' if len(high_importance) > 5 else ''}")
        print(f"    Medium (2-5%): {len(medium_importance)} params")
        print(f"    Low (0.5-2%):  {len(low_importance)} params")
        print(f"    Noise (<0.5%): {len(noise_params)} params - likely candidates to fix at defaults")
        
        if noise_params:
            print(f"\n  Low-impact parameters (consider fixing to defaults):")
            for param, pct in noise_params:
                print(f"    {param}: {pct:.2f}%")
                
    except Exception as e:
        print(f"\n  Could not compute parameter importance: {e}")
        print("  (This may happen with too few trials or insufficient parameter variation)")


def write_optimized_config(config, output_path, adapter_id, n_trials,
                           default_fitness, optimized_fitness):
    """Write optimized config as INI file with metadata header.

    Args:
        config: ConfigParser with optimized parameters.
        output_path: Path to write the config file.
        adapter_id: Adapter identifier.
        n_trials: Number of trials used.
        default_fitness: Fitness with default parameters.
        optimized_fitness: Fitness with optimized parameters.
    """
    os.makedirs(os.path.dirname(output_path) or '.', exist_ok=True)

    header = textwrap.dedent(f"""\
    # Optimized alink_gs configuration for adapter: {adapter_id}
    # Generated: {datetime.datetime.now().isoformat()}
    # Optimization: {n_trials} trials via Bayesian optimization (optuna TPE)
    # Fitness: default={default_fitness:.4f} -> optimized={optimized_fitness:.4f}
    #          improvement={((optimized_fitness - default_fitness) / max(abs(default_fitness), 0.001)) * 100:.1f}%
    """)

    with open(output_path, 'w') as f:
        f.write(header + '\n')
        config.write(f)


def main():
    parser = argparse.ArgumentParser(
        description='Optimize alink_gs parameters per adapter using Bayesian optimization')
    parser.add_argument('--adapter', required=True,
                        help='Adapter ID to optimize (or "all" for each adapter)')
    parser.add_argument('--telemetry-dir', required=True,
                        help='Directory containing telemetry JSONL files')
    parser.add_argument('--output-dir', default='config',
                        help='Directory for output config files (default: config)')
    parser.add_argument('--n-trials', type=int, default=200,
                        help='Number of optimization trials (default: 200)')
    parser.add_argument('--seed', type=int, default=42,
                        help='Random seed for reproducibility (default: 42)')
    parser.add_argument('--adapters-yaml',
                        default=os.path.join(os.path.dirname(__file__),
                                             '..', '..', 'config', 'wlan_adapters.yaml'),
                        help='Path to wlan_adapters.yaml')
    args = parser.parse_args()

    # Load telemetry
    print(f"Loading telemetry from {args.telemetry_dir}...")
    ticks_df, _ = load_telemetry(args.telemetry_dir)
    print(f"  Loaded {len(ticks_df)} ticks")

    # Load adapter constraints
    adapter_constraints = _load_adapter_constraints(args.adapters_yaml)

    # Determine which adapters to optimize
    if args.adapter == 'all':
        if 'adapter' in ticks_df.columns:
            adapters = ticks_df['adapter'].unique().tolist()
        else:
            adapters = ['default']
    else:
        adapters = [args.adapter]

    for adapter_id in adapters:
        print(f"\n{'='*60}")
        print(f"Optimizing for adapter: {adapter_id}")
        print(f"{'='*60}")

        constraints = adapter_constraints.get(adapter_id, {
            'max_mcs': 7, 'bandwidths': [20, 40]
        })

        try:
            optimizer = AdapterOptimizer(
                adapter_id, ticks_df,
                adapter_constraints=constraints,
                n_trials=args.n_trials,
                seed=args.seed
            )
        except ValueError as e:
            print(f"  Skipping: {e}")
            continue

        # Get default baseline
        default_fitness, default_result = optimizer.get_default_fitness()
        print(f"  Default fitness: {default_fitness:.4f}")
        print(f"    Throughput: {default_result.mean_bitrate:.0f} kbps")
        print(f"    Loss: {default_result.mean_loss:.4f}")
        print(f"    Transitions: {default_result.transition_count}")
        print(f"    Crashes: {default_result.crash_events}")

        # Optimize
        print(f"\n  Running {args.n_trials} optimization trials...")
        best_config, best_fitness, study = optimizer.optimize()

        print(f"\n  Optimized fitness: {best_fitness:.4f}")
        improvement = ((best_fitness - default_fitness) / max(abs(default_fitness), 0.001)) * 100
        print(f"  Improvement: {improvement:+.1f}%")

        # Print parameter importance analysis
        print_param_importance(study)

        # Write output config
        output_path = os.path.join(args.output_dir,
                                   f'alink_gs.{adapter_id}.conf')
        write_optimized_config(best_config, output_path, adapter_id,
                               args.n_trials, default_fitness, best_fitness)
        print(f"\n  Config written to: {output_path}")

    print("\nDone.")


if __name__ == '__main__':
    main()
