#!/usr/bin/env python3
"""
make_reference.py — generate reference.json from training data.

See SPEC.md §3 for the output schema. Stdlib-only; no numpy/pandas required.

Usage:
  # From a CSV file (feature names from the header row):
  python3 tools/make_reference.py \\
      --input data.csv --features rsrp delay angle \\
      --buckets 10 --window-size 1000 --output reference.json

  # Use all columns in the CSV:
  python3 tools/make_reference.py --input data.csv --output reference.json

  # Self-test with synthetic data (validates without real data):
  python3 tools/make_reference.py --self-test [--output /path/to/out.json]
"""

import argparse
import csv
import json
import math
import random
import sys
import tempfile
import os


# ---------------------------------------------------------------------------
# Core algorithm
# ---------------------------------------------------------------------------

def _percentile(sorted_vals, p):
    """Return the p-th percentile (0–100) via linear interpolation."""
    n = len(sorted_vals)
    if n == 1:
        return float(sorted_vals[0])
    idx = p / 100.0 * (n - 1)
    lo = int(idx)
    hi = min(lo + 1, n - 1)
    return sorted_vals[lo] + (idx - lo) * (sorted_vals[hi] - sorted_vals[lo])


def compute_feature_ref(values, num_buckets):
    """Compute (edges, ref_ratios) for one feature.

    Uses equal-frequency binning (quantiles) so each bucket captures
    approximately the same fraction of training data.

    Args:
        values:      Iterable of float; NaN/Inf are filtered out.
        num_buckets: Number of buckets (>= 1). Output: num_buckets + 1 edges.

    Returns:
        (edges, ref_ratios) — both lists of length num_buckets+1 and
        num_buckets respectively, satisfying SPEC §3 invariants.
    """
    finite = sorted(v for v in values if not math.isnan(v) and not math.isinf(v))
    if len(finite) < num_buckets:
        raise ValueError(
            f"need at least {num_buckets} finite samples, got {len(finite)}"
        )

    # Equal-frequency edges: 0%, (100/N)%, …, 100%
    edges = [
        _percentile(finite, i * 100.0 / num_buckets)
        for i in range(num_buckets + 1)
    ]

    # Ensure strict monotonicity (handles ties / flat regions in data).
    eps = max(abs(edges[-1]) * 1e-9, 1e-9)
    for i in range(1, len(edges)):
        if edges[i] <= edges[i - 1]:
            edges[i] = edges[i - 1] + eps

    # Count samples per bucket (mirrors SPEC §2.2 semantics: edges[k] <= x < edges[k+1]).
    counts = [0] * num_buckets
    for v in finite:
        if v < edges[0]:
            counts[0] += 1          # clamp to first (SPEC §2.4)
        elif v >= edges[-1]:
            counts[-1] += 1         # clamp to last
        else:
            lo, hi = 0, num_buckets - 1
            while lo < hi:
                mid = (lo + hi) // 2
                if v >= edges[mid + 1]:
                    lo = mid + 1
                else:
                    hi = mid
            counts[lo] += 1

    total = sum(counts)
    ref_ratios = [c / total for c in counts]

    # Sanity-check: ratios must sum to 1.0 (float precision).
    ratio_sum = sum(ref_ratios)
    if abs(ratio_sum - 1.0) > 1e-9:
        raise RuntimeError(f"ref_ratios sum to {ratio_sum}, expected 1.0")

    return edges, ref_ratios


def build_profile(feature_data, feature_names, num_buckets, window_size):
    """Build a reference profile dict from {name: [float, ...]} data.

    Args:
        feature_data:  Dict mapping feature name → list of float values.
        feature_names: Ordered list of names to include (subset of feature_data).
        num_buckets:   Buckets per feature.
        window_size:   Monitoring window size written into the JSON.

    Returns:
        Dict matching SPEC §3 schema (ready for json.dump).
    """
    features = []
    for name in feature_names:
        edges, ref_ratios = compute_feature_ref(feature_data[name], num_buckets)
        features.append({"name": name, "edges": edges, "ref_ratios": ref_ratios})
    return {"version": 1, "window_size": window_size, "features": features}


# ---------------------------------------------------------------------------
# I/O helpers
# ---------------------------------------------------------------------------

def read_csv(path, feature_names):
    """Read feature columns from a CSV file. Header row is required."""
    data = {name: [] for name in feature_names}
    with open(path, newline="") as f:
        reader = csv.DictReader(f)
        for row in reader:
            for name in feature_names:
                try:
                    data[name].append(float(row[name]))
                except KeyError:
                    raise ValueError(
                        f"column '{name}' not found in {path}. "
                        f"Available: {list(row.keys())}"
                    )
                except ValueError:
                    pass  # skip non-numeric cells silently

    return data


