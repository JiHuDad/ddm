// adwin_detector.cpp — simplified ADWIN over the mean-bin signal.
#include "adwin_detector.h"

#include <cmath>

namespace driftmon {

void AdwinDetector::configure(const FeatureRef& ref) {
    // Signal lives in [0, n_physical_bins-1]; use that span to scale the bound.
    range_ = ref.ref_ratios.empty() ? 1.0
                                     : static_cast<double>(ref.ref_ratios.size() - 1);
    if (range_ <= 0.0) range_ = 1.0;
    reset();
}

DriftResult AdwinDetector::eval(const Histogram& current) {
    DriftResult r;
    if (current.total == 0) return r;

    window_.push_back(histogram_mean_bin(current));
    while (window_.size() > max_window_) window_.pop_front();

    const size_t n = window_.size();
    if (n < 2) return r;

    // Running prefix sums let us test every split point cheaply.
    double total = 0.0;
    for (double v : window_) total += v;

    double prefix = 0.0;
    double best_gap = 0.0;
    size_t best_cut = 0;
    for (size_t cut = 1; cut < n; ++cut) {
        prefix += window_[cut - 1];
        const double n0 = static_cast<double>(cut);
        const double n1 = static_cast<double>(n - cut);
        const double m0 = prefix / n0;
        const double m1 = (total - prefix) / n1;
        // Hoeffding bound on the mean difference at confidence delta_.
        const double m_harm = 1.0 / (1.0 / n0 + 1.0 / n1);   // harmonic-ish term
        const double eps = range_ * std::sqrt(std::log(2.0 / delta_) / (2.0 * m_harm));
        const double gap = std::fabs(m0 - m1);
        if (gap > eps && gap > best_gap) { best_gap = gap; best_cut = cut; }
    }

    if (best_cut > 0) {
        r.score = best_gap / range_;     // normalized change magnitude
        r.alarm = true;
        // Adapt: drop the stale older sub-window, keep the newer regime.
        for (size_t i = 0; i < best_cut; ++i) window_.pop_front();
    }
    return r;
}

}  // namespace driftmon
