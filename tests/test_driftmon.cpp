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

// ─── Phase 2: zero-observation feature must not manufacture drift ────────────

// Regression: a feature whose observations are all NaN (zero valid counts)
// must report PSI=0, not a spurious high value from the epsilon floor.
TEST(zero_observation_feature_psi_is_zero) {
    std::string path = write_tmp("zero_obs", VALID_2F);  // 2 features
    driftmon_t* m = driftmon_create(path.c_str());
    CHECK(m != nullptr);

    double nan_val = std::numeric_limits<double>::quiet_NaN();
    // feature a: real data matching ref [0.5,0.3,0.2]; feature b: all NaN.
    for (int i = 0; i < 5; ++i) { double f[2] = {0.5, nan_val}; driftmon_observe(m, f, 2); }
    for (int i = 0; i < 3; ++i) { double f[2] = {1.5, nan_val}; driftmon_observe(m, f, 2); }
    for (int i = 0; i < 2; ++i) { double f[2] = {2.5, nan_val}; driftmon_observe(m, f, 2); }

    double psi[2], max_psi;
    driftmon_compute(m, psi, &max_psi);
    CHECK_NEAR(psi[0], 0.0, 1e-9);   // matches ref → ~0
    CHECK_NEAR(psi[1], 0.0, 1e-12);  // no valid data → exactly 0 (the fix)
    CHECK_NEAR(max_psi, 0.0, 1e-9);  // not polluted by feature b
    driftmon_destroy(m);
}

// ─── Phase 2: threshold classification ───────────────────────────────────────

TEST(classify_thresholds) {
    CHECK(driftmon_classify(0.0)  == DRIFTMON_STABLE);
    CHECK(driftmon_classify(0.05) == DRIFTMON_STABLE);
    CHECK(driftmon_classify(0.1)  == DRIFTMON_WARNING);      // boundary inclusive
    CHECK(driftmon_classify(0.15) == DRIFTMON_WARNING);
    CHECK(driftmon_classify(0.2)  == DRIFTMON_SIGNIFICANT);  // boundary inclusive
    CHECK(driftmon_classify(5.0)  == DRIFTMON_SIGNIFICANT);
}

TEST(classify_nan_is_stable) {
    double nan_val = std::numeric_limits<double>::quiet_NaN();
    CHECK(driftmon_classify(nan_val) == DRIFTMON_STABLE);
}

// ─── Phase 2: consecutive tumbling windows ───────────────────────────────────

// Window 1 = reference distribution (PSI~0, stable). After reset, window 2 =
// fully shifted (significant). Verifies tumbling: reset starts a clean window.
TEST(consecutive_tumbling_windows) {
    std::string path = write_tmp("tumbling", VALID_1F);  // window_size=10
    driftmon_t* m = driftmon_create(path.c_str());
    CHECK(m != nullptr);

    // Window 1: matches reference.
    double v;
    for (int i = 0; i < 5; ++i) { v = 0.5; driftmon_observe(m, &v, 1); }
    for (int i = 0; i < 3; ++i) { v = 1.5; driftmon_observe(m, &v, 1); }
    for (int i = 0; i < 2; ++i) { v = 2.5; driftmon_observe(m, &v, 1); }
    CHECK(driftmon_ready(m) != 0);
    double max1;
    driftmon_compute(m, nullptr, &max1);
    CHECK(driftmon_classify(max1) == DRIFTMON_STABLE);

    driftmon_reset(m);

    // Window 2: all in bucket 0 → significant drift.
    for (int i = 0; i < 10; ++i) { v = 0.5; driftmon_observe(m, &v, 1); }
    CHECK(driftmon_ready(m) != 0);
    double max2;
    driftmon_compute(m, nullptr, &max2);
    CHECK(driftmon_classify(max2) == DRIFTMON_SIGNIFICANT);
    driftmon_destroy(m);
}

// ─── Phase 6: sliding window ─────────────────────────────────────────────────

// driftmon_create_ex with an invalid mode value must return NULL.
TEST(create_ex_invalid_mode_returns_null) {
    std::string path = write_tmp("ex_invalid_mode", VALID_1F);
    // Cast an out-of-range int to the enum — must be rejected.
    driftmon_t* m = driftmon_create_ex(path.c_str(),
                                       static_cast<driftmon_window_mode_t>(99));
    CHECK(m == nullptr);
}

