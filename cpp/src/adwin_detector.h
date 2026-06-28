/*
 * adwin_detector.h — simplified ADWIN streaming drift detector (SPEC §5.3 R3.4).
 *
 * Keeps a bounded window of the recent per-window mean-bin signals. At each
 * step it looks for a split into an older and a newer sub-window whose means
 * differ by more than a Hoeffding bound (confidence delta); if found, it flags
 * drift and drops the older sub-window (adapts to the new regime). This is a
 * compact, testable variant of ADWIN — not the full exponential-histogram
 * algorithm — sufficient for tier-1 early warning. State persists across eval().
 */
#ifndef DRIFTMON_ADWIN_DETECTOR_H
#define DRIFTMON_ADWIN_DETECTOR_H

#include <deque>

#include "detector.h"

namespace driftmon {

inline constexpr double DRIFTMON_ADWIN_DELTA = 0.05;     // confidence
inline constexpr size_t DRIFTMON_ADWIN_MAXWIN = 64;      // bounded memory

class AdwinDetector : public Detector {
public:
    explicit AdwinDetector(double delta = DRIFTMON_ADWIN_DELTA,
                           size_t max_window = DRIFTMON_ADWIN_MAXWIN)
        : delta_(delta), max_window_(max_window) {}

    void        configure(const FeatureRef& ref) override;   // sets value range
    DriftResult eval(const Histogram& current) override;      // streaming update

    void reset() { window_.clear(); }

private:
    std::deque<double> window_;   // recent mean-bin signals
    double delta_;
    size_t max_window_;
    double range_ = 1.0;          // bin-index span, for the Hoeffding bound scale
};

}  // namespace driftmon

#endif  // DRIFTMON_ADWIN_DETECTOR_H
