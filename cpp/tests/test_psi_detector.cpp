// test_psi_detector.cpp — PSI numeric behaviour + physical-bin reference build.
#include "test_framework.h"

#include <string>

#include "bundle.h"
#include "psi_detector.h"
#include "worker.h"

using namespace driftmon;

// One feature, interior bins [10,20,30,40] over edges; physical bins add
// underflow (idx 0) and overflow (idx 5).
static Bundle one_feature_bundle() {
    const char* j = R"({"model_id":"m","features":[
      {"name":"f","bin_edges":[0,1,2,3,4],"ref_hist":[10,20,30,40]}]})";
    Bundle b; std::string err;
    parse_bundle(j, b, err);
    return b;
}

TEST(ref_ratios_physical_layout) {
    Bundle b = one_feature_bundle();
    auto refs = feature_refs_from_bundle(b);
    CHECK(refs.size() == 1);
    // physical: [underflow, .1, .2, .3, .4, overflow]
    CHECK(refs[0].ref_ratios.size() == 6);
    CHECK_NEAR(refs[0].ref_ratios[0], 0.0, 1e-12);   // underflow = 0
    CHECK_NEAR(refs[0].ref_ratios[1], 0.1, 1e-12);
    CHECK_NEAR(refs[0].ref_ratios[4], 0.4, 1e-12);
    CHECK_NEAR(refs[0].ref_ratios[5], 0.0, 1e-12);   // overflow = 0
}

static Histogram hist(std::vector<uint64_t> c) {
    Histogram h; h.counts = std::move(c);
    for (uint64_t x : h.counts) h.total += x;
    return h;
}

TEST(psi_zero_on_same_distribution) {
    Bundle b = one_feature_bundle();
    PsiDetector d; d.configure(feature_refs_from_bundle(b)[0]);
    DriftResult r = d.eval(hist({0, 10, 20, 30, 40, 0}));   // identical shape
    CHECK_NEAR(r.score, 0.0, 1e-9);
    CHECK(!r.alarm);
}

TEST(psi_alarms_on_shift) {
    Bundle b = one_feature_bundle();
    PsiDetector d; d.configure(feature_refs_from_bundle(b)[0]);   // threshold 0.2
    DriftResult r = d.eval(hist({0, 40, 30, 20, 10, 0}));         // reversed
    CHECK(r.score > 0.2);
    CHECK(r.alarm);
}

TEST(psi_out_of_range_mass_is_drift) {
    Bundle b = one_feature_bundle();
    PsiDetector d; d.configure(feature_refs_from_bundle(b)[0]);
    // Half the mass escapes into overflow (ref overflow ratio is 0) ⇒ strong drift.
    DriftResult r = d.eval(hist({0, 5, 10, 15, 20, 50}));
    CHECK(r.score > 0.2);
    CHECK(r.alarm);
}

TEST(psi_zero_observations_no_drift) {
    Bundle b = one_feature_bundle();
    PsiDetector d; d.configure(feature_refs_from_bundle(b)[0]);
    DriftResult r = d.eval(hist({0, 0, 0, 0, 0, 0}));   // empty window
    CHECK_NEAR(r.score, 0.0, 1e-12);
    CHECK(!r.alarm);
}

int main() { return RUN_ALL(); }
