// driftmon.cpp — drift monitoring core (Phase 1 + Phase 6).
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
    // One or more reference profiles (multi-reference support, Phase 6c).
    // references[0] is always present and defines the canonical binning
    // (edges used in driftmon_observe) and structural metadata (window_size,
    // feature count, bucket counts per feature).
    // All entries must share the same structure — validated in create_multi.
    std::vector<dm::ReferenceProfile> references;

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

    // Notification callback (Phase 6b). NULL means no callback registered.
    driftmon_callback_t callback      = nullptr;
    void*               callback_data = nullptr;
};

// Shared initialisation after references[] is populated.
static driftmon_t* init_monitor(std::vector<dm::ReferenceProfile> refs,
                                driftmon_window_mode_t mode) {
    driftmon_t* m = new (std::nothrow) driftmon_t();
    if (m == nullptr) return nullptr;
    m->references = std::move(refs);
    m->mode = mode;
    const size_t nf = m->references[0].features.size();
    m->window_counts.resize(nf);
    for (size_t j = 0; j < nf; ++j)
        m->window_counts[j].assign(
            m->references[0].features[j].ref_ratios.size(), 0L);
    if (mode == DRIFTMON_SLIDING) {
        const long ws = static_cast<long>(m->references[0].window_size);
        m->sliding_buf.assign(ws, std::vector<int>(static_cast<int>(nf), -1));
    }
    return m;
}

extern "C" {

driftmon_t* driftmon_create_ex(const char* reference_json_path,
                                driftmon_window_mode_t mode) {
    if (reference_json_path == nullptr) return nullptr;
    if (mode != DRIFTMON_TUMBLING && mode != DRIFTMON_SLIDING) return nullptr;
    dm::ReferenceProfile ref;
    if (!dm::load_reference(reference_json_path, ref)) return nullptr;
    std::vector<dm::ReferenceProfile> refs;
    refs.push_back(std::move(ref));
    return init_monitor(std::move(refs), mode);
}

driftmon_t* driftmon_create(const char* reference_json_path) {
    return driftmon_create_ex(reference_json_path, DRIFTMON_TUMBLING);
}

driftmon_t* driftmon_create_multi(const char** paths, int n) {
    if (paths == nullptr || n < 1) return nullptr;

    std::vector<dm::ReferenceProfile> refs(n);
    for (int i = 0; i < n; ++i) {
        if (paths[i] == nullptr) return nullptr;
        if (!dm::load_reference(paths[i], refs[i])) return nullptr;
    }

    // Validate all references are structurally compatible with refs[0].
    const auto& base = refs[0];
    for (int i = 1; i < n; ++i) {
        if (refs[i].window_size != base.window_size) return nullptr;
        if (refs[i].features.size() != base.features.size()) return nullptr;
        for (size_t j = 0; j < base.features.size(); ++j) {
            if (refs[i].features[j].name != base.features[j].name) return nullptr;
            if (refs[i].features[j].ref_ratios.size() !=
                base.features[j].ref_ratios.size()) return nullptr;
        }
    }

    return init_monitor(std::move(refs), DRIFTMON_TUMBLING);
}

int driftmon_num_features(const driftmon_t* m) {
    if (m == nullptr) return 0;
    return static_cast<int>(m->references[0].features.size());
}

void driftmon_observe(driftmon_t* m, const double* feats, int n) {
    if (m == nullptr || feats == nullptr) return;
    if (n != driftmon_num_features(m)) return;
    const int nf = n;

    // Compute bucket indices using references[0] edges (canonical binning).
    // -1 means NaN/Inf — the feature is skipped for counting purposes.
    std::vector<int> buckets(nf, -1);
    for (int j = 0; j < nf; ++j) {
        const double v = feats[j];
        if (std::isnan(v) || std::isinf(v)) continue;
        const auto& edges = m->references[0].features[j].edges;
        const int nb = static_cast<int>(
            m->references[0].features[j].ref_ratios.size());
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
        const long ws = static_cast<long>(m->references[0].window_size);
        const int slot = m->sliding_head;

        // Buffer full: subtract the oldest observation's contribution before
        // overwriting its slot.
        if (m->observations >= ws) {
            for (int j = 0; j < nf; ++j) {
                const int old_k = m->sliding_buf[slot][j];
                if (old_k >= 0) m->window_counts[j][old_k]--;
            }
        }

        for (int j = 0; j < nf; ++j) m->sliding_buf[slot][j] = buckets[j];
        m->sliding_head = static_cast<int>((slot + 1) % ws);
    }

    for (int j = 0; j < nf; ++j)
        if (buckets[j] >= 0) m->window_counts[j][buckets[j]]++;

    m->observations++;
}

int driftmon_ready(const driftmon_t* m) {
    if (m == nullptr) return 0;
    return m->observations >=
           static_cast<long>(m->references[0].window_size) ? 1 : 0;
}

void driftmon_compute(driftmon_t* m, double* psi_out, double* max_psi) {
    if (m == nullptr) {
        if (max_psi != nullptr) *max_psi = 0.0;
        return;
    }
    const int nf = driftmon_num_features(m);
    double max_val = 0.0;

    for (int j = 0; j < nf; ++j) {
        // Per-feature observation count (NaN-safe denominator, SPEC §2.4).
        long feature_obs = 0;
        for (long c : m->window_counts[j]) feature_obs += c;
        const double denom = static_cast<double>(feature_obs);

        if (feature_obs == 0) {
            if (psi_out != nullptr) psi_out[j] = 0.0;
            continue;
        }

        // Compute PSI against every reference; take max for this feature.
        double feature_psi = 0.0;
        for (const auto& ref : m->references) {
            const auto& ref_ratios = ref.features[j].ref_ratios;
            const int nb = static_cast<int>(ref_ratios.size());
            double psi = 0.0;
            for (int k = 0; k < nb; ++k) {
                double e = ref_ratios[k];
                double a = m->window_counts[j][k] / denom;
                if (e < PSI_EPSILON) e = PSI_EPSILON;
                if (a < PSI_EPSILON) a = PSI_EPSILON;
                psi += (a - e) * std::log(a / e);
            }
            if (psi > feature_psi) feature_psi = psi;
        }

        if (psi_out != nullptr) psi_out[j] = feature_psi;
        if (feature_psi > max_val) max_val = feature_psi;
    }

    if (max_psi != nullptr) *max_psi = max_val;

    if (m->callback != nullptr)
        m->callback(m, max_val, driftmon_classify(max_val), m->callback_data);
}

driftmon_severity_t driftmon_classify(double psi) {
    // NaN falls through to STABLE (both comparisons false) — safe default.
    if (psi >= 0.2) return DRIFTMON_SIGNIFICANT;
    if (psi >= 0.1) return DRIFTMON_WARNING;
    return DRIFTMON_STABLE;
}

void driftmon_set_callback(driftmon_t*         m,
                           driftmon_callback_t fn,
                           void*               user_data) {
    if (m == nullptr) return;
    m->callback      = fn;
    m->callback_data = user_data;
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
