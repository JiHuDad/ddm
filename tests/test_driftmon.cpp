// test_driftmon.cpp — unit tests for the driftmon library.
//
// Phase 0: smoke tests (null-safety, contract invariants).
// Phase 1: reference loading, histogram accumulation, PSI computation.
#include "driftmon.h"
#include "test_framework.h"

#include <cmath>
#include <cstdio>
#include <fstream>
#include <limits>
#include <string>

// Write a JSON string to a temp file and return the path.
static std::string write_tmp(const char* name, const char* json) {
    std::string path = std::string("/tmp/driftmon_") + name + ".json";
    std::ofstream f(path);
    f << json;
    return path;
}

// 1-feature, 3-bucket reference.
// edges=[0,1,2,3], ref_ratios=[0.5, 0.3, 0.2], window_size=10.
static const char* VALID_1F =
    "{"
    "  \"version\": 1,"
    "  \"window_size\": 10,"
    "  \"features\": [{"
    "    \"name\": \"x\","
    "    \"edges\": [0.0, 1.0, 2.0, 3.0],"
    "    \"ref_ratios\": [0.5, 0.3, 0.2]"
    "  }]"
    "}";

// 2-feature reference for testing multi-feature paths.
static const char* VALID_2F =
    "{"
    "  \"version\": 1,"
    "  \"window_size\": 10,"
    "  \"features\": ["
    "    {\"name\": \"a\", \"edges\": [0.0, 1.0, 2.0, 3.0], \"ref_ratios\": [0.5, 0.3, 0.2]},"
    "    {\"name\": \"b\", \"edges\": [10.0, 20.0, 30.0], \"ref_ratios\": [0.6, 0.4]}"
    "  ]"
    "}";

// ─── Phase 0 smoke tests ────────────────────────────────────────────────────

TEST(destroy_null_is_safe) {
    driftmon_destroy(nullptr);
    CHECK(true);
}

TEST(create_with_null_path_returns_null) {
    CHECK(driftmon_create(nullptr) == nullptr);
}

TEST(num_features_of_null_is_zero) {
    CHECK(driftmon_num_features(nullptr) == 0);
}

TEST(compute_on_null_sets_max_psi_zero) {
    double max_psi = -1.0;
    driftmon_compute(nullptr, nullptr, &max_psi);
    CHECK_NEAR(max_psi, 0.0, 1e-12);
}

// ─── Phase 1: reference loading ─────────────────────────────────────────────

TEST(reference_valid_load) {
    std::string path = write_tmp("valid_1f", VALID_1F);
    driftmon_t* m = driftmon_create(path.c_str());
    CHECK(m != nullptr);
    CHECK(driftmon_num_features(m) == 1);
    driftmon_destroy(m);
}

TEST(reference_valid_load_2f) {
    std::string path = write_tmp("valid_2f", VALID_2F);
    driftmon_t* m = driftmon_create(path.c_str());
    CHECK(m != nullptr);
    CHECK(driftmon_num_features(m) == 2);
    driftmon_destroy(m);
}

TEST(reference_missing_file_returns_null) {
    CHECK(driftmon_create("/tmp/driftmon_does_not_exist_xyzzy.json") == nullptr);
}

TEST(reference_bad_ratios_sum_rejected) {
    // ref_ratios sum to 0.9 (not ~1.0) → validation must reject.
    std::string path = write_tmp("bad_sum",
        "{\"version\":1,\"window_size\":10,\"features\":["
        "{\"name\":\"x\",\"edges\":[0.0,1.0,2.0],\"ref_ratios\":[0.5,0.4]}"
        "]}");
    // sum = 0.9, tolerance is 0.01 → should fail
    CHECK(driftmon_create(path.c_str()) == nullptr);
}

TEST(reference_edges_mismatch_rejected) {
    // edges has 3 elements (2 buckets) but ref_ratios has 3 → mismatch.
    std::string path = write_tmp("edge_mismatch",
        "{\"version\":1,\"window_size\":10,\"features\":["
        "{\"name\":\"x\",\"edges\":[0.0,1.0,2.0],\"ref_ratios\":[0.33,0.33,0.34]}"
        "]}");
    CHECK(driftmon_create(path.c_str()) == nullptr);
}

TEST(reference_empty_features_rejected) {
    std::string path = write_tmp("empty_features",
        "{\"version\":1,\"window_size\":10,\"features\":[]}");
    CHECK(driftmon_create(path.c_str()) == nullptr);
}

// ─── Phase 1: window readiness ───────────────────────────────────────────────

TEST(window_not_ready_before_enough_observations) {
    std::string path = write_tmp("ready_1f", VALID_1F);  // window_size=10
    driftmon_t* m = driftmon_create(path.c_str());
    CHECK(m != nullptr);
    double feats[1] = {0.5};
    for (int i = 0; i < 9; ++i) driftmon_observe(m, feats, 1);
    CHECK(driftmon_ready(m) == 0);
    driftmon_destroy(m);
}

TEST(window_ready_after_window_size_observations) {
    std::string path = write_tmp("ready_1f2", VALID_1F);  // window_size=10
    driftmon_t* m = driftmon_create(path.c_str());
    CHECK(m != nullptr);
    double feats[1] = {0.5};
    for (int i = 0; i < 10; ++i) driftmon_observe(m, feats, 1);
    CHECK(driftmon_ready(m) != 0);
    driftmon_destroy(m);
}

// ─── Phase 1: PSI computation ────────────────────────────────────────────────

