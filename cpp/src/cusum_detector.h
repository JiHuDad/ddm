/*
 * cusum_detector.h — CUSUM streaming drift detector (SPEC §5.3 R3.4, §6.3).
 *
 * Unlike PSI/KS (batch over one window), CUSUM keeps running state ACROSS
 * evaluated windows: a two-sided cumulative sum of the per-window mean-bin
 * signal against the reference mean. It flags a sustained mean shift even when
 * each individual window looks unremarkable. State is retained between eval()
 * calls (no batch boundary) per R3.4.
 */
#ifndef DRIFTMON_CUSUM_DETECTOR_H
#define DRIFTMON_CUSUM_DETECTOR_H

#include "detector.h"

namespace driftmon {

// Defaults: k = slack (half the shift to ignore), h = decision threshold, both
// in units of bin indices. Tunable via the constructor.
inline constexpr double DRIFTMON_CUSUM_K = 0.5;
inline constexpr double DRIFTMON_CUSUM_H = 5.0;

class CusumDetector : public Detector {
public:
    explicit CusumDetector(double k = DRIFTMON_CUSUM_K, double h = DRIFTMON_CUSUM_H)
        : k_(k), h_(h) {}

    void        configure(const FeatureRef& ref) override;   // target = reference mean bin
    DriftResult eval(const Histogram& current) override;      // streaming update

    void reset() { s_hi_ = 0.0; s_lo_ = 0.0; }

private:
    double target_ = 0.0;   // reference mean bin index
    double k_;
    double h_;
    double s_hi_ = 0.0;     // running positive-shift sum
    double s_lo_ = 0.0;     // running negative-shift sum
};

}  // namespace driftmon

#endif  // DRIFTMON_CUSUM_DETECTOR_H