// driftmon_create_ex with TUMBLING behaves identically to driftmon_create.
TEST(create_ex_tumbling_same_as_create) {
    std::string path = write_tmp("ex_tumbling", VALID_1F);
    driftmon_t* m = driftmon_create_ex(path.c_str(), DRIFTMON_TUMBLING);
    CHECK(m != nullptr);
    CHECK(driftmon_num_features(m) == 1);

    double v = 0.5;
    for (int i = 0; i < 9; ++i) driftmon_observe(m, &v, 1);
    CHECK(driftmon_ready(m) == 0);          // not yet full
    driftmon_observe(m, &v, 1);
    CHECK(driftmon_ready(m) != 0);          // full at window_size=10
    driftmon_reset(m);
    CHECK(driftmon_ready(m) == 0);          // reset clears tumbling window
    driftmon_destroy(m);
}

// Sliding window: not ready before window_size observations.
TEST(sliding_not_ready_before_window_size) {
    std::string path = write_tmp("sl_not_ready", VALID_1F);  // window_size=10
    driftmon_t* m = driftmon_create_ex(path.c_str(), DRIFTMON_SLIDING);
    CHECK(m != nullptr);

    double v = 0.5;
    for (int i = 0; i < 9; ++i) driftmon_observe(m, &v, 1);
    CHECK(driftmon_ready(m) == 0);
    driftmon_destroy(m);
}

// Sliding window: ready at exactly window_size and stays ready thereafter.
TEST(sliding_ready_at_window_size_and_stays) {
    std::string path = write_tmp("sl_ready", VALID_1F);
    driftmon_t* m = driftmon_create_ex(path.c_str(), DRIFTMON_SLIDING);
    CHECK(m != nullptr);

    double v = 0.5;
    for (int i = 0; i < 10; ++i) driftmon_observe(m, &v, 1);
    CHECK(driftmon_ready(m) != 0);          // ready at window_size

    // Additional observations: still ready (sliding never un-readies).
    for (int i = 0; i < 5; ++i) driftmon_observe(m, &v, 1);
    CHECK(driftmon_ready(m) != 0);
    driftmon_destroy(m);
}

// Sliding window: reset is a no-op — ready and counts are preserved.
TEST(sliding_reset_is_noop) {
    std::string path = write_tmp("sl_reset", VALID_1F);
    driftmon_t* m = driftmon_create_ex(path.c_str(), DRIFTMON_SLIDING);
    CHECK(m != nullptr);

    double v = 0.5;
    for (int i = 0; i < 10; ++i) driftmon_observe(m, &v, 1);
    CHECK(driftmon_ready(m) != 0);

    driftmon_reset(m);                      // must be a no-op
    CHECK(driftmon_ready(m) != 0);          // still ready
    double max_psi;
    driftmon_compute(m, nullptr, &max_psi);
    CHECK(max_psi >= 0.0);                  // compute still works
    driftmon_destroy(m);
}

// Sliding window reflects only the most recent window_size observations.
// Step 1: fill window with reference distribution → PSI~0 (stable).
// Step 2: add window_size drifted observations → oldest are evicted, PSI high.
TEST(sliding_reflects_recent_observations) {
    // VALID_1F: window_size=10, ref_ratios=[0.5, 0.3, 0.2], edges=[0,1,2,3]
    std::string path = write_tmp("sl_recent", VALID_1F);
    driftmon_t* m = driftmon_create_ex(path.c_str(), DRIFTMON_SLIDING);
    CHECK(m != nullptr);

    // Step 1: 10 obs matching reference → stable.
    double v;
    for (int i = 0; i < 5; ++i) { v = 0.5; driftmon_observe(m, &v, 1); }  // bucket 0
    for (int i = 0; i < 3; ++i) { v = 1.5; driftmon_observe(m, &v, 1); }  // bucket 1
    for (int i = 0; i < 2; ++i) { v = 2.5; driftmon_observe(m, &v, 1); }  // bucket 2

    CHECK(driftmon_ready(m) != 0);
    double max1;
    driftmon_compute(m, nullptr, &max1);
    CHECK_NEAR(max1, 0.0, 1e-9);           // reference distribution → PSI~0

    // Step 2: 10 more obs all in bucket 0 → evicts all reference-matching obs.
    for (int i = 0; i < 10; ++i) { v = 0.5; driftmon_observe(m, &v, 1); }

    double max2;
    driftmon_compute(m, nullptr, &max2);
    CHECK(max2 > 0.2);                      // only drifted obs in window → significant
    driftmon_destroy(m);
}

// ─── Phase 6: notification callback ─────────────────────────────────────────

struct CallbackCapture {
    int         call_count = 0;
    double      last_max_psi = -1.0;
    driftmon_severity_t last_severity = DRIFTMON_STABLE;
};

static void capture_cb(driftmon_t* /*m*/, double max_psi,
                       driftmon_severity_t severity, void* user_data) {
    auto* cap = static_cast<CallbackCapture*>(user_data);
    cap->call_count++;
    cap->last_max_psi  = max_psi;
    cap->last_severity = severity;
}

