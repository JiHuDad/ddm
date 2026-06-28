// cusum_detector.cpp — two-sided tabular CUSUM over the mean-bin signal.
#include "cusum_detector.h"

#include <algorithm>

namespace driftmon {

void CusumDetector::configure(const FeatureRef& ref) {
    target_ = ratios_mean_bin(ref.ref_ratios);
    reset();
}

DriftResult CusumDetector::eval(const Histogram& current) {
    DriftResult r;
    if (current.total == 0) return r;

    const double x = histogram_mean_bin(current);
    // Standard two-sided CUSUM with slack k_.
    s_hi_ = std::max(0.0, s_hi_ + (x - target_ - k_));
    s_lo_ = std::max(0.0, s_lo_ + (target_ - k_ - x));

    r.score = std::max(s_hi_, s_lo_);
    r.alarm = r.score > h_;
    if (r.alarm) reset();   // signal the change once, then resume from baseline
    return r;
}

}  // namespace driftmon
