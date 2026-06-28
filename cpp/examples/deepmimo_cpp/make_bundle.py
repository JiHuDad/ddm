#!/usr/bin/env python3
"""
make_bundle.py — generate a driftmon-cpp reference bundle (SPEC-cpp §5.4) from
training CSV data.

Mirrors tools/make_reference.py's equal-frequency (quantile) binning, but emits
the richer cpp bundle schema: per-feature {bin_edges, ref_hist (counts),
psi_threshold}, plus window{min_samples,max_seconds} and tests[]. Stdlib only.

Usage:
  python3 make_bundle.py --input zone_a_train.csv --model-id beam_v3 \\
      --buckets 10 --min-samples 1000 --max-seconds 60 \\
      --tests psi ks --output bundle.json
"""
import argparse
import csv
import json
import math
import sys


def _percentile(sorted_vals, p):
    n = len(sorted_vals)
    if n == 1:
        return float(sorted_vals[0])
    idx = p / 100.0 * (n - 1)
    lo = int(idx)
    hi = min(lo + 1, n - 1)
    return sorted_vals[lo] + (idx - lo) * (sorted_vals[hi] - sorted_vals[lo])


def compute_feature(values, num_buckets):
    """Return (bin_edges, ref_hist counts) via equal-frequency binning."""
    finite = sorted(v for v in values if not math.isnan(v) and not math.isinf(v))
    if len(finite) < num_buckets:
        raise ValueError(f"need >= {num_buckets} finite samples, got {len(finite)}")

    edges = [_percentile(finite, i * 100.0 / num_buckets) for i in range(num_buckets + 1)]
    eps = max(abs(edges[-1]) * 1e-9, 1e-9)
    for i in range(1, len(edges)):
        if edges[i] <= edges[i - 1]:
            edges[i] = edges[i - 1] + eps

    counts = [0] * num_buckets
    for v in finite:
        if v < edges[0]:
            counts[0] += 1
        elif v >= edges[-1]:
            counts[-1] += 1
        else:
            lo, hi = 0, num_buckets - 1
            while lo < hi:
                mid = (lo + hi) // 2
                if v >= edges[mid + 1]:
                    lo = mid + 1
                else:
                    hi = mid
            counts[lo] += 1
    return edges, counts


def read_csv(path, feature_names):
    data = {name: [] for name in feature_names}
    with open(path, newline="") as f:
        reader = csv.DictReader(f)
        for row in reader:
            for name in feature_names:
                try:
                    data[name].append(float(row[name]))
                except (KeyError, ValueError):
                    pass
    return data


def build_bundle(feature_data, feature_names, num_buckets, model_id,
                 min_samples, max_seconds, tests):
    features = []
    for idx, name in enumerate(feature_names):
        edges, counts = compute_feature(feature_data[name], num_buckets)
        features.append({
            "name": name,
            "dtype": "float32",
            "index": idx,
            "bin_edges": edges,
            "ref_hist": counts,
            "psi_threshold": 0.2,
        })
    return {
        "schema_version": "1.0",
        "model_id": model_id,
        "window": {"min_samples": min_samples, "max_seconds": max_seconds},
        "tests": tests,
        "features": features,
    }


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--input", "-i", required=True, metavar="CSV")
    p.add_argument("--features", "-f", nargs="+", metavar="NAME",
                   help="feature columns (default: all header columns)")
    p.add_argument("--model-id", required=True)
    p.add_argument("--buckets", "-b", type=int, default=10)
    p.add_argument("--min-samples", type=int, default=1000)
    p.add_argument("--max-seconds", type=int, default=60)
    p.add_argument("--tests", nargs="+", default=["psi", "ks"])
    p.add_argument("--output", "-o", required=True, metavar="JSON")
    args = p.parse_args(argv)

    if args.features is None:
        with open(args.input, newline="") as f:
            args.features = [c.strip() for c in next(csv.reader(f)) if c.strip()]
        if not args.features:
            sys.exit(f"error: no columns in header of {args.input}")

    data = read_csv(args.input, args.features)
    bundle = build_bundle(data, args.features, args.buckets, args.model_id,
                          args.min_samples, args.max_seconds, args.tests)
    with open(args.output, "w", encoding="utf-8") as f:
        json.dump(bundle, f, indent=2, ensure_ascii=False)
        f.write("\n")

    print(f"Written bundle: {args.output}  model_id={args.model_id}")
    for feat in bundle["features"]:
        lo, hi = feat["bin_edges"][0], feat["bin_edges"][-1]
        print(f"  {feat['name']}: edges=[{lo:.4g} .. {hi:.4g}]  "
              f"ref_hist sum={sum(feat['ref_hist'])}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
