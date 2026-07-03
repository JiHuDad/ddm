/*
 * bundle.h — driftmon-cpp reference bundle (SPEC §5.4).
 *
 * The operations↔development handoff contract: the model team ships one JSON
 * bundle per model; the framework reads numbers from it (never hardcodes them).
 * Richer than the old reference.json — per-feature {dtype, index, bin_edges,
 * ref_hist (counts), psi_threshold}, plus outputs[], window{}, tests[].
 *
 * The parser concatenates inputs then outputs into ONE ordered feature list so
 * the tap and detectors treat input/output uniformly (flat feature space).
 */
#ifndef DRIFTMON_BUNDLE_H
#define DRIFTMON_BUNDLE_H

#include <string>
#include <vector>

namespace driftmon {

struct BundleFeature {
    std::string name;
    bool   is_output = false;          // false = model input, true = model output
    std::vector<double> bin_edges;     // length = interior_bins + 1, strictly increasing
    std::vector<long>   ref_hist;      // length = interior_bins (training counts)
    double psi_threshold = 0.2;        // default per SPEC if unset
    double ks_threshold = 0.1;         // default if unset (DRIFTMON_DEFAULT_KS_THRESHOLD)

    // interior bins (= ref_hist.size()); physical bins add underflow+overflow.
    size_t interior_bins() const { return ref_hist.size(); }
};

struct Bundle {
    std::string schema_version;
    std::string model_id;
    long min_samples = 0;              // window.min_samples (default applied if unset)
    long max_seconds = 0;              // window.max_seconds (default applied if unset)
    // Data-quality gates (optional "quality" block). NaN influx / a feature
    // going constant means the PIPELINE broke — flagged separately from drift
    // so operators don't chase a retrain when the fix is upstream repair.
    double nan_ratio_max = 0.01;       // quality alarm if NaN share exceeds this
    double oor_ratio_max = 0.05;       // out-of-range flag (drift-kind input, not quality alarm)
    std::vector<std::string> tests;    // active detectors (e.g. "psi", "ks")
    std::vector<BundleFeature> features;  // inputs first, then outputs

    // Total physical bins across all features (interior + 2 each).
    long n_bins_total() const;
};

// Defaults applied when window fields are absent (SPEC §7 / R3.3).
inline constexpr long DRIFTMON_DEFAULT_MIN_SAMPLES = 1000;
inline constexpr long DRIFTMON_DEFAULT_MAX_SECONDS = 60;

// Parse + validate (R4.1) a bundle file. On any schema violation returns false
// and sets `err` to a clear human-readable reason — the caller's GATE (R4.2)
// then disables monitoring for that model WITHOUT affecting serving or siblings.
// "Silently run with wrong values" is forbidden.
bool load_bundle(const std::string& path, Bundle& out, std::string& err);

// Parse + validate from an in-memory JSON string (for tests / non-file sources).
bool parse_bundle(const std::string& json, Bundle& out, std::string& err);

}  // namespace driftmon

#endif  // DRIFTMON_BUNDLE_H
