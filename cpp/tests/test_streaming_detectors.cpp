// test_streaming_detectors.cpp — CUSUM + ADWIN streaming change detection (AC10).
#include "test_framework.h"

#include <string>
#include <vector>

#include "adwin_detector.h"
#include "bundle.h"
#include "cusum_detector.h"
#include "worker.h"

using namespace driftmon;

// Build a single-feature histogram with all mass in one interior physical bin.
static Histogram at_bin(uint32_t phys_bin, uint32_t nphys, uint64_t n) {
    Histogram h;
    h.counts.assign(nphys, 0);
    h.counts[phys_bin] = n;
    h.total = n;
    return h;
}

static FeatureRef ref8() {
    // 6 interior bins ⇒ 8 physical bins; uniform reference (mean bin ~3.5).
    const char* j = R"({"model_id":"m","features":[
      {"name":"f","bin_edges":[0,1,2,3,4,5,6],"ref_hist":[1,1,1,1,1,1]}]})";
    Bundle b; std::string err; parse_bundle(j, b, err);
    return feature_refs_from_bundle(b)[0];
}

TEST(cusum_detects_sustained_shift) {
    CusumDetector d;
    d.configure(ref8());   // target = reference mean bin

    // Stable phase: feed windows centered on the reference mean ⇒ no alarm.
    bool stable_alarm = false;
    for (int i = 0; i < 20; ++i)
        if (d.eval(at_bin(3, 8, 1000)).alarm || d.eval(at_bin(4, 8, 1000)).alarm)
            stable_alarm = true;
    CHECK(!stable_alarm);

    // Shift phase: mass jumps to a far bin ⇒ CUSUM accumulates and alarms.
    bool shifted_alarm = false;
    for (int i = 0; i < 20 && !shifted_alarm; ++i)
        shifted_alarm = d.eval(at_bin(7, 8, 1000)).alarm;
    CHECK(shifted_alarm);
}

TEST(adwin_detects_regime_change) {
    AdwinDetector d;
    d.configure(ref8());

    // Stable regime around bin 3–4: no change flagged.
    bool stable_alarm = false;
    for (int i = 0; i < 16; ++i)
        if (d.eval(at_bin(i % 2 ? 3 : 4, 8, 1000)).alarm) stable_alarm = true;
    CHECK(!stable_alarm);

    // Abrupt regime change to bin 0: ADWIN should detect the split.
    bool changed = false;
    for (int i = 0; i < 16 && !changed; ++i)
        changed = d.eval(at_bin(0, 8, 1000)).alarm;
    CHECK(changed);
}

TEST(cusum_reset_after_alarm) {
    CusumDetector d;
    d.configure(ref8());
    bool alarmed = false;
    for (int i = 0; i < 20 && !alarmed; ++i) alarmed = d.eval(at_bin(7, 8, 1000)).alarm;
    CHECK(alarmed);
    // Right after an alarm the accumulators reset, so a single matching window
    // does not immediately re-alarm.
    DriftResult r = d.eval(at_bin(4, 8, 1000));
    CHECK(!r.alarm);
}

int main() { return RUN_ALL(); }