def write_reference(profile, path):
    """Write profile dict to JSON at path."""
    with open(path, "w", encoding="utf-8") as f:
        json.dump(profile, f, indent=2, ensure_ascii=False)
        f.write("\n")


# ---------------------------------------------------------------------------
# Self-test
# ---------------------------------------------------------------------------

def self_test(output=None):
    """Generate synthetic data, build reference.json, validate invariants.

    Args:
        output: Optional path to write the reference.json; if None a temp
                file is created.

    Returns:
        Path of the written reference.json.
    """
    rng = random.Random(42)
    n = 500
    feature_data = {
        "rsrp":  [rng.gauss(-80.0, 15.0) for _ in range(n)],
        "delay": [max(0.0, rng.gauss(50.0, 20.0)) for _ in range(n)],
        "angle": [rng.uniform(0.0, 180.0) for _ in range(n)],
    }
    feature_names = list(feature_data.keys())
    num_buckets, window_size = 10, 100

    profile = build_profile(feature_data, feature_names, num_buckets, window_size)

    # Validate structure.
    assert profile["version"] == 1
    assert profile["window_size"] == window_size
    assert len(profile["features"]) == len(feature_names)
    for feat in profile["features"]:
        assert len(feat["edges"]) == num_buckets + 1, "wrong edge count"
        assert len(feat["ref_ratios"]) == num_buckets, "wrong ratio count"
        assert abs(sum(feat["ref_ratios"]) - 1.0) < 1e-9, "ratios don't sum to 1"
        for i in range(1, len(feat["edges"])):
            assert feat["edges"][i] > feat["edges"][i - 1], "edges not monotone"

    # Write to file.
    if output:
        out_path = output
        write_reference(profile, out_path)
    else:
        fd, out_path = tempfile.mkstemp(suffix=".json", prefix="driftmon_ref_")
        os.close(fd)
        write_reference(profile, out_path)

    # Re-read and confirm JSON round-trips cleanly.
    with open(out_path) as f:
        loaded = json.load(f)
    assert loaded["version"] == 1
    assert len(loaded["features"]) == len(feature_names)

    print(f"self-test PASSED — {out_path}")
    for feat in loaded["features"]:
        lo, hi = feat["edges"][0], feat["edges"][-1]
        print(f"  {feat['name']}: edges=[{lo:.2f} .. {hi:.2f}]  "
              f"ratios=[{min(feat['ref_ratios']):.3f} .. {max(feat['ref_ratios']):.3f}]")
    return out_path


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------

def main(argv=None):
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    parser.add_argument("--input", "-i", metavar="CSV",
                        help="input CSV file (header row required)")
    parser.add_argument("--features", "-f", nargs="+", metavar="NAME",
                        help="feature column names (default: all columns)")
    parser.add_argument("--buckets", "-b", type=int, default=10,
                        help="buckets per feature (default: 10)")
    parser.add_argument("--window-size", "-w", type=int, default=1000,
                        help="window_size in output JSON (default: 1000)")
    parser.add_argument("--output", "-o", metavar="JSON",
                        help="output path (default: reference.json, or temp for --self-test)")
    parser.add_argument("--self-test", action="store_true",
                        help="run built-in validation with synthetic data")
    args = parser.parse_args(argv)

    # Validate numeric arguments early — before any I/O.
    if args.buckets < 1:
        parser.error("--buckets must be >= 1")
    if args.window_size < 1:
        parser.error("--window-size must be >= 1 (SPEC §3 requires window_size > 0)")

    if args.self_test:
        self_test(output=args.output)
        return 0

    if not args.input:
        parser.error("--input is required (or use --self-test)")

    # Discover columns from header if not specified.
    if args.features is None:
        with open(args.input, newline="") as f:
            reader = csv.reader(f)
            try:
                args.features = [c.strip() for c in next(reader) if c.strip()]
            except StopIteration:
                sys.exit(f"error: {args.input} is empty")
        if not args.features:
            sys.exit(f"error: no feature columns found in header of {args.input}")

    out_path = args.output or "reference.json"

    print(f"Loading: {args.input}")
    print(f"Features: {args.features}")
    feature_data = read_csv(args.input, args.features)

    profile = build_profile(feature_data, args.features, args.buckets, args.window_size)
    write_reference(profile, out_path)

    print(f"Written: {out_path}")
    for feat in profile["features"]:
        lo, hi = feat["edges"][0], feat["edges"][-1]
        print(f"  {feat['name']}: edges=[{lo:.4g} .. {hi:.4g}]  "
              f"ratios=[{min(feat['ref_ratios']):.3f} .. {max(feat['ref_ratios']):.3f}]")
    return 0


if __name__ == "__main__":
    sys.exit(main())
