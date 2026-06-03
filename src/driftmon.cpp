// driftmon.cpp — drift monitoring core (Phase 1).
//
// Implements the C ABI in include/driftmon.h (FROZEN — see SPEC.md §4).
// Uses C++ internally; depends on nothing beyond the standard library.
// See SPEC.md for algorithm details (PSI, epsilon floor, clamp rules).
#include "driftmon.h"

#include <algorithm>
#include <cmath>
#include <new>
#include <vector>

#include "json_min.h"

static constexpr double PSI_EPSILON = 1e-4;

// Private state behind the opaque handle.
struct driftmon {
    dm::ReferenceProfile reference;
    std::vector<std::vector<long>> window_counts;  // [feature][bucket]
    long observations = 0;
};

extern "C" {

driftmon_t* driftmon_create(const char* reference_json_path) {
    if (reference_json_path == nullptr) return nullptr;
    dm::ReferenceProfile ref;
    if (!dm::load_reference(reference_json_path, ref)) return nullptr;
    driftmon_t* m = new (std::nothrow) driftmon_t();
    if (m == nullptr) return nullptr;
    m->reference = std::move(ref);
    const size_t nf = m->reference.features.size();
    m->window_counts.resize(nf);
    for (size_t j = 0; j < nf; ++j)
        m->window_counts[j].assign(m->reference.features[j].ref_ratios.size(), 0L);
    return m;
}

int driftmon_num_features(const driftmon_t* m) {
    if (m == nullptr) return 0;
    return static_cast<int>(m->reference.features.size());
}

void driftmon_observe(driftmon_t* m, const double* feats, int n) {
    if (m == nullptr || feats == nullptr) return;
    if (n != driftmon_num_features(m)) return;
    const int nf = n;
    for (int j = 0; j < nf; ++j) {
        const double v = feats[j];
        // NaN/Inf: skip this feature's bucket, per SPEC §2.4.
        if (std::isnan(v) || std::isinf(v)) continue;
        const auto& edges = m->reference.features[j].edges;
        const int nb = static_cast<int>(m->reference.features[j].ref_ratios.size());
        int k;
        if (v < edges.front()) {
            k = 0;  // clamp to first bucket
        } else if (v >= edges.back()) {
            k = nb - 1;  // clamp to last bucket
        } else {
            // upper_bound gives first edge > v; the bucket index is one less.
            auto it = std::upper_bound(edges.begin(), edges.end(), v);
            k = static_cast<int>(it - edges.begin()) - 1;
        }
        m->window_counts[j][k]++;
    }
    m->observations++;
}

int driftmon_ready(const driftmon_t* m) {
    if (m == nullptr) return 0;
    return m->observations >= static_cast<long>(m->reference.window_size) ? 1 : 0;
}

void driftmon_compute(driftmon_t* m, double* psi_out, double* max_psi) {
    if (m == nullptr) {
        if (max_psi != nullptr) *max_psi = 0.0;
        return;
    }
    const int nf = driftmon_num_features(m);
    double max_val = 0.0;
    for (int j = 0; j < nf; ++j) {
        const auto& ref_ratios = m->reference.features[j].ref_ratios;
        const int nb = static_cast<int>(ref_ratios.size());
        // Use per-feature observation count so NaN-skipped observations don't
        // distort the actual ratios for that feature (SPEC §2.4).
        long feature_obs = 0;
        for (long c : m->window_counts[j]) feature_obs += c;
        const double denom = static_cast<double>(feature_obs);

        // No valid observations for this feature (e.g. all NaN/Inf, or out of
        // any window): drift is undefined, so report 0 rather than letting the
        // epsilon floor manufacture a spurious high PSI (SPEC §2.4).
        if (feature_obs == 0) {
            if (psi_out != nullptr) psi_out[j] = 0.0;
            continue;
        }

        double psi = 0.0;
        for (int k = 0; k < nb; ++k) {
            double e = ref_ratios[k];
            double a = (denom > 0.0) ? (m->window_counts[j][k] / denom) : 0.0;
            // Epsilon floor to avoid log(0) / division by zero (SPEC §2.4).
            if (e < PSI_EPSILON) e = PSI_EPSILON;
            if (a < PSI_EPSILON) a = PSI_EPSILON;
            psi += (a - e) * std::log(a / e);
        }
        if (psi_out != nullptr) psi_out[j] = psi;
        if (psi > max_val) max_val = psi;
    }
    if (max_psi != nullptr) *max_psi = max_val;
}

driftmon_severity_t driftmon_classify(double psi) {
    // Thresholds per SPEC §2.3. NaN falls through to STABLE (both comparisons
    // are false), which is the safe default for an undefined PSI.
    if (psi >= 0.2) return DRIFTMON_SIGNIFICANT;
    if (psi >= 0.1) return DRIFTMON_WARNING;
    return DRIFTMON_STABLE;
}

void driftmon_reset(driftmon_t* m) {
    if (m == nullptr) return;
    m->observations = 0;
    for (auto& counts : m->window_counts)
        std::fill(counts.begin(), counts.end(), 0L);
}

void driftmon_destroy(driftmon_t* m) {
    delete m;
}

}  // extern "C"
