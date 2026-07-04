// test_quality.cpp — data-quality channel (v2): NaN counting through the real
// tap, NaN-ratio alarm, constant-feature detection, out-of-range flag, and the
// key separation property: a NaN flood must NOT fake drift.
#include "test_framework.h"

#include <cmath>
#include <fstream>
#include <string>
#include <vector>

#include "driftmon/tap.h"
#include "arena_rt.h"
#include "bundle.h"
#include "worker.h"

using namespace driftmon;

// Uniform reference over 4 interior bins; min_samples low so one tick verdicts.
static const char* kJson = R"({"model_id":"qual","window":{"min_samples":100,"max_seconds":600},
  "quality":{"nan_ratio_max":0.05,"oor_ratio_max":0.10},
  "features":[{"name":"f","bin_edges":[0,1,2,3,4],"ref_hist":[100,100,100,100]}]})";

static Bundle bundle() {
    Bundle b; std::string err;
    parse_bundle(kJson, b, err);
    return b;
}

// Inject physical-bin counts directly (as the tap would over many calls).
static void inject(SlotHeader* s, const std::vector<uint64_t>& phys) {
    const uint32_t idx = writer_acquire(s);
    uint64_t n = 0;
    for (uint32_t k = 0; k < phys.size(); ++k) {
        hist_buffer(s, idx)[s->bin_offset[0] + k].fetch_add(phys[k], std::memory_order_relaxed);
        n += phys[k];
    }
    s->sample_count[idx].fetch_add(n, std::memory_order_relaxed);
    writer_release(s, idx);
}

TEST(nan_counted_via_real_tap) {
    Bundle b = bundle();
    std::string err;
    ModelMonitor mon;
    CHECK(mon.init(b, err));

    std::ofstream("qual_bundle.json") << kJson;
    CHECK(tap_init("qual", "qual_bundle.json"));
    float nan = std::nanf("");
    float ok = 1.5f;
    tap_update_input(&nan, 1);
    tap_update_input(&ok, 1);

    std::vector<Histogram> frozen;
    CHECK(slot_swap_read(mon.slot(), frozen) == 2);
    // physical: [under, b1..b4, over, nan] — NaN landed in the last bin.
    CHECK(frozen[0].counts.size() == 7);
    CHECK(frozen[0].counts[6] == 1);
    CHECK(frozen[0].counts[2] == 1);   // 1.5 → interior bin 2
    tap_shutdown();
}

TEST(nan_flood_raises_quality_not_drift) {
    Bundle b = bundle();
    std::string err;
    ModelMonitor mon;
    CHECK(mon.init(b, err));

    // 400 well-distributed values (matching the reference) + 100 NaNs (20%).
    inject(mon.slot(), {0, 100, 100, 100, 100, 0, 100});
    ModelVerdict v = mon.tick(1.0);
    CHECK(v.produced);
    CHECK(v.quality_alarm);                    // 20% NaN >> 5% threshold
    CHECK_NEAR(v.quality[0].nan_ratio, 0.2, 1e-9);
    CHECK(!v.alarm);                           // distribution itself matches ⇒ NO drift
    CHECK(v.max_score < 0.1);                  // NaN mass excluded from PSI/KS
    CHECK(v.kind == "data_quality");           // R2: fix the pipeline, don't retrain
}

TEST(constant_feature_detected) {
    Bundle b = bundle();
    std::string err;
    ModelMonitor mon;
    CHECK(mon.init(b, err));

    // Upstream stuck: every sample lands in one bin (ref was uniform).
    inject(mon.slot(), {0, 0, 500, 0, 0, 0, 0});
    ModelVerdict v = mon.tick(1.0);
    CHECK(v.produced);
    CHECK(v.quality[0].constant);
    CHECK(v.quality_alarm);
}

TEST(out_of_range_flag_without_quality_alarm) {
    Bundle b = bundle();
    std::string err;
    ModelMonitor mon;
    CHECK(mon.init(b, err));

    // 30% of mass escapes the edges — a DRIFT-kind signal (out_of_range flag),
    // not a pipeline-quality alarm.
    inject(mon.slot(), {150, 100, 100, 100, 50, 0, 0});
    ModelVerdict v = mon.tick(1.0);
    CHECK(v.produced);
    CHECK(v.quality[0].out_of_range);
    CHECK(v.quality[0].oor_ratio > 0.10);
    CHECK(!v.quality[0].alarm);        // no NaN, not constant ⇒ quality clean
    CHECK(v.alarm);                    // but PSI sees the escaped mass as drift
}

TEST(boundary_mass_is_not_constant) {
    // Codex review fix: ~all mass in a BOUNDARY bin is regime escape, not a
    // stuck feature. It must route as out_of_range (retrain + re-bin), never
    // as data_quality (pipeline repair, retraining forbidden), which would win
    // by kind precedence.
    Bundle b = bundle();
    std::string err;
    ModelMonitor mon;
    CHECK(mon.init(b, err));

    inject(mon.slot(), {0, 0, 0, 0, 0, 500, 0});   // 100% overflow
    ModelVerdict v = mon.tick(1.0);
    CHECK(v.produced);
    CHECK(!v.quality[0].constant);     // boundary concentration ≠ constant
    CHECK(!v.quality_alarm);
    CHECK(v.quality[0].out_of_range);
    CHECK(v.alarm);
    CHECK(v.kind == "out_of_range");   // correctly routed to retrain-and-rebin
}

TEST(clean_window_no_quality_signals) {
    Bundle b = bundle();
    std::string err;
    ModelMonitor mon;
    CHECK(mon.init(b, err));

    inject(mon.slot(), {0, 100, 100, 100, 100, 0, 0});
    ModelVerdict v = mon.tick(1.0);
    CHECK(v.produced);
    CHECK(!v.quality_alarm);
    CHECK(!v.quality[0].out_of_range);
    CHECK(!v.quality[0].constant);
    CHECK_NEAR(v.quality[0].nan_ratio, 0.0, 1e-12);
    CHECK(!v.alarm);
}

int main() { return RUN_ALL(); }
