"""Bayesian parameter optimization for alink_gs ProfileSelector.

Uses optuna's TPE sampler to search the two-channel gate parameter space
per adapter type. Evaluates candidate parameter sets using the replay
simulator against historical telemetry data.
"""

import argparse
import configparser
import datetime
import os
import re
import sys

import optuna
import pandas as pd

sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..'))
from ml.replay_simulator import load_config_from_file, ReplaySimulator
from ml.feature_engineering import load_telemetry


# =============================================================================
# Parameter registry
# =============================================================================
# Each entry: (name, section, kind, low, high, choices)
#   kind   = 'int' | 'float' | 'cat'
#   low/high are Optuna search-space bounds (used for int/float only)
#   choices is the list of options for categorical parameters
#
# Runtime overrides applied in ParameterSpace.define_trial():
#   - max_mcs: `high` is replaced by max_mcs_bound (read from config)
PARAM_REGISTRY = [
    ('hold_fallback_mode_ms',            'profile selection', 'int',     200,  5000, None),
    ('hold_modes_down_ms',               'profile selection', 'int',     500, 10000, None),
    ('min_between_changes_ms',           'profile selection', 'int',      50,  1000, None),
    ('upward_confidence_loops',          'profile selection', 'int',       1,    10, None),
    ('fast_downgrade',                   'profile selection', 'cat',    None,  None, [True, False]),
    ('hysteresis_up_db',                 'gate',              'float',   0.5,   6.0, None),
    ('hysteresis_down_db',               'gate',              'float',   0.0,   4.0, None),
    ('snr_slope_alpha',                  'gate',              'float',  0.05,   0.8, None),
    ('snr_predict_horizon_ticks',        'gate',              'float',   0.0,  10.0, None),
    ('emergency_loss_rate',              'gate',              'float',  0.05,  0.35, None),
    ('emergency_fec_pressure',           'gate',              'float',   0.4,  0.95, None),
    ('snr_safety_margin',                'dynamic',           'float',   1.0,   8.0, None),
    ('snr_ema_alpha',                    'dynamic',           'float',  0.05,   0.8, None),
    ('loss_margin_weight',               'dynamic',           'float',   5.0,  50.0, None),
    ('fec_margin_weight',                'dynamic',           'float',   1.0,  15.0, None),
    ('max_mcs',                          'dynamic',           'int',       2,  None, None),
    ('short_gi_snr_margin',              'dynamic',           'float',   2.0,  10.0, None),
    ('loss_threshold_for_fec_downgrade', 'dynamic',           'float',  0.01,  0.15, None),
    ('fec_redundancy_ratio',             'dynamic',           'float',  0.15,  0.40, None),
    ('utilization_factor',               'dynamic',           'float',   0.2,   0.7, None),
    ('max_bitrate',                      'dynamic',           'int',   15000, 40000, None),
    ('min_bitrate',                      'dynamic',           'int',    1000,  5000, None),
    ('max_power',                        'dynamic',           'int',    1000,  2900, None),
    ('min_power',                        'dynamic',           'int',      50,  1000, None),
]

PARAM_SECTION = {name: section for (name, section, *_) in PARAM_REGISTRY}
ALL_PARAM_NAMES = {p[0] for p in PARAM_REGISTRY}

_SECTION_ALIAS = {
    'profile_selection': 'profile selection',
    'profile-selection': 'profile selection',
    'gate': 'gate',
    'dynamic': 'dynamic',
}


def _read_skip_set(base_config_str):
    """Parse [optimizer].skip_optimize_params into a set of parameter names.

    Returns an empty set if the section or field is missing. Unknown names
    are dropped (with a warning) so typos don't silently freeze nothing.
    """
    cfg = configparser.ConfigParser()
    cfg.read_string(base_config_str)
    if not cfg.has_section('optimizer'):
        return set()
    raw = cfg.get('optimizer', 'skip_optimize_params', fallback='')
    tokens = {t.strip() for t in raw.split(',') if t.strip()}
    unknown = tokens - ALL_PARAM_NAMES
    if unknown:
        print(f"  Warning: unknown names in skip_optimize_params: {sorted(unknown)}")
    return tokens & ALL_PARAM_NAMES


