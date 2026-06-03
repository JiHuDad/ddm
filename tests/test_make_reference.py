#!/usr/bin/env python3
"""Unit tests for tools/make_reference.py.

Run directly:  python3 tests/test_make_reference.py
Or via pytest: pytest tests/test_make_reference.py
"""
import json
import math
import os
import random
import sys
import tempfile
import unittest

# Allow importing the tool without installing it.
sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "tools"))
from make_reference import compute_feature_ref, build_profile, self_test


class TestComputeFeatureRef(unittest.TestCase):

    def test_basic_output_shapes(self):
        rng = random.Random(0)
        vals = [rng.gauss(0, 1) for _ in range(500)]
        edges, ratios = compute_feature_ref(vals, 10)
        self.assertEqual(len(edges), 11)
        self.assertEqual(len(ratios), 10)

    def test_ratios_sum_to_one(self):
        rng = random.Random(1)
        for nb in (3, 5, 10):
            vals = [rng.uniform(-100, 100) for _ in range(300)]
            _, ratios = compute_feature_ref(vals, nb)
            self.assertAlmostEqual(sum(ratios), 1.0, places=9,
                                   msg=f"failed for {nb} buckets")

    def test_edges_strictly_monotone(self):
        rng = random.Random(2)
        vals = [rng.gauss(0, 1) for _ in range(200)]
        edges, _ = compute_feature_ref(vals, 10)
        for i in range(1, len(edges)):
            self.assertGreater(edges[i], edges[i - 1],
                               f"edges[{i}] <= edges[{i-1}]")

    def test_equal_frequency_roughly_uniform(self):
        # With large n, equal-freq bins give ~1/nb each.
        rng = random.Random(3)
        vals = [rng.uniform(0, 1) for _ in range(10000)]
        _, ratios = compute_feature_ref(vals, 10)
        for r in ratios:
            self.assertAlmostEqual(r, 0.1, delta=0.015)

    def test_all_identical_values_no_crash(self):
        # Degenerate: monotonicity fix must kick in.
        vals = [5.0] * 100
        edges, ratios = compute_feature_ref(vals, 10)
        self.assertAlmostEqual(sum(ratios), 1.0, places=9)
        for i in range(1, len(edges)):
            self.assertGreater(edges[i], edges[i - 1])

    def test_negative_values(self):
        # Typical RSRP-like (dBm).
        rng = random.Random(4)
        vals = [rng.gauss(-80, 15) for _ in range(300)]
        edges, ratios = compute_feature_ref(vals, 10)
        self.assertAlmostEqual(sum(ratios), 1.0, places=9)
        # Most edges should be negative for negative data.
        self.assertTrue(sum(1 for e in edges if e < 0) > 5)

    def test_nan_and_inf_filtered(self):
        rng = random.Random(5)
        vals = [rng.uniform(0, 1) for _ in range(200)]
        vals += [float("nan"), float("inf"), float("-inf")]
        edges, ratios = compute_feature_ref(vals, 5)
        self.assertAlmostEqual(sum(ratios), 1.0, places=9)

    def test_too_few_samples_raises(self):
        with self.assertRaises(ValueError):
            compute_feature_ref([1.0, 2.0], num_buckets=10)


class TestBuildProfile(unittest.TestCase):

    def _data(self, seed=0, n=300):
        rng = random.Random(seed)
        return {
            "a": [rng.gauss(0, 1) for _ in range(n)],
            "b": [rng.uniform(10, 20) for _ in range(n)],
        }

    def test_spec_schema_invariants(self):
        """All SPEC §3 invariants must hold on the output dict."""
        data = self._data()
        profile = build_profile(data, ["a", "b"], num_buckets=10, window_size=500)

        self.assertEqual(profile["version"], 1)
        self.assertEqual(profile["window_size"], 500)
        self.assertEqual(len(profile["features"]), 2)

        for feat in profile["features"]:
            nb = 10
            self.assertEqual(len(feat["edges"]), nb + 1)
            self.assertEqual(len(feat["ref_ratios"]), nb)
            # Ratios sum to ~1.
            self.assertAlmostEqual(sum(feat["ref_ratios"]), 1.0, places=9)
            # All ratios non-negative.
            for r in feat["ref_ratios"]:
                self.assertGreaterEqual(r, 0.0)
            # Edges strictly monotone.
            for i in range(1, nb + 1):
                self.assertGreater(feat["edges"][i], feat["edges"][i - 1])

    def test_feature_order_preserved(self):
        data = self._data(1)
        profile = build_profile(data, ["b", "a"], 5, 100)
        self.assertEqual(profile["features"][0]["name"], "b")
        self.assertEqual(profile["features"][1]["name"], "a")

    def test_json_roundtrip(self):
        """Profile must survive json.dump → json.load unchanged."""
        data = self._data(2)
        profile = build_profile(data, ["a", "b"], 10, 1000)
        with tempfile.NamedTemporaryFile(mode="w", suffix=".json",
                                        delete=False) as tf:
            json.dump(profile, tf)
            path = tf.name
        with open(path) as f:
            loaded = json.load(f)
        self.assertEqual(loaded["version"], profile["version"])
        self.assertEqual(len(loaded["features"]), len(profile["features"]))
        for orig, reloaded in zip(profile["features"], loaded["features"]):
            self.assertEqual(orig["name"], reloaded["name"])
            self.assertAlmostEqual(sum(reloaded["ref_ratios"]), 1.0, places=9)


class TestSelfTest(unittest.TestCase):

    def test_self_test_passes(self):
        # self_test() must not raise.
        path = self_test()
        self.assertTrue(os.path.isfile(path))

    def test_self_test_writes_to_given_path(self):
        with tempfile.NamedTemporaryFile(suffix=".json", delete=False) as tf:
            path = tf.name
        result = self_test(output=path)
        self.assertEqual(result, path)
        with open(path) as f:
            loaded = json.load(f)
        self.assertEqual(loaded["version"], 1)
        self.assertEqual(len(loaded["features"]), 3)   # rsrp, delay, angle


if __name__ == "__main__":
    unittest.main(verbosity=2)
