// test_ks_detector.cpp — KS statistic over physical-bin histograms (AC10).
#include "test_framework.h"

#include <string>

#include "bundle.h"
#include "ks_detector.h"
#include "worker.h"

using namespace driftmon;

static Bundle uniform_bundle() {
    const char* j = R"({"model_id":"m","features":[
      {"name":"f","bin_edges":[0,1,2,3,4],"ref_hist":[100,100,100,100],"ks_threshold":0.1}]})";
    Bundle b; std::string err;
    parse_bundle(j, b, err);
    return b;
}

static Histogram hist(std::vector<uint64_t> c) {
    Histogram h; h.counts = std::move(c);
    for (uint64_t x : h.counts) h.total += x;
    return h;
}

TEST(ks_zero_on_same_distribution) {
    KsDetector d; d.configure(feature_refs_from_bundle(uniform_bundle())[0]);
    DriftResult r = d.eval(hist({0, 25, 25, 25, 25, 0}));   // matches ref ratios
    CHECK_NEAR(r.score, 0.0, 1e-9);
    CHECK(!r.alarm);
}

TEST(ks_known_value_on_shift) {
    // Reference CDF over interior bins: .25/.5/.75/1.0. All mass in first bin ⇒
    // actual CDF jumps to 1 immediately; max gap = |1 - .25| = .75.
    KsDetector d;
    FeatureRef ref = feature_refs_from_bundle(uniform_bundle())[0];
    ref.threshold = 0.1;
    d.configure(ref);
    DriftResult r = d.eval(hist({0, 100, 0, 0, 0, 0}));
    CHECK_NEAR(r.score, 0.75, 1e-9);
    CHECK(r.alarm);
}

TEST(ks_threshold_boundary) {
    FeatureRef ref = feature_refs_from_bundle(uniform_bundle())[0];
    ref.threshold = 0.9;   // above the .75 gap ⇒ no alarm
    KsDetector d; d.configure(ref);
    DriftResult r = d.eval(hist({0, 100, 0, 0, 0, 0}));
    CHECK(r.score < ref.threshold);
    CHECK(!r.alarm);
}

int main() { return RUN_ALL(); }