def _expand_param_tokens(spec_str):
    """Expand a comma-separated CLI string into a set of parameter names.

    Accepts individual names and section shorthands from _SECTION_ALIAS.
    Raises SystemExit on unknown tokens.
    """
    selected = set()
    unknown = []
    for tok in (t.strip() for t in spec_str.split(',') if t.strip()):
        if tok in _SECTION_ALIAS:
            section = _SECTION_ALIAS[tok]
            selected.update(p[0] for p in PARAM_REGISTRY if p[1] == section)
        elif tok in ALL_PARAM_NAMES:
            selected.add(tok)
        else:
            unknown.append(tok)
    if unknown:
        raise SystemExit(
            f"Unknown parameter(s): {', '.join(unknown)}. "
            f"Run --list-params to see valid names."
        )
    return selected


def _resolve_selection(params_arg, exclude_arg, skip_set):
    """Compute the final set of parameter names to optimize.

    Precedence:
      - skip_set (from [optimizer].skip_optimize_params) always wins.
      - --params and --exclude are mutually exclusive (enforced by argparse).
      - Without either flag, every registry parameter is selected.
    """
    if params_arg:
        chosen = _expand_param_tokens(params_arg)
    elif exclude_arg:
        chosen = set(ALL_PARAM_NAMES) - _expand_param_tokens(exclude_arg)
    else:
        chosen = set(ALL_PARAM_NAMES)
    chosen -= skip_set
    if not chosen:
        raise SystemExit(
            "No parameters left to optimize after applying skip_optimize_params / --exclude."
        )
    return chosen


def _print_registry():
    """Print the parameter registry and exit-worthy info for --list-params."""
    by_section = {}
    for name, section, kind, low, high, choices in PARAM_REGISTRY:
        by_section.setdefault(section, []).append((name, kind, low, high, choices))

    print("Optimizable parameters:\n")
    for section in ('profile selection', 'gate', 'dynamic'):
        if section not in by_section:
            continue
        print(f"[{section}]")
        for name, kind, low, high, choices in by_section[section]:
            if kind == 'cat':
                bounds = f"choices={choices}"
            elif high is None:
                bounds = f"{kind} [{low}..adapter_max]"
            else:
                bounds = f"{kind} [{low}..{high}]"
            print(f"  {name:38s} {bounds}")
        print()
    print("Section shorthands for --params / --exclude:")
    print("  profile_selection, gate, dynamic")


class ParameterSpace:
    """Defines the tunable parameters for the two-channel gate with bounds."""

    def __init__(self, base_config_str, selected_params=None, max_mcs_bound=7):
        """Initialize parameter space.

        Args:
            base_config_str: INI config string used as the baseline for trials.
            selected_params: Optional set of parameter names to sample. When
                             None, every parameter in PARAM_REGISTRY is sampled
                             (current behavior). Names not in the set keep
                             their baseline value from base_config_str.
            max_mcs_bound: Upper bound for max_mcs parameter (default: 7).
        """
        self.base_config_str = base_config_str
        self.max_mcs_bound = max_mcs_bound
        self.selected_params = selected_params

    def define_trial(self, trial):
        """Map optuna trial suggestions to a ConfigParser.

        Args:
            trial: optuna.Trial instance.

        Returns:
            configparser.ConfigParser with candidate parameters.
        """
        config = configparser.ConfigParser()
        config.read_string(self.base_config_str)

        for name, section, kind, low, high, choices in PARAM_REGISTRY:
            if self.selected_params is not None and name not in self.selected_params:
                # Baseline value from base_config_str stays in place.
                continue

            # Runtime override for max_mcs upper bound.
            if name == 'max_mcs':
                high = self.max_mcs_bound

            if kind == 'int':
                val = trial.suggest_int(name, low, high)
            elif kind == 'float':
                val = trial.suggest_float(name, low, high)
            else:  # 'cat'
                val = trial.suggest_categorical(name, choices)

            config.set(section, name, str(val))

        return config


