#!/usr/bin/env python3
"""
gen_zones.py — synthetic DeepMIMO-like channel statistics.

Produces three CSV files that represent two radio propagation environments:

  Zone A  (LOS urban, reference)
    rsrp              ~ N(-70, 8)   dBm   — strong signal
    main_path_delay   ~ N(20, 5)    ns    — short LOS delay
    strongest_angle   ~ N(45, 10)   deg   — narrow beam

  Zone E  (NLOS, drifted)
    rsrp              ~ N(-95, 12)  dBm   — weakened signal
    main_path_delay   ~ N(85, 25)   ns    — long NLOS delay
    strongest_angle   ~ N(120, 30)  deg   — different direction

The distributions are chosen so that PSI(Zone E | Zone A reference) >> 0.2
(significant drift) while PSI(Zone A test | Zone A reference) << 0.1 (stable).
"""
import argparse
import csv
import os
import random
import sys

FEATURES = ["rsrp", "main_path_delay", "strongest_angle"]

ZONE_A = {
    "rsrp":            ("gauss", -70.0,  8.0),
    "main_path_delay": ("gauss",  20.0,  5.0),
    "strongest_angle": ("gauss",  45.0, 10.0),
}
ZONE_E = {
    "rsrp":            ("gauss", -95.0, 12.0),
    "main_path_delay": ("gauss",  85.0, 25.0),
    "strongest_angle": ("gauss", 120.0, 30.0),
}


def _sample(rng, spec):
    kind, *params = spec
    if kind == "gauss":
        return rng.gauss(*params)
    raise ValueError(f"unknown distribution: {kind}")


def write_csv(path, zone_params, n, seed):
    rng = random.Random(seed)
    os.makedirs(os.path.dirname(path) or ".", exist_ok=True)
    with open(path, "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(FEATURES)
        for _ in range(n):
            w.writerow([f"{_sample(rng, zone_params[feat]):.4f}" for feat in FEATURES])


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--out-dir", default=".",
                        help="directory to write CSV files (default: .)")
    parser.add_argument("--n-train", type=int, default=1000,
                        help="Zone A training samples for reference.json (default: 1000)")
    parser.add_argument("--n-test", type=int, default=500,
                        help="test samples per zone (default: 500)")
    parser.add_argument("--seed", type=int, default=42)
    args = parser.parse_args(argv)

    d = args.out_dir
    files = {
        "zone_a_train": os.path.join(d, "zone_a_train.csv"),
        "zone_a_test":  os.path.join(d, "zone_a_test.csv"),
        "zone_e_test":  os.path.join(d, "zone_e_test.csv"),
    }

    write_csv(files["zone_a_train"], ZONE_A, args.n_train, args.seed)
    write_csv(files["zone_a_test"],  ZONE_A, args.n_test,  args.seed + 1)
    write_csv(files["zone_e_test"],  ZONE_E, args.n_test,  args.seed + 2)

    print(f"Zone A train ({args.n_train} samples) → {files['zone_a_train']}")
    print(f"Zone A test  ({args.n_test}  samples) → {files['zone_a_test']}")
    print(f"Zone E test  ({args.n_test}  samples) → {files['zone_e_test']}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
