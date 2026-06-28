// test_tap_noop.cpp — degrade-to-no-op (R1.4) + positive single-model path (AC2).
#include "test_framework.h"

#include <cmath>
#include <fstream>
#include <string>
#include <vector>

#include "driftmon/tap.h"
#include "bundle.h"
#include "worker.h"

using namespace driftmon;

static const char* kBundlePath = "tap_noop_bundle.json";
static const char* kBundleJson = R"({"model_id":"tapm","window":{"min_samples":1000,"max_seconds":60},
  "features":[{"name":"f","bin_edges":[0,1,2,3,4],"ref_hist":[10,20,30,40]}]})";

static void write_bundle() {
    std::ofstream(kBundlePath) << kBundleJson;
}

TEST(noop_when_init_fails) {
    tap_shutdown();                               // safe even if never inited
    CHECK(!tap_init(nullptr, nullptr));
    CHECK(!tap_init("m", "/no/such/bundle.json"));  // bundle load fails (gate)

    write_bundle();
    CHECK(!tap_init("wrong_id", kBundlePath));      // model_id mismatch
    CHECK(!tap_init("tapm", kBundlePath));          // valid bundle, but no worker arena up

    // After every failed init, the hot path must be a safe no-op (no crash).
    float f[3] = {1.0f, 2.0f, 3.0f};
    tap_update_input(f, 3);
    tap_update_output(f, 3);
    tap_update_input(nullptr, 3);
    tap_shutdown();
}

TEST(positive_single_model_accumulation) {
    write_bundle();
    Bundle b; std::string err;
    CHECK(parse_bundle(kBundleJson, b, err));

    ModelMonitor mon;                              // worker side: owns the arena
    CHECK(mon.init(b, err));

    CHECK(tap_init("tapm", kBundlePath));          // now attach succeeds

    // 5 valid samples in [2,3) ⇒ interior physical bin 3; 1 NaN (skipped, but
    // still counts as a sample); 1 underflow; 1 overflow.
    float v = 2.5f;
    for (int i = 0; i < 5; ++i) tap_update_input(&v, 1);
    float nan = std::nanf("");
    tap_update_input(&nan, 1);
    float under = -1.0f, over = 99.0f;
    tap_update_input(&under, 1);
    tap_update_input(&over, 1);

    std::vector<Histogram> frozen;
    uint64_t samples = slot_swap_read(mon.slot(), frozen);

    CHECK(samples == 8);                           // 8 tap calls (NaN included)
    CHECK(frozen.size() == 1);
    CHECK(frozen[0].counts.size() == 6);           // underflow + 4 interior + overflow
    CHECK(frozen[0].counts[0] == 1);               // underflow (-1)
    CHECK(frozen[0].counts[3] == 5);               // [2,3) bin (2.5 × 5)
    CHECK(frozen[0].counts[5] == 1);               // overflow (99)
    CHECK(frozen[0].total == 7);                   // 7 binned (NaN not binned)

    tap_shutdown();
}

int main() { return RUN_ALL(); }