class AdapterOptimizer:
    """Runs Bayesian optimization for a single adapter type."""

    def __init__(self, adapter_id, ticks_df, base_config_str,
                 n_trials=200, seed=42, selected_params=None, max_mcs_bound=7):
        """Initialize optimizer.

        Args:
            adapter_id: Adapter identifier for filtering telemetry.
            ticks_df: DataFrame of telemetry ticks (may contain multiple adapters).
            base_config_str: INI config string used as the baseline.
            n_trials: Number of optimization trials.
            seed: Random seed for reproducibility.
            selected_params: Optional set of parameter names to optimize.
                             None = optimize every parameter in PARAM_REGISTRY.
            max_mcs_bound: Upper bound for max_mcs parameter (default: 7).
        """
        self.adapter_id = adapter_id
        self.base_config_str = base_config_str
        self.n_trials = n_trials
        self.seed = seed
        self.param_space = ParameterSpace(
            base_config_str, selected_params=selected_params, max_mcs_bound=max_mcs_bound
        )

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
        config.read_string(self.base_config_str)
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


_SECTION_RE = re.compile(r'^\s*\[([^\]]+)\]\s*$')
_KV_RE = re.compile(r'^(\s*)([A-Za-z_][A-Za-z0-9_]*)\s*=\s*(.*?)\s*$')


def write_optimized_config(baseline_lines, optimized_values, output_path,
                           adapter_id, n_trials, default_fitness,
                           optimized_fitness, tuned_param_names,
                           skipped_param_names):
    """Write a comment-preserving optimized config.

    The baseline file is walked line-by-line; only the value side of keys
    that were actually tuned is replaced. Every other line (section headers,
    blank lines, full-line comments, non-tuned keys) is emitted verbatim so
    hand-written documentation in the baseline survives the round-trip.

    Args:
        baseline_lines: Original config file as a list of lines (with '\\n').
        optimized_values: dict[(section, name) -> str] of tuned values only.
        output_path: Path to write the config file.
        adapter_id: Adapter identifier.
        n_trials: Number of trials used.
        default_fitness: Fitness with default parameters.
        optimized_fitness: Fitness with optimized parameters.
        tuned_param_names: Iterable of parameter names that were optimized.
        skipped_param_names: Iterable of names frozen via skip_optimize_params.
    """
    os.makedirs(os.path.dirname(output_path) or '.', exist_ok=True)

    improvement_pct = (
        (optimized_fitness - default_fitness) / max(abs(default_fitness), 0.001)
    ) * 100

    tuned_list = ', '.join(tuned_param_names) if tuned_param_names else '(none)'
    header_lines = [
        f"# Optimized alink_gs configuration for adapter: {adapter_id}\n",
        f"# Generated: {datetime.datetime.now().isoformat()}\n",
        f"# Optimization: {n_trials} trials via Bayesian optimization (optuna TPE)\n",
        f"# Fitness: default={default_fitness:.4f} -> optimized={optimized_fitness:.4f}\n",
        f"#          improvement={improvement_pct:.1f}%\n",
        f"# Tuned parameters: {tuned_list}\n",
    ]
    if skipped_param_names:
        skipped_list = ', '.join(skipped_param_names)
        header_lines.append(f"# Skipped (skip_optimize_params): {skipped_list}\n")
    header_lines.append("\n")

    # Track which tuned (section, key) pairs we've emitted so we can detect
    # keys that are missing from the baseline file (programmer error).
    remaining = dict(optimized_values)
    output = list(header_lines)
    current_section = None

    for line in baseline_lines:
        section_match = _SECTION_RE.match(line)
        if section_match:
            current_section = section_match.group(1).strip()
            output.append(line)
            continue

        kv_match = _KV_RE.match(line)
        if kv_match and current_section is not None:
            indent, key, _old_value = kv_match.groups()
            key_id = (current_section, key)
            if key_id in remaining:
                new_value = remaining.pop(key_id)
                output.append(f"{indent}{key} = {new_value}\n")
                continue

        output.append(line)

    if remaining:
        raise RuntimeError(
            f"Could not locate tuned keys in baseline file: {sorted(remaining)}"
        )

    with open(output_path, 'w') as f:
        f.writelines(output)


