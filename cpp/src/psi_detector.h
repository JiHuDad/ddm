/*
 * psi_detector.h — Population Stability Index detector (SPEC §2.1, §6.3).
 *
 * PSI = Σ (a_k − e_k)·ln(a_k / e_k) over physical bins, with an epsilon floor of
 * 1e-4 on both actual (a) and expected (e) ratios. Same formula and constants as
 * the frozen core (src/driftmon.cpp); reimplemented here over the tap's physical
 * bin layout (which adds underflow/overflow bins the core does not have).
 */
#ifndef DRIFTMON_PSI_DETECTOR_H
#define DRIFTMON_PSI_DETECTOR_H

#include "detector.h"

namespace driftmon {

inline constexpr double DRIFTMON_PSI_EPSILON = 1e-4;

class PsiDetector : public Detector {
public:
    void        configure(const FeatureRef& ref) override;
    DriftResult eval(const Histogram& current) override;

private:
    std::vector<double> ref_ratios_;   // expected ratios over physical bins
    double threshold_ = 0.2;
};

}  // namespace driftmon

#endif  // DRIFTMON_PSI_DETECTOR_H
