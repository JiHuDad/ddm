// test_tap_reconnect.cpp — tap self-heal (A1): late worker, degraded
// auto-retry, and stale-arena re-attach after a worker rebuild.
#include "test_framework.h"

#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "driftmon/tap.h"
#include "bundle.h"
#include "tap_state.h"
#include "worker.h"

using namespace driftmon;

static const char* kBundlePath = "reconnect_bundle.json";
static const char* kBundleJson = R"({"model_id":"reconn","window":{"min_samples":100000,"max_seconds":100000},
  "features":[{"name":"f","bin_edges":[0,1,2,3,4],"ref_hist":[10,20,30,40]}]})";

static Bundle write_and_parse_bundle() {
    std::ofstream(kBundlePath) << kBundleJson;
    Bundle b; std::string err;
    parse_bundle(kBundleJson, b, err);
    return b;
}

TEST(late_worker_recovery_via_maintain) {
    Bundle b = write_and_parse_bundle();
    arena_unlink(arena_name(b.model_id));   // ensure no arena exists

    // Serving starts FIRST — init fails, tap degrades to no-op.
    CHECK(!tap_init("reconn", kBundlePath));
    CHECK(!tap_maintain());                 // still no worker

    // Worker comes up late.
    ModelMonitor mon;
    std::string err;
    CHECK(mon.init(b, err));

    // Explicit maintenance recovers the tap; samples flow again.
    CHECK(tap_maintain());
    float v = 2.5f;
    tap_update_input(&v, 1);
    std::vector<Histogram> frozen;
    CHECK(slot_swap_read(mon.slot(), frozen) == 1);
    CHECK(frozen[0].counts[3] == 1);        // 2.5 → interior bin 3

    tap_shutdown();
}

TEST(degraded_auto_retry_from_hot_path) {
    Bundle b = write_and_parse_bundle();
    arena_unlink(arena_name(b.model_id));

    CHECK(!tap_init("reconn", kBundlePath));   // degraded

    ModelMonitor mon;                          // worker comes up late
    std::string err;
    CHECK(mon.init(b, err));

    // Lower the retry interval so the test doesn't need 2^20 calls, then hit
    // the hot path in no-op mode until the automatic retry fires.
    detail::g_tap_retry_calls = 4;
    detail::g_tap.noop_calls.store(0);
    float v = 2.5f;
    for (int i = 0; i < 8; ++i) tap_update_input(&v, 1);
    detail::g_tap_retry_calls = 1u << 20;      // restore for other tests

    CHECK(detail::g_tap.live.load());          // self-healed without tap_maintain()
    tap_update_input(&v, 1);
    std::vector<Histogram> frozen;
    CHECK(slot_swap_read(mon.slot(), frozen) >= 1);   // post-recovery sample arrived

    tap_shutdown();
}

TEST(stale_arena_reattach_after_worker_rebuild) {
    Bundle b = write_and_parse_bundle();
    arena_unlink(arena_name(b.model_id));

    auto mon1 = std::make_unique<ModelMonitor>();
    std::string err;
    CHECK(mon1->init(b, err));
    CHECK(tap_init("reconn", kBundlePath));    // healthy tap
    CHECK(tap_maintain());                     // healthy ⇒ no-op, stays live

    // Worker rebuilds its arena (bundle update path): old shm unlinked, new one
    // created. The tap's mapping is now a ghost.
    mon1.reset();
    auto mon2 = std::make_unique<ModelMonitor>();
    CHECK(mon2->init(b, err));

    float v = 2.5f;
    tap_update_input(&v, 1);                   // lands in the ghost — lost, harmless
    std::vector<Histogram> frozen;
    CHECK(slot_swap_read(mon2->slot(), frozen) == 0);   // new arena saw nothing

    CHECK(tap_maintain());                     // detects nlink==0 → re-attaches
    tap_update_input(&v, 1);
    CHECK(slot_swap_read(mon2->slot(), frozen) == 1);   // flowing into the NEW arena
    CHECK(frozen[0].counts[3] == 1);

    tap_shutdown();
}

int main() { return RUN_ALL(); }