def main():
    parser = argparse.ArgumentParser(
        description='Optimize alink_gs parameters per adapter using Bayesian optimization')
    parser.add_argument('--adapter',
                        help='Adapter ID to optimize (or "all" for each adapter)')
    parser.add_argument('--telemetry-dir',
                        help='Directory containing telemetry JSONL files')
    parser.add_argument('--output-dir', default='config',
                        help='Directory for output config files (default: config)')
    parser.add_argument('--n-trials', type=int, default=200,
                        help='Number of optimization trials (default: 200)')
    parser.add_argument('--seed', type=int, default=42,
                        help='Random seed for reproducibility (default: 42)')
    parser.add_argument('--config',
                        help='Path to alink_gs.conf baseline config file')
    sel_group = parser.add_mutually_exclusive_group()
    sel_group.add_argument('--params', default=None,
                           help='Comma-separated parameter names OR section '
                                'shorthands (profile_selection|gate|dynamic) '
                                'to optimize. Default: all.')
    sel_group.add_argument('--exclude', default=None,
                           help='Comma-separated parameter names OR section '
                                'shorthands to exclude from optimization.')
    parser.add_argument('--list-params', action='store_true',
                        help='Print all optimizable parameters with bounds and exit.')
    args = parser.parse_args()

    if args.list_params:
        _print_registry()
        return

    # --adapter, --telemetry-dir and --config are required for an actual run
    # (but not for --list-params, hence no `required=True` on argparse).
    missing = [
        flag for flag, val in (
            ('--adapter', args.adapter),
            ('--telemetry-dir', args.telemetry_dir),
            ('--config', args.config),
        ) if not val
    ]
    if missing:
        parser.error(f"the following arguments are required: {', '.join(missing)}")

    # Load baseline config
    base_config_str = load_config_from_file(args.config)
    baseline_lines = base_config_str.splitlines(keepends=True)

    # Resolve which parameters to optimize
    skip_set = _read_skip_set(base_config_str)
    selected = _resolve_selection(args.params, args.exclude, skip_set)
    print(f"  Tuning {len(selected)} params; "
          f"skipping {len(skip_set)} via skip_optimize_params")

    # Load telemetry
    print(f"Loading telemetry from {args.telemetry_dir}...")
    ticks_df, _ = load_telemetry(args.telemetry_dir)
    print(f"  Loaded {len(ticks_df)} ticks")

    # Determine max_mcs bound from config
    temp_config = configparser.ConfigParser()
    temp_config.read_string(base_config_str)
    max_mcs_bound = 7  # default
    if temp_config.has_section('dynamic') and temp_config.has_option('dynamic', 'max_mcs'):
        try:
            max_mcs_bound = temp_config.getint('dynamic', 'max_mcs')
        except ValueError:
            pass

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
        print(f"  max_mcs bound: {max_mcs_bound}")

        try:
            optimizer = AdapterOptimizer(
                adapter_id, ticks_df, base_config_str,
                n_trials=args.n_trials,
                seed=args.seed,
                selected_params=selected,
                max_mcs_bound=max_mcs_bound,
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

        # Build the tuned-values dict for the comment-preserving writer.
        # Read values out of best_config (already adapter-constrained) rather
        # than best_trial.params so stringification stays consistent with the
        # ConfigParser representation.
        optimized_values = {
            (PARAM_SECTION[name], name): best_config.get(PARAM_SECTION[name], name)
            for name in selected
        }

        output_path = os.path.join(args.output_dir,
                                   f'alink_gs.{adapter_id}.conf')
        write_optimized_config(
            baseline_lines, optimized_values, output_path, adapter_id,
            args.n_trials, default_fitness, best_fitness,
            sorted(selected), sorted(skip_set),
        )
        print(f"\n  Config written to: {output_path}")

    print("\nDone.")


if __name__ == '__main__':
    main()
