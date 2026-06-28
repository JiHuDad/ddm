/*
 * detector.h — driftmon-cpp detector interface (SPEC §6.3).
 *
 * Worker-internal. Each drift test (PSI in Phase 1; KS/ADWIN/CUSUM later)
 * implements Detector. Detectors are pure compute over a frozen histogram
 * snapshot — no shm, no I/O. Zero external dependencies.
 */
#ifndef DRIFTMON_DETECTOR_H
#define DRIFTMON_DETECTOR_H

#include <cstdint>
#include <string>
#include <vector>

namespace driftmon {

// A frozen per-feature histogram snapshot: physical-bin counts for ONE feature
// (index 0 = underflow, 1..B = interior, B+1 = overflow — see shm_abi.h).
struct Histogram {
    std::vector<uint64_t> counts;   // length = interior_bins + 2
    uint64_t total = 0;             // Σ counts (cached)
};

// One feature's reference for a detector: the expected ratios over the SAME
// physical-bin layout as Histogram (underflow/overflow included), plus its
// alarm threshold.
struct FeatureRef {
    std::string name;
    std::vector<double> ref_ratios;   // length = interior_bins + 2, sums to ~1
    double threshold = 0.0;           // detector-specific alarm threshold
};

struct DriftResult {
    double score = 0.0;
    bool   alarm = false;
};

class Detector {
public:
    virtual void        configure(const FeatureRef& ref) = 0;
    virtual DriftResult eval(const Histogram& current)   = 0;
    virtual ~Detector() = default;
};

// Mean physical-bin index of a histogram — the scalar drift signal the
// streaming detectors (CUSUM/ADWIN) track across windows.
inline double histogram_mean_bin(const Histogram& h) {
    if (h.total == 0) return 0.0;
    double acc = 0.0;
    for (size_t k = 0; k < h.counts.size(); ++k)
        acc += static_cast<double>(k) * static_cast<double>(h.counts[k]);
    return acc / static_cast<double>(h.total);
}

// Mean physical-bin index implied by a reference ratio vector.
inline double ratios_mean_bin(const std::vector<double>& r) {
    double acc = 0.0, sum = 0.0;
    for (size_t k = 0; k < r.size(); ++k) { acc += static_cast<double>(k) * r[k]; sum += r[k]; }
    return sum > 0.0 ? acc / sum : 0.0;
}

}  // namespace driftmon

#endif  // DRIFTMON_DETECTOR_H
