#!/usr/bin/env python3
"""
run_demo.py — driftmon-cpp cross-process end-to-end drift demo.

Proves the WHOLE pipeline on realistic, drifting data, across REAL processes:

  1. gen_zones.py  → Zone A (LOS, reference) and Zone E (NLOS, drifted) CSVs.
  2. make_bundle.py(Zone A train) → bundle.json (quantile bins + ref_hist).
  3. start driftmon_worker  (separate process; creates the shm arena).
  4. tap_driver replays a CSV through the real tap (separate process) → shm.
  5. read the worker's Prometheus export and assert the severity.

  Phase A: replay Zone A test  → expect severity 0 (STABLE).
  Phase E: replay Zone E test  → expect severity 2 (SIGNIFICANT alarm).

Each phase uses a fresh worker (clean shm), so the phases are independent.
Exit 0 on success, 1 on failure.
"""
import argparse
import os
import re
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
GEN_ZONES = os.path.join(HERE, "..", "..", "..", "examples", "deepmimo_demo", "gen_zones.py")
MAKE_BUNDLE = os.path.join(HERE, "make_bundle.py")
MODEL_ID = "deepmimo"
SHM_PATH = f"/dev/shm/driftmon.{MODEL_ID}"


def run(cmd, **kw):
    print("  $", " ".join(str(c) for c in cmd))
    return subprocess.run(cmd, check=True, **kw)


def wait_for(predicate, timeout, interval=0.05):
    deadline = time.time() + timeout
    while time.time() < deadline:
        if predicate():
            return True
        time.sleep(interval)
    return False


def read_metric(prom_path, metric):
    """Return int value of driftmon_<metric>{model=...}, or None if absent."""
    if not os.path.exists(prom_path):
        return None
    pat = re.compile(r'driftmon_%s\{model="%s"\}\s+(\d+)' % (metric, re.escape(MODEL_ID)))
    with open(prom_path) as f:
        m = pat.search(f.read())
    return int(m.group(1)) if m else None


def read_severity(prom_path):
    """Severity once a real verdict exists (generation >= 1); None before that.

    The worker also heartbeats records before any window closes (tap-liveness
    export), so a severity line alone is not proof a verdict happened.
    """
    gen = read_metric(prom_path, "generation")
    if gen is None or gen < 1:
        return None
    return read_metric(prom_path, "drift_severity")


def run_phase(name, worker_bin, driver_bin, bundle, csv, prom, expect, loops):
    print(f"\n=== Phase {name}: replay {os.path.basename(csv)} (expect severity {expect}) ===")
    if os.path.exists(SHM_PATH):
        os.remove(SHM_PATH)
    if os.path.exists(prom):
        os.remove(prom)

    worker = subprocess.Popen(
        [worker_bin, "--bundle", bundle, "--export", "prometheus",
         "--export-target", prom, "--period-ms", "50"],
        stderr=subprocess.PIPE, text=True)
    try:
        # Worker creates the shm arena on startup.
        if not wait_for(lambda: os.path.exists(SHM_PATH), timeout=5.0):
            print("  FAIL: worker never created the shm arena")
            return False

        # Replay the CSV through the real tap (separate process).
        run([driver_bin, "--bundle", bundle, "--csv", csv, "--loops", str(loops)])

        # Wait for the worker to close a window and export a verdict.
        if not wait_for(lambda: read_severity(prom) is not None, timeout=10.0):
            print("  FAIL: worker produced no verdict (no export)")
            return False
        sev = read_severity(prom)
        ok = (sev == expect)
        print(f"  exported severity = {sev}  ({'PASS' if ok else 'FAIL'}, expected {expect})")
        return ok
    finally:
        worker.terminate()
        try:
            worker.wait(timeout=5)
        except subprocess.TimeoutExpired:
            worker.kill()
        if os.path.exists(SHM_PATH):
            os.remove(SHM_PATH)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--build-dir", required=True,
                    help="CMake build dir containing driftmon_worker and tap_driver")
    ap.add_argument("--work-dir", default=None, help="scratch dir for CSVs/bundle (default: build-dir)")
    args = ap.parse_args(argv)

    worker_bin = os.path.join(args.build_dir, "driftmon_worker")
    driver_bin = os.path.join(args.build_dir, "tap_driver")
    for b in (worker_bin, driver_bin):
        if not os.path.exists(b):
            sys.exit(f"missing binary: {b} (build with -DDRIFTMON_ENABLE_CPP=ON -DDRIFTMON_ENABLE_EXAMPLES=ON)")

    work = args.work_dir or args.build_dir
    os.makedirs(work, exist_ok=True)

    # 1. Generate Zone A / Zone E data.
    print("=== Step 1: generate DeepMIMO-like zones ===")
    run([sys.executable, GEN_ZONES, "--out-dir", work, "--n-train", "2000", "--n-test", "1000"])

    # 2. Build the reference bundle from Zone A training data.
    print("\n=== Step 2: make reference bundle from Zone A ===")
    bundle = os.path.join(work, "bundle.json")
    run([sys.executable, MAKE_BUNDLE, "--input", os.path.join(work, "zone_a_train.csv"),
         "--model-id", MODEL_ID, "--buckets", "10",
         "--min-samples", "400", "--max-seconds", "3600",
         "--tests", "psi", "ks", "--output", bundle])

    # 3+4+5. Two phases, fresh worker each.
    a_ok = run_phase("A (stable)", worker_bin, driver_bin, bundle,
                     os.path.join(work, "zone_a_test.csv"),
                     os.path.join(work, "a.prom"), expect=0, loops=2)
    e_ok = run_phase("E (drift)", worker_bin, driver_bin, bundle,
                     os.path.join(work, "zone_e_test.csv"),
                     os.path.join(work, "e.prom"), expect=2, loops=2)

    print("\n==============================")
    print(f"Phase A (STABLE)      : {'PASS' if a_ok else 'FAIL'}")
    print(f"Phase E (SIGNIFICANT) : {'PASS' if e_ok else 'FAIL'}")
    print("==============================")
    return 0 if (a_ok and e_ok) else 1


if __name__ == "__main__":
    sys.exit(main())
