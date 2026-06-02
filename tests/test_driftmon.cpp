// test_driftmon.cpp — skeleton tests for the driftmon contract.
//
// Phase 0: only self-evident smoke tests that pass against the stub. The real
// coverage (PSI value checks against known distributions, reference round-trip,
// bucket-boundary cases) is left as TODO placeholders for Phase 1 — to be
// filled in by whichever AI tool picks up the testing tickets in TASKS.md.
#include "driftmon.h"
#include "test_framework.h"

// --- Phase 0 smoke tests (pass against the stub) ---------------------------

TEST(destroy_null_is_safe) {
    // Contract: driftmon_destroy(NULL) must be a no-op, not a crash.
    driftmon_destroy(nullptr);
    CHECK(true);
}

TEST(create_with_null_path_returns_null) {
    // Contract: NULL path -> NULL handle (no parsing possible).
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

// --- Phase 1 TODO placeholders (do NOT enable until implemented) -----------
//
// TEST(psi_zero_for_identical_distribution) {
//     // Load a reference, observe samples drawn from the same distribution,
//     // expect max_psi ~ 0.
// }
//
// TEST(psi_above_threshold_for_shifted_distribution) {
//     // Shift the distribution; expect max_psi > 0.2 (significant drift).
// }
//
// TEST(reference_roundtrip) {
//     // load_reference() of a known reference.json yields the expected
//     // edges/ref_ratios.
// }
//
// TEST(bucket_boundary_and_empty_bucket) {
//     // Values exactly on edges, and empty buckets, follow SPEC.md's
//     // epsilon-floor / zero-division rules.
// }

int main() { return RUN_ALL(); }
