// test_tap_handles.cpp — multi-model tap handles (A2): two models tapped from
// ONE serving process, isolation between them, no-op degrade + self-heal per
// handle, legacy single-model API coexistence.
#include "test_framework.h"

#include <fstream>
#include <string>
#include <vector>

#include "driftmon/tap.h"
#include "bundle.h"
#include "worker.h"

using namespace driftmon;

static Bundle make_model(const char* id, const char* path) {
    std::string j = std::string(R"({"model_id":")") + id +
        R"(","window":{"min_samples":100000,"max_seconds":100000},
        "features":[{"name":"f","bin_edges":[0,1,2,3,4],"ref_hist":[10,20,30,40]}]})";
    std::ofstream(path) << j;
    Bundle b; std::string err;
    parse_bundle(j, b, err);
    return b;
}

TEST(two_models_one_process) {
    Bundle b1 = make_model("multi_a", "multi_a.json");
    Bundle b2 = make_model("multi_b", "multi_b.json");
    std::string err;
    ModelMonitor mon1, mon2;                    // one worker, two arenas
    CHECK(mon1.init(b1, err));
    CHECK(mon2.init(b2, err));

    TapHandle* h1 = tap_open("multi_a", "multi_a.json");
    TapHandle* h2 = tap_open("multi_b", "multi_b.json");
    CHECK(h1 != nullptr);
    CHECK(h2 != nullptr);

    // Distinct values through each handle: 2.5 → bin 3 on A, 0.5 → bin 1 on B.
    float va = 2.5f, vb = 0.5f;
    for (int i = 0; i < 3; ++i) tap_input(h1, &va, 1);
    for (int i = 0; i < 5; ++i) tap_input(h2, &vb, 1);

    std::vector<Histogram> fa, fb;
    CHECK(slot_swap_read(mon1.slot(), fa) == 3);
    CHECK(slot_swap_read(mon2.slot(), fb) == 5);
    CHECK(fa[0].counts[3] == 3);                // isolation: A got only A's
    CHECK(fa[0].counts[1] == 0);
    CHECK(fb[0].counts[1] == 5);
    CHECK(fb[0].counts[3] == 0);

    tap_close(h1);
    tap_close(h2);
}

TEST(handle_degrades_and_self_heals) {
    Bundle b = make_model("multi_late", "multi_late.json");
    arena_unlink(arena_name("multi_late"));

    // Worker not up yet: handle opens in no-op mode instead of failing.
    TapHandle* h = tap_open("multi_late", "multi_late.json");
    CHECK(h != nullptr);
    float v = 2.5f;
    tap_input(h, &v, 1);                        // safe no-op

    std::string err;
    ModelMonitor mon;                           // worker arrives
    CHECK(mon.init(b, err));
    CHECK(tap_maintain());                      // maintains ALL handles

    tap_input(h, &v, 1);
    std::vector<Histogram> frozen;
    CHECK(slot_swap_read(mon.slot(), frozen) == 1);
    CHECK(frozen[0].counts[3] == 1);

    tap_close(h);
}

TEST(null_and_bad_args_are_safe) {
    CHECK(tap_open(nullptr, "x.json") == nullptr);
    CHECK(tap_open("m", nullptr) == nullptr);
    tap_input(nullptr, nullptr, 0);             // all no-ops, no crash
    tap_output(nullptr, nullptr, 0);
    tap_close(nullptr);
}

int main() { return RUN_ALL(); }
