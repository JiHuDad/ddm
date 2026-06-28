// test_warmup_guard.cpp — warm-up guard (AC5): withhold verdict below min_samples.
#include "test_framework.h"

#include <string>

#include "arena_rt.h"
#include "bundle.h"
#include "worker.h"

using namespace driftmon;

TEST(window_decision_pure) {
    // Below min, time left ⇒ keep accumulating.
    CHECK(window_decision(500, 1.0, 1000, 60) == WindowDecision::Accumulate);
    // Reached min ⇒ evaluate (regardless of time).
    CHECK(window_decision(1000, 1.0, 1000, 60) == WindowDecision::Evaluate);
    CHECK(window_decision(1000, 999.0, 1000, 60) == WindowDecision::Evaluate);
    // Time closed the window but still below min ⇒ WITHHOLD (warm-up).
    CHECK(window_decision(500, 61.0, 1000, 60) == WindowDecision::WarmupClose);
}

// Inject `n` synthetic samples straight into the slot's active buffer using the
// real writer drain-guard (one interior bin), as the tap would.
static void inject_samples(SlotHeader* s, int n) {
    for (int i = 0; i < n; ++i) {
        const uint32_t idx = writer_acquire(s);
        hist_buffer(s, idx)[s->bin_offset[0] + 1].fetch_add(1, std::memory_order_relaxed);
        s->sample_count[idx].fetch_add(1, std::memory_order_relaxed);
        writer_release(s, idx);
    }
}

TEST(warmup_guard_integration) {
    const char* j = R"({"model_id":"warm","window":{"min_samples":50,"max_seconds":10},
      "features":[{"name":"f","bin_edges":[0,1,2,3,4],"ref_hist":[10,20,30,40]}]})";
    Bundle b; std::string err;
    CHECK(parse_bundle(j, b, err));

    ModelMonitor mon;
    CHECK(mon.init(b, err));
    SlotHeader* s = mon.slot();

    // 30 samples, time not elapsed ⇒ still accumulating, verdict withheld.
    inject_samples(s, 30);
    ModelVerdict v1 = mon.tick(/*elapsed=*/1.0);
    CHECK(!v1.produced);
    CHECK(!v1.warming_up);
    CHECK(v1.window_samples == 30);

    // 30 more (accum 60 ≥ 50) ⇒ window closes by count, verdict produced.
    inject_samples(s, 30);
    ModelVerdict v2 = mon.tick(1.0);
    CHECK(v2.produced);
    CHECK(v2.window_samples == 60);

    // New window: few samples but time exceeds max_seconds ⇒ warm-up close,
    // verdict still withheld (G5/AC5: time can close a window, min gates verdict).
    inject_samples(s, 10);
    ModelVerdict v3 = mon.tick(/*elapsed=*/11.0);
    CHECK(!v3.produced);
    CHECK(v3.warming_up);
    CHECK(v3.window_samples == 10);
}

int main() { return RUN_ALL(); }
