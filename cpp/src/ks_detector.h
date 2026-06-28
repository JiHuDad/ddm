/*
 * ks_detector.h — Kolmogorov–Smirnov drift detector (SPEC §5.3, §6.3).
 *
 * KS statistic = max_k | CDF_actual(k) − CDF_expected(k) | over the physical
 * bins (the largest gap between the cumulative reference and current
 * distributions). A complementary view to PSI: sensitive to a localized shift
 * of mass even when the per-bin PSI terms stay individually small.
 */
#ifndef DRIFTMON_KS_DETECTOR_H
#define DRIFTMON_KS_DETECTOR_H

#include "detector.h"

namespace driftmon {

inline constexpr double DRIFTMON_DEFAULT_KS_THRESHOLD = 0.1;

class KsDetector : public Detector {
public:
    void        configure(const FeatureRef& ref) override;
    DriftResult eval(const Histogram& current) override;

private:
    std::vector<double> ref_cdf_;   // cumulative expected ratios over physical bins
    double threshold_ = DRIFTMON_DEFAULT_KS_THRESHOLD;
};

}  // namespace driftmon

#endif  // DRIFTMON_KS_DETECTOR_H
