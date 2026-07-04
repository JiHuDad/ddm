// test_debounce.cpp — bundle-driven severity + anti-flapping debounce (P4).
#include "test_framework.h"

#include <string>
#include <vector>

#include "arena_rt.h"
#include "bundle.h"
#include "worker.h"

using namespace driftmon;

TEST(debouncer_pure_logic) {
    // Defaults: up=1 (raise immediately), down=2 (need 2 clean windows).
    Debouncer d(1, 2);
    CHECK(d.update(2) == 2);       // raise on first alarm
    CHECK(d.update(0) == 2);       // 1 clean window: hold
    CHECK(d.update(0) == 0);       // 2nd clean window: release
    // Flapping alarm/clean/alarm/clean reports STEADY alarm — no storm.
    CHECK(d.update(2) == 2);
    CHECK(d.update(0) == 2);
    CHECK(d.update(2) == 2);
    CHECK(d.update(0) == 2);

    // up=2: one spike is NOT enough to raise.
    Debouncer slow(2, 1);
    CHECK(slow.update(2) == 0);    // pending
    CHECK(slow.update(0) == 0);    // streak broken
    CHECK(slow.update(2) == 0);
    CHECK(slow.update(2) == 2);    // 2 consecutive: raised
    CHECK(slow.update(1) == 1);    // down=1: lowers immediately (to raw)
}

// Uniform reference; psi_threshold 0.2, warn_ratio 0.5 ⇒ WARNING at PSI >= 0.1.
static Bundle bundle(int up, int down) {
    std::string j = std::string(R"({"model_id":"deb","window":{"min_samples":100,"max_seconds":600},
      "alarm":{"warn_ratio":0.5,"up_windows":)") + std::to_string(up) +
      R"(,"down_windows":)" + std::to_string(down) + R"(},
      "features":[{"name":"f","bin_edges":[0,1,2,3,4],"ref_hist":[100,100,100,100]}]})";
    Bundle b; std::string err;
    parse_bundle(j, b, err);
    return b;
}

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

TEST(severity_levels_from_bundle_thresholds) {
    Bundle b = bundle(1, 2);
    std::string err;
    ModelMonitor mon;
    CHECK(mon.init(b, err));

    // Matching distribution ⇒ severity 0.
    inject(mon.slot(), {0, 100, 100, 100, 100, 0, 0});
    ModelVerdict v0 = mon.tick(1.0);
    CHECK(v0.produced);
    CHECK(v0.severity == 0);
    CHECK(!v0.alarm);

    // Mild skew: PSI in [0.1, 0.2) ⇒ WARNING (1), no alarm.
    inject(mon.slot(), {0, 160, 110, 80, 50, 0, 0});
    ModelVerdict v1 = mon.tick(1.0);
    CHECK(v1.produced);
    CHECK(v1.max_score >= 0.1);
    CHECK(v1.max_score < 0.2);
    CHECK(v1.severity == 1);
    CHECK(!v1.alarm);

    // Gross shift ⇒ SIGNIFICANT (2) + alarm.
    inject(mon.slot(), {0, 400, 0, 0, 0, 0, 0});
    ModelVerdict v2 = mon.tick(1.0);
    CHECK(v2.produced);
    CHECK(v2.severity == 2);
    CHECK(v2.alarm);
}

TEST(debounce_suppresses_flapping_verdicts) {
    Bundle b = bundle(1, 2);   // raise fast, release slow
    std::string err;
    ModelMonitor mon;
    CHECK(mon.init(b, err));

    auto drift  = std::vector<uint64_t>{0, 400, 0, 0, 0, 0, 0};
    auto normal = std::vector<uint64_t>{0, 100, 100, 100, 100, 0, 0};

    inject(mon.slot(), drift);
    CHECK(mon.tick(1.0).severity == 2);      // raised immediately (up=1)
    inject(mon.slot(), normal);
    CHECK(mon.tick(1.0).severity == 2);      // 1 clean window: HELD (down=2)
    inject(mon.slot(), drift);
    CHECK(mon.tick(1.0).severity == 2);      // flapping reports steady alarm
    inject(mon.slot(), normal);
    CHECK(mon.tick(1.0).severity == 2);
    inject(mon.slot(), normal);
    CHECK(mon.tick(1.0).severity == 0);      // 2 consecutive clean: released
}

TEST(slow_raise_needs_consecutive_alarms) {
    Bundle b = bundle(2, 1);   // up=2: single-window spikes are ignored
    std::string err;
    ModelMonitor mon;
    CHECK(mon.init(b, err));

    auto drift  = std::vector<uint64_t>{0, 400, 0, 0, 0, 0, 0};
    auto normal = std::vector<uint64_t>{0, 100, 100, 100, 100, 0, 0};

    inject(mon.slot(), drift);
    ModelVerdict v = mon.tick(1.0);
    CHECK(v.severity == 0);                  // one spike: not reported yet
    CHECK(!v.alarm);
    inject(mon.slot(), drift);
    CHECK(mon.tick(1.0).severity == 2);      // sustained: reported
    inject(mon.slot(), normal);
    CHECK(mon.tick(1.0).severity == 0);      // down=1: releases immediately
}

int main() { return RUN_ALL(); }