// Observe exactly the reference distribution → PSI must be ~0.
// ref_ratios=[0.5, 0.3, 0.2], window_size=10.
// Observe: 5 in [0,1), 3 in [1,2), 2 in [2,3) → actual ratios = ref ratios.
TEST(psi_zero_for_identical_distribution) {
    std::string path = write_tmp("psi_zero", VALID_1F);
    driftmon_t* m = driftmon_create(path.c_str());
    CHECK(m != nullptr);

    // 5 in bucket 0: [0,1)
    double v;
    for (int i = 0; i < 5; ++i) { v = 0.1 * (i + 1); driftmon_observe(m, &v, 1); }
    // 3 in bucket 1: [1,2)
    for (int i = 0; i < 3; ++i) { v = 1.1 + 0.1 * i; driftmon_observe(m, &v, 1); }
    // 2 in bucket 2: [2,3)
    for (int i = 0; i < 2; ++i) { v = 2.1 + 0.1 * i; driftmon_observe(m, &v, 1); }

    CHECK(driftmon_ready(m) != 0);
    double psi, max_psi;
    driftmon_compute(m, &psi, &max_psi);
    CHECK_NEAR(psi, 0.0, 1e-9);
    CHECK_NEAR(max_psi, 0.0, 1e-9);
    driftmon_destroy(m);
}

// All observations in bucket 0 → PSI well above 0.2 (significant drift).
// Computed: ~4.27 for ref=[0.5,0.3,0.2] with epsilon floor = 1e-4.
TEST(psi_high_for_shifted_distribution) {
    std::string path = write_tmp("psi_high", VALID_1F);
    driftmon_t* m = driftmon_create(path.c_str());
    CHECK(m != nullptr);

    double v = 0.5;  // always in bucket 0
    for (int i = 0; i < 10; ++i) driftmon_observe(m, &v, 1);

    double max_psi;
    driftmon_compute(m, nullptr, &max_psi);
    CHECK(max_psi > 0.2);
    driftmon_destroy(m);
}

// ─── Phase 1: reset ──────────────────────────────────────────────────────────

TEST(reset_clears_window) {
    std::string path = write_tmp("reset", VALID_1F);
    driftmon_t* m = driftmon_create(path.c_str());
    CHECK(m != nullptr);

    double v = 0.5;
    for (int i = 0; i < 10; ++i) driftmon_observe(m, &v, 1);
    CHECK(driftmon_ready(m) != 0);

    driftmon_reset(m);
    CHECK(driftmon_ready(m) == 0);

    // After reset, re-observing the reference distribution gives PSI ~0 again.
    for (int i = 0; i < 5; ++i) { v = 0.1 * (i + 1); driftmon_observe(m, &v, 1); }
    for (int i = 0; i < 3; ++i) { v = 1.1 + 0.1 * i; driftmon_observe(m, &v, 1); }
    for (int i = 0; i < 2; ++i) { v = 2.1 + 0.1 * i; driftmon_observe(m, &v, 1); }

    double psi, max_psi;
    driftmon_compute(m, &psi, &max_psi);
    CHECK_NEAR(psi, 0.0, 1e-9);
    driftmon_destroy(m);
}

// ─── Phase 1: edge cases ─────────────────────────────────────────────────────

// Values outside edge range clamp to first/last bucket — no crash.
TEST(observe_clamp_below_and_above) {
    std::string path = write_tmp("clamp", VALID_1F);
    driftmon_t* m = driftmon_create(path.c_str());
    CHECK(m != nullptr);

    // 5 below min edge (clamp to bucket 0)
    double v = -999.0;
    for (int i = 0; i < 5; ++i) driftmon_observe(m, &v, 1);
    // 5 above max edge (clamp to last bucket = bucket 2)
    v = 999.0;
    for (int i = 0; i < 5; ++i) driftmon_observe(m, &v, 1);

    CHECK(driftmon_ready(m) != 0);
    double max_psi;
    driftmon_compute(m, nullptr, &max_psi);
    // Actual = [0.5, 0.0, 0.5] vs ref = [0.5, 0.3, 0.2] → PSI > 0.
    CHECK(max_psi > 0.0);
    driftmon_destroy(m);
}

// NaN and Inf values must not crash, and observations still count toward ready().
TEST(observe_nan_and_inf_no_crash) {
    std::string path = write_tmp("nan_inf", VALID_1F);
    driftmon_t* m = driftmon_create(path.c_str());
    CHECK(m != nullptr);

    double nan_val = std::numeric_limits<double>::quiet_NaN();
    double inf_val = std::numeric_limits<double>::infinity();
    for (int i = 0; i < 5; ++i) driftmon_observe(m, &nan_val, 1);
    for (int i = 0; i < 5; ++i) driftmon_observe(m, &inf_val, 1);

    // observations = 10, so ready() must be true even though no bucket was hit.
    CHECK(driftmon_ready(m) != 0);

    // Compute must not crash with zero bucket counts.
    double max_psi;
    driftmon_compute(m, nullptr, &max_psi);
    CHECK(max_psi >= 0.0);
    driftmon_destroy(m);
}

// Calling observe with wrong feature count must not crash.
TEST(observe_wrong_n_is_noop) {
    std::string path = write_tmp("wrong_n", VALID_1F);
    driftmon_t* m = driftmon_create(path.c_str());
    CHECK(m != nullptr);

    double feats[2] = {0.5, 1.5};
    driftmon_observe(m, feats, 2);  // wrong: this is a 1-feature monitor
    // observations should NOT have been incremented.
    CHECK(driftmon_ready(m) == 0);
    driftmon_destroy(m);
}

int main() { return RUN_ALL(); }
