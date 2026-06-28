// ks_detector.cpp — KS statistic over physical-bin histograms.
#include "ks_detector.h"

#include <cmath>

namespace driftmon {

void KsDetector::configure(const FeatureRef& ref) {
    threshold_ = ref.threshold;
    ref_cdf_.assign(ref.ref_ratios.size(), 0.0);
    double acc = 0.0;
    for (size_t k = 0; k < ref.ref_ratios.size(); ++k) {
        acc += ref.ref_ratios[k];
        ref_cdf_[k] = acc;
    }
}

DriftResult KsDetector::eval(const Histogram& current) {
    DriftResult r;
    if (current.total == 0 || ref_cdf_.empty()) return r;

    const double denom = static_cast<double>(current.total);
    double cum_a = 0.0;
    double max_gap = 0.0;
    for (size_t k = 0; k < ref_cdf_.size(); ++k) {
        const double c = (k < current.counts.size())
                             ? static_cast<double>(current.counts[k])
                             : 0.0;
        cum_a += c / denom;
        const double gap = std::fabs(cum_a - ref_cdf_[k]);
        if (gap > max_gap) max_gap = gap;
    }
    r.score = max_gap;
    r.alarm = max_gap >= threshold_;
    return r;
}

}  // namespace driftmon
