// psi_detector.cpp — PSI implementation (SPEC §2.1, §2.4).
#include "psi_detector.h"

#include <cmath>

namespace driftmon {

void PsiDetector::configure(const FeatureRef& ref) {
    ref_ratios_ = ref.ref_ratios;
    threshold_  = ref.threshold;
}

DriftResult PsiDetector::eval(const Histogram& current) {
    DriftResult r;
    // NaN-safe denominator (SPEC §2.4): zero observations ⇒ PSI 0, no spurious drift.
    if (current.total == 0 || ref_ratios_.empty()) return r;

    const double denom = static_cast<double>(current.total);
    const size_t nb = ref_ratios_.size();
    double psi = 0.0;
    for (size_t k = 0; k < nb; ++k) {
        double e = ref_ratios_[k];
        double a = (k < current.counts.size())
                       ? static_cast<double>(current.counts[k]) / denom
                       : 0.0;
        if (e < DRIFTMON_PSI_EPSILON) e = DRIFTMON_PSI_EPSILON;
        if (a < DRIFTMON_PSI_EPSILON) a = DRIFTMON_PSI_EPSILON;
        psi += (a - e) * std::log(a / e);
    }
    r.score = psi;
    r.alarm = psi >= threshold_;
    return r;
}

}  // namespace driftmon
