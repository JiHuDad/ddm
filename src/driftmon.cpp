// driftmon.cpp — drift monitoring core (Phase 0 stub).
//
// Implements the C ABI declared in include/driftmon.h. The header is FROZEN
// (see SPEC.md). This translation unit may use C++ internally, but exposes
// only the C contract and depends on nothing beyond the standard library.
//
// Phase 0: stubs that compile, link, and let the test runner go green.
// Phase 1+ fills in histogram accumulation, reference loading, and PSI.
#include "driftmon.h"

#include <vector>

#include "json_min.h"

// Private representation behind the opaque driftmon_t handle.
struct driftmon {
    dm::ReferenceProfile reference;
    std::vector<std::vector<long>> window_counts;  // per-feature bucket counts
    long observations = 0;  // observations accumulated in the current window
};

extern "C" {

driftmon_t* driftmon_create(const char* reference_json_path) {
    if (reference_json_path == nullptr) return nullptr;
    // Phase 1: TODO — load reference, allocate per-feature bucket counters.
    // For now we return NULL because no reference can be parsed yet (the
    // JSON parser is a Phase 0 stub). Smoke tests exercise the NULL path.
    dm::ReferenceProfile ref;
    if (!dm::load_reference(reference_json_path, ref)) {
        return nullptr;
    }
    driftmon_t* m = new (std::nothrow) driftmon_t();
    if (m == nullptr) return nullptr;
    m->reference = std::move(ref);
    return m;
}

int driftmon_num_features(const driftmon_t* m) {
    if (m == nullptr) return 0;
    return static_cast<int>(m->reference.features.size());
}

void driftmon_observe(driftmon_t* m, const double* feats, int n) {
    (void)feats;
    (void)n;
    if (m == nullptr) return;
    // Phase 1: TODO — bucketize each feature and increment window_counts.
}

int driftmon_ready(const driftmon_t* m) {
    if (m == nullptr) return 0;
    // Phase 1: TODO — return m->observations >= m->reference.window_size.
    return 0;
}

void driftmon_compute(driftmon_t* m, double* psi_out, double* max_psi) {
    if (m == nullptr) {
        if (max_psi != nullptr) *max_psi = 0.0;
        return;
    }
    // Phase 1: TODO — compute per-feature PSI per SPEC.md, fill psi_out, and
    // set *max_psi to the maximum. Apply the epsilon-floor rule for empty
    // buckets / zero division documented in SPEC.md.
    const int n = driftmon_num_features(m);
    if (psi_out != nullptr) {
        for (int i = 0; i < n; ++i) psi_out[i] = 0.0;
    }
    if (max_psi != nullptr) *max_psi = 0.0;
}

void driftmon_reset(driftmon_t* m) {
    if (m == nullptr) return;
    m->observations = 0;
    // Phase 1: TODO — zero out window_counts.
}

void driftmon_destroy(driftmon_t* m) {
    delete m;  // safe for nullptr
}

}  // extern "C"
