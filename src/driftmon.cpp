// driftmon.cpp — drift monitoring core (Phase 1 + Phase 6 sliding window).
//
// Implements the C ABI in include/driftmon.h (FROZEN — see SPEC.md §4).
// Uses C++ internally; depends on nothing beyond the standard library.
// See SPEC.md for algorithm details (PSI, epsilon floor, clamp rules, windowing).
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
    driftmon_window_mode_t mode = DRIFTMON_TUMBLING;

    // Per-feature per-bucket counts for the current window.
    // Tumbling: accumulated since last reset.
    // Sliding:  incremental running sum of the last window_size observations.
    std::vector<std::vector<long>> window_counts;  // [feature][bucket]

    // Total observations: in tumbling mode reset to 0 by driftmon_reset;
    // in sliding mode never reset (driftmon_reset is a no-op).
    long observations = 0;

    // Sliding-mode circular buffer.
    // sliding_buf[slot][feature] = bucket_index, -1 means NaN/Inf (skipped).
    // Size: window_size × num_features; allocated only for DRIFTMON_SLIDING.
    std::vector<std::vector<int>> sliding_buf;  // [slot][feature]
    int sliding_head = 0;  // next write position (wraps modulo window_size)
};

extern "C" {

driftmon_t* driftmon_create_ex(const char* reference_json_path,
                                driftmon_window_mode_t mode) {
    if (reference_json_path == nullptr) return nullptr;
    if (mode != DRIFTMON_TUMBLING && mode != DRIFTMON_SLIDING) return nullptr;
    dm::ReferenceProfile ref;
    if (!dm::load_reference(reference_json_path, ref)) return nullptr;
    driftmon_t* m = new (std::nothrow) driftmon_t();
    if (m == nullptr) return nullptr;
    m->reference = std::move(ref);
    m->mode = mode;
    const size_t nf = m->reference.features.size();
    m->window_counts.resize(nf);
    for (size_t j = 0; j < nf; ++j)
        m->window_counts[j].assign(m->reference.features[j].ref_ratios.size(), 0L);
    if (mode == DRIFTMON_SLIDING) {
        const long ws = static_cast<long>(m->reference.window_size);
        // Initialise all slots to -1 (empty).
        m->sliding_buf.assign(ws, std::vector<int>(static_cast<int>(nf), -1));
    }
    return m;
}

driftmon_t* driftmon_create(const char* reference_json_path) {
    return driftmon_create_ex(reference_json_path, DRIFTMON_TUMBLING);
}

int driftmon_num_features(const driftmon_t* m) {
    if (m == nullptr) return 0;
    return static_cast<int>(m->reference.features.size());
}

void driftmon_observe(driftmon_t* m, const double* feats, int n) {
    if (m == nullptr || feats == nullptr) return;
    if (n != driftmon_num_features(m)) return;
    const int nf = n;

    // Compute bucket indices for every feature in this observation.
    // -1 means NaN/Inf — the feature is skipped for counting purposes.
    std::vector<int> buckets(nf, -1);
    for (int j = 0; j < nf; ++j) {
        const double v = feats[j];
        if (std::isnan(v) || std::isinf(v)) continue;
        const auto& edges = m->reference.features[j].edges;
        const int nb = static_cast<int>(m->reference.features[j].ref_ratios.size());
        int k;
        if (v < edges.front()) {
            k = 0;
        } else if (v >= edges.back()) {
            k = nb - 1;
        } else {
            auto it = std::upper_bound(edges.begin(), edges.end(), v);
            k = static_cast<int>(it - edges.begin()) - 1;
        }
        buckets[j] = k;
    }

    if (m->mode == DRIFTMON_SLIDING) {
        const long ws = static_cast<long>(m->reference.window_size);
        const int slot = m->sliding_head;

        // Buffer full: subtract the oldest observation's contribution before
        // overwriting its slot.
        if (m->observations >= ws) {
            for (int j = 0; j < nf; ++j) {
                const int old_k = m->sliding_buf[slot][j];
                if (old_k >= 0) m->window_counts[j][old_k]--;
            }
        }

        // Store the new observation's bucket indices in the circular buffer.
        for (int j = 0; j < nf; ++j) m->sliding_buf[slot][j] = buckets[j];
        m->sliding_head = static_cast<int>((slot + 1) % ws);
    }

    // Add the new observation to the running counts.
    for (int j = 0; j < nf; ++j)
        if (buckets[j] >= 0) m->window_counts[j][buckets[j]]++;

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

        // No valid observations for this feature: drift is undefined, report 0
        // rather than letting the epsilon floor manufacture a spurious high PSI.
        if (feature_obs == 0) {
            if (psi_out != nullptr) psi_out[j] = 0.0;
            continue;
        }

        double psi = 0.0;
        for (int k = 0; k < nb; ++k) {
            double e = ref_ratios[k];
            double a = (denom > 0.0) ? (m->window_counts[j][k] / denom) : 0.0;
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
    // NaN falls through to STABLE (both comparisons false) — safe default.
    if (psi >= 0.2) return DRIFTMON_SIGNIFICANT;
    if (psi >= 0.1) return DRIFTMON_WARNING;
    return DRIFTMON_STABLE;
}

void driftmon_reset(driftmon_t* m) {
    if (m == nullptr) return;
    if (m->mode == DRIFTMON_SLIDING) return;  // no-op: rolling buffer continues
    m->observations = 0;
    for (auto& counts : m->window_counts)
        std::fill(counts.begin(), counts.end(), 0L);
}

void driftmon_destroy(driftmon_t* m) {
    delete m;
}

}  // extern "C"
