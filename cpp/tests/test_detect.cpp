// test_detect.cpp — true-positive drift (AC3) and false-positive suppression (AC4).
#include "test_framework.h"

#include <string>
#include <vector>

#include "arena_rt.h"
#include "bundle.h"
#include "worker.h"

using namespace driftmon;

// Inject physical-bin counts for feature `f` straight into the active buffer
// (one writer critical section), as a serving tap would over many calls.
static void inject(SlotHeader* s, uint32_t f, const std::vector<uint64_t>& phys) {
    const uint32_t idx = writer_acquire(s);
    uint64_t n = 0;
    for (uint32_t k = 0; k < phys.size(); ++k) {
        hist_buffer(s, idx)[s->bin_offset[f] + k].fetch_add(phys[k], std::memory_order_relaxed);
        n += phys[k];
    }
    s->sample_count[idx].fetch_add(n, std::memory_order_relaxed);
    writer_release(s, idx);
}

// Uniform reference over 4 interior bins; PSI+KS both active; min_samples 1000.
static Bundle bundle(const char* id) {
    std::string j = std::string(R"({"model_id":")") + id + R"(",
      "window":{"min_samples":1000,"max_seconds":600},
      "tests":["psi","ks"],
      "features":[{"name":"f","bin_edges":[0,1,2,3,4],"ref_hist":[100,100,100,100]}]})";
    Bundle b; std::string err;
    parse_bundle(j, b, err);
    return b;
}

TEST(true_positive_on_shift) {     // AC3
    Bundle b = bundle("tp"); std::string err;
    ModelMonitor mon; CHECK(mon.init(b, err));
    // Mass piles into the low bins (70/30) — a gross in-range distribution
    // shift, but spread over two bins so it is NOT a constant-feature case.
    inject(mon.slot(), 0, {0, 700, 300, 0, 0, 0});
    ModelVerdict v = mon.tick(/*elapsed=*/1.0);
    CHECK(v.produced);
    CHECK(v.alarm);                // PSI ≥ 0.2 and/or KS ≥ 0.1
    CHECK(v.max_score > 0.2);
    // In-range shape shift ⇒ classified as a distribution change (R2).
    CHECK(v.kind == "distribution");
}

TEST(false_positive_suppressed_on_match) {    // AC4
    Bundle b = bundle("fp"); std::string err;
    ModelMonitor mon; CHECK(mon.init(b, err));
    // Distribution matching the reference ratios (250 each interior bin).
    inject(mon.slot(), 0, {0, 250, 250, 250, 250, 0});
    ModelVerdict v = mon.tick(1.0);
    CHECK(v.produced);
    CHECK(!v.alarm);               // no spurious drift on the reference distribution
    CHECK(v.max_score < 0.1);
    CHECK(v.kind == "none");
}

TEST(out_of_range_mass_detected) {     // AC3 variant: boundary escape is drift
    Bundle b = bundle("oor"); std::string err;
    ModelMonitor mon; CHECK(mon.init(b, err));
    // Half the mass lands in the overflow bin (ref overflow ratio is 0).
    inject(mon.slot(), 0, {0, 125, 125, 125, 125, 500});
    ModelVerdict v = mon.tick(1.0);
    CHECK(v.produced);
    CHECK(v.alarm);
    // Regime escaped the reference edges ⇒ retrain AND re-bin (R2).
    CHECK(v.kind == "out_of_range");
}

TEST(sustained_kind_from_streaming_detector) {    // R2: fine-tuning candidate
    // CUSUM only: a persistent mean shift inside the bin range.
    const char* j = R"({"model_id":"sus","window":{"min_samples":100,"max_seconds":600},
      "tests":["cusum"],
      "features":[{"name":"f","bin_edges":[0,1,2,3,4],"ref_hist":[100,100,100,100]}]})";
    Bundle b; std::string err;
    CHECK(parse_bundle(j, b, err));
    ModelMonitor mon; CHECK(mon.init(b, err));

    // Repeated windows all leaning toward the top bins (spread over two bins,
    // so not a constant-feature case): CUSUM accumulates the mean shift.
    std::string kind;
    for (int i = 0; i < 20 && kind != "sustained"; ++i) {
        inject(mon.slot(), 0, {0, 0, 0, 100, 300, 0});
        ModelVerdict v = mon.tick(1.0);
        if (v.produced && v.alarm) kind = v.kind;
    }
    CHECK(kind == "sustained");
}

int main() { return RUN_ALL(); }