// No callback registered: compute must not crash.
TEST(callback_not_registered_no_crash) {
    std::string path = write_tmp("cb_none", VALID_1F);
    driftmon_t* m = driftmon_create(path.c_str());
    CHECK(m != nullptr);
    double v = 0.5;
    for (int i = 0; i < 10; ++i) driftmon_observe(m, &v, 1);
    driftmon_compute(m, nullptr, nullptr);  // must not crash
    driftmon_destroy(m);
}

// driftmon_set_callback with NULL m must not crash.
TEST(callback_set_on_null_handle_no_crash) {
    driftmon_set_callback(nullptr, capture_cb, nullptr);
    CHECK(true);
}

// Callback is fired once per driftmon_compute call with correct values.
TEST(callback_fires_with_correct_severity) {
    std::string path = write_tmp("cb_fire", VALID_1F);
    driftmon_t* m = driftmon_create(path.c_str());
    CHECK(m != nullptr);

    CallbackCapture cap;
    driftmon_set_callback(m, capture_cb, &cap);

    // Fill with reference distribution → STABLE.
    double v;
    for (int i = 0; i < 5; ++i) { v = 0.5; driftmon_observe(m, &v, 1); }
    for (int i = 0; i < 3; ++i) { v = 1.5; driftmon_observe(m, &v, 1); }
    for (int i = 0; i < 2; ++i) { v = 2.5; driftmon_observe(m, &v, 1); }

    driftmon_compute(m, nullptr, nullptr);
    CHECK(cap.call_count == 1);
    CHECK(cap.last_severity == DRIFTMON_STABLE);
    CHECK_NEAR(cap.last_max_psi, 0.0, 1e-9);
    driftmon_destroy(m);
}

// Callback receives SIGNIFICANT severity for a heavily drifted window.
TEST(callback_fires_significant_on_drift) {
    std::string path = write_tmp("cb_significant", VALID_1F);
    driftmon_t* m = driftmon_create(path.c_str());
    CHECK(m != nullptr);

    CallbackCapture cap;
    driftmon_set_callback(m, capture_cb, &cap);

    double v = 0.5;  // all in bucket 0 → significant drift
    for (int i = 0; i < 10; ++i) driftmon_observe(m, &v, 1);
    driftmon_compute(m, nullptr, nullptr);

    CHECK(cap.call_count == 1);
    CHECK(cap.last_severity == DRIFTMON_SIGNIFICANT);
    CHECK(cap.last_max_psi > 0.2);
    driftmon_destroy(m);
}

// Callback receives WARNING severity for a moderately drifted window.
// actual=[0.7, 0.2, 0.1] vs ref=[0.5, 0.3, 0.2] → PSI ≈ 0.177 (in [0.1, 0.2)).
TEST(callback_fires_warning_on_moderate_drift) {
    std::string path = write_tmp("cb_warning", VALID_1F);
    driftmon_t* m = driftmon_create(path.c_str());
    CHECK(m != nullptr);

    CallbackCapture cap;
    driftmon_set_callback(m, capture_cb, &cap);

    // 7 in bucket 0, 2 in bucket 1, 1 in bucket 2.
    double v;
    for (int i = 0; i < 7; ++i) { v = 0.5; driftmon_observe(m, &v, 1); }
    for (int i = 0; i < 2; ++i) { v = 1.5; driftmon_observe(m, &v, 1); }
    { v = 2.5; driftmon_observe(m, &v, 1); }

    driftmon_compute(m, nullptr, nullptr);
    CHECK(cap.call_count == 1);
    CHECK(cap.last_severity == DRIFTMON_WARNING);
    CHECK(cap.last_max_psi >= 0.1);
    CHECK(cap.last_max_psi <  0.2);
    driftmon_destroy(m);
}

// Setting fn to NULL unregisters the callback; subsequent compute must not call it.
TEST(callback_null_unregisters) {
    std::string path = write_tmp("cb_unreg", VALID_1F);
    driftmon_t* m = driftmon_create(path.c_str());
    CHECK(m != nullptr);

    CallbackCapture cap;
    driftmon_set_callback(m, capture_cb, &cap);

    double v = 0.5;
    for (int i = 0; i < 10; ++i) driftmon_observe(m, &v, 1);
    driftmon_compute(m, nullptr, nullptr);
    CHECK(cap.call_count == 1);

    // Unregister and reset for a second window.
    driftmon_set_callback(m, nullptr, nullptr);
    driftmon_reset(m);
    for (int i = 0; i < 10; ++i) driftmon_observe(m, &v, 1);
    driftmon_compute(m, nullptr, nullptr);
    CHECK(cap.call_count == 1);  // still 1 — callback was not fired again
    driftmon_destroy(m);
}

int main() { return RUN_ALL(); }
