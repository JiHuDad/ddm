// test_sample_ring.cpp — raw-input sample ring (v2, R-R1): sampling rate,
// ring wraparound (most-recent wins), content fidelity, disabled path.
#include "test_framework.h"

#include <fstream>
#include <string>
#include <vector>

#include "driftmon/tap.h"
#include "bundle.h"
#include "tap_state.h"
#include "worker.h"

using namespace driftmon;

static const char* kRingJson = R"({"model_id":"ring","window":{"min_samples":100000,"max_seconds":100000},
  "sampling":{"every_n":2,"ring_rows":4},
  "features":[{"name":"a","bin_edges":[0,1,2,3,4],"ref_hist":[1,1,1,1]},
              {"name":"b","bin_edges":[0,10,20,30,40],"ref_hist":[1,1,1,1]}]})";

static Bundle setup(const char* json, const char* path) {
    std::ofstream(path) << json;
    Bundle b; std::string err;
    parse_bundle(json, b, err);
    return b;
}

TEST(ring_samples_every_n_and_wraps) {
    Bundle b = setup(kRingJson, "ring_bundle.json");
    std::string err;
    ModelMonitor mon;
    CHECK(mon.init(b, err));
    CHECK(mon.slot()->ring_rows == 4);
    CHECK(mon.slot()->sample_every == 2);
    CHECK(mon.slot()->n_inputs == 2);

    CHECK(tap_init("ring", "ring_bundle.json"));
    detail::g_tap.sample_ctr.store(0);   // deterministic sampling phase

    // 12 distinct vectors; every_n=2 ⇒ vectors 0,2,4,6,8,10 sampled (6 claims);
    // ring holds 4 rows ⇒ survivors are vectors 4,6,8,10 (most recent 4).
    for (int i = 0; i < 12; ++i) {
        float v[2] = {static_cast<float>(i), static_cast<float>(i * 10)};
        tap_update_input(v, 2);
    }

    CHECK(mon.slot()->ring_head.load() == 6);
    auto rows = read_ring(mon.slot());
    CHECK(rows.size() == 4);
    CHECK(rows[0].size() == 2);
    CHECK_NEAR(rows[0][0], 4.0, 1e-6);     // oldest surviving claim
    CHECK_NEAR(rows[0][1], 40.0, 1e-6);
    CHECK_NEAR(rows[3][0], 10.0, 1e-6);    // newest
    CHECK_NEAR(rows[3][1], 100.0, 1e-6);

    tap_shutdown();
}

TEST(ring_disabled_is_a_noop) {
    const char* j = R"({"model_id":"ring0","window":{"min_samples":100000,"max_seconds":100000},
      "sampling":{"ring_rows":0},
      "features":[{"name":"a","bin_edges":[0,1,2,3,4],"ref_hist":[1,1,1,1]}]})";
    Bundle b = setup(j, "ring0_bundle.json");
    std::string err;
    ModelMonitor mon;
    CHECK(mon.init(b, err));
    CHECK(mon.slot()->ring_rows == 0);

    CHECK(tap_init("ring0", "ring0_bundle.json"));
    float v = 1.5f;
    for (int i = 0; i < 10; ++i) tap_update_input(&v, 1);

    CHECK(mon.slot()->ring_head.load() == 0);
    CHECK(read_ring(mon.slot()).empty());
    // Histograms still accumulate normally with the ring off.
    std::vector<Histogram> frozen;
    CHECK(slot_swap_read(mon.slot(), frozen) == 10);
    tap_shutdown();
}

TEST(stale_claim_not_accepted) {
    // Codex review fix: a writer bumps ring_head BEFORE marking its row odd.
    // If the reader runs in that window, the claimed-but-unwritten row still
    // holds the previous lap's even rowseq — it must be rejected (rowseq must
    // equal 2k+2 for THIS claim), not returned as fresh data.
    Bundle b = setup(kRingJson, "ring_bundle.json");
    std::string err;
    ModelMonitor mon;
    CHECK(mon.init(b, err));
    CHECK(tap_init("ring", "ring_bundle.json"));
    detail::g_tap.sample_ctr.store(0);

    // Fill exactly one lap: claims 0..3 land in rows 0..3 (every_n=2 ⇒ 8 feeds).
    for (int i = 0; i < 8; ++i) {
        float v[2] = {static_cast<float>(i), static_cast<float>(i)};
        tap_update_input(v, 2);
    }
    CHECK(mon.slot()->ring_head.load() == 4);
    CHECK(read_ring(mon.slot()).size() == 4);

    // Simulate a writer paused between claiming (head++) and writing: claim
    // k=4 targets row 0, whose rowseq is still 2 (claim 0's completed value).
    mon.slot()->ring_head.fetch_add(1);
    auto rows = read_ring(mon.slot());
    CHECK(rows.size() == 3);               // claims 1..3 only — stale row 0 rejected
    CHECK_NEAR(rows[0][0], 2.0, 1e-6);     // oldest surviving = claim 1 (vector #2)

    tap_shutdown();
}

TEST(short_vector_not_sampled) {
    Bundle b = setup(kRingJson, "ring_bundle.json");
    std::string err;
    ModelMonitor mon;
    CHECK(mon.init(b, err));
    CHECK(tap_init("ring", "ring_bundle.json"));
    detail::g_tap.sample_ctr.store(0);

    // Vector shorter than n_inputs: histogram still counts what it got, but no
    // ring row is claimed (only COMPLETE vectors are retraining material).
    float v = 1.5f;
    tap_update_input(&v, 1);
    CHECK(mon.slot()->ring_head.load() == 0);
    tap_shutdown();
}

int main() { return RUN_ALL(); }
