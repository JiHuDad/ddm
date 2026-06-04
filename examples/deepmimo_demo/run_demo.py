#!/usr/bin/env python3
"""
run_demo.py — end-to-end DeepMIMO drift demonstration.

Workflow:
  1. gen_zones.py   → zone_a_train.csv, zone_a_test.csv, zone_e_test.csv
  2. make_reference.py  → reference.json  (from zone_a_train.csv)
  3. demo_driver    → Zone A test   (expect STABLE,      max_psi < 0.1)
  4. demo_driver    → Zone E test   (expect SIGNIFICANT, max_psi > 0.2)

Prerequisites:
  - Build driftmon with examples enabled:
      cmake -S . -B build -DDRIFTMON_ENABLE_EXAMPLES=ON
      cmake --build build
  - Run from the repo root or pass --build-dir.

Exit codes:
  0  both assertions pass (Zone A stable, Zone E significant)
  1  an assertion failed or a subprocess error occurred
"""
import argparse
import os
import subprocess
import sys


def run(cmd, *, check=True, **kw):
    print(f"  $ {' '.join(str(c) for c in cmd)}", flush=True)
    return subprocess.run(cmd, check=check, **kw)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--build-dir", default="build",
                        help="CMake build directory (default: build)")
    parser.add_argument("--work-dir", default="/tmp/deepmimo_demo",
                        help="working directory for generated CSV/JSON files")
    parser.add_argument("--n-train", type=int, default=1000)
    parser.add_argument("--n-test",  type=int, default=500)
    parser.add_argument("--seed",    type=int, default=42)
    parser.add_argument("--buckets", type=int, default=10)
    args = parser.parse_args(argv)

    repo_root  = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    build_dir  = os.path.join(repo_root, args.build_dir)
    work_dir   = args.work_dir
    os.makedirs(work_dir, exist_ok=True)

    driver = os.path.join(build_dir, "demo_driver")
    if not os.path.isfile(driver):
        sys.exit(
            f"error: demo_driver not found at {driver}\n"
            f"Build first:  cmake -S {repo_root} -B {build_dir} "
            f"-DDRIFTMON_ENABLE_EXAMPLES=ON && cmake --build {build_dir}"
        )

    gen_zones     = os.path.join(repo_root, "examples", "deepmimo_demo", "gen_zones.py")
    make_ref      = os.path.join(repo_root, "tools", "make_reference.py")
    ref_json      = os.path.join(work_dir, "reference.json")
    zone_a_train  = os.path.join(work_dir, "zone_a_train.csv")
    zone_a_test   = os.path.join(work_dir, "zone_a_test.csv")
    zone_e_test   = os.path.join(work_dir, "zone_e_test.csv")

    print("\n=== Step 1: generate synthetic zone data ===")
    run([sys.executable, gen_zones,
         "--out-dir", work_dir,
         "--n-train", str(args.n_train),
         "--n-test",  str(args.n_test),
         "--seed",    str(args.seed)])

    print("\n=== Step 2: build reference profile from Zone A training data ===")
    run([sys.executable, make_ref,
         "--input",       zone_a_train,
         "--buckets",     str(args.buckets),
         "--window-size", str(args.n_test),
         "--output",      ref_json])

    print("\n=== Step 3: Zone A test (same distribution — expect STABLE) ===")
    result_a = run([driver, ref_json, zone_a_test],
                   capture_output=True, text=True, check=False)
    print(result_a.stdout, end="")
    if result_a.returncode != 0:
        print(result_a.stderr, end="", file=sys.stderr)
        sys.exit(1)

    print("\n=== Step 4: Zone E test (NLOS drift — expect SIGNIFICANT) ===")
    result_e = run([driver, ref_json, zone_e_test],
                   capture_output=True, text=True, check=False)
    print(result_e.stdout, end="")
    if result_e.returncode != 0:
        print(result_e.stderr, end="", file=sys.stderr)
        sys.exit(1)

    # ---- assertions ----
    def parse_max_psi(output):
        """Extract max_psi value from first WINDOW line."""
        for line in output.splitlines():
            if line.startswith("WINDOW"):
                for tok in line.split():
                    if tok.startswith("max_psi="):
                        return float(tok.split("=")[1])
        return None

    psi_a = parse_max_psi(result_a.stdout)
    psi_e = parse_max_psi(result_e.stdout)

    print("\n=== Results ===")
    passed = True

    if psi_a is None:
        print("FAIL  Zone A: no WINDOW output (0 completed windows?)")
        passed = False
    elif psi_a >= 0.1:
        print(f"FAIL  Zone A: max_psi={psi_a:.4f} >= 0.1 (expected STABLE)")
        passed = False
    else:
        print(f"PASS  Zone A: max_psi={psi_a:.4f} < 0.1  (STABLE ✓)")

    if psi_e is None:
        print("FAIL  Zone E: no WINDOW output (0 completed windows?)")
        passed = False
    elif psi_e <= 0.2:
        print(f"FAIL  Zone E: max_psi={psi_e:.4f} <= 0.2 (expected SIGNIFICANT)")
        passed = False
    else:
        print(f"PASS  Zone E: max_psi={psi_e:.4f} > 0.2  (SIGNIFICANT ✓)")

    if passed:
        print("\nAll assertions PASSED.")
        return 0
    else:
        print("\nOne or more assertions FAILED.")
        return 1


if __name__ == "__main__":
    sys.exit(main())
