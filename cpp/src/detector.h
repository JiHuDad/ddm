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

}  // namespace driftmon

#endif  // DRIFTMON_DETECTOR_H
