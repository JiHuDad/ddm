// test_fault_isolation.cpp — worker crash/restart does not disturb serving (AC6).
//
// Serving and worker are separate processes (separate address spaces), so a
// worker crash cannot crash serving — that isolation is structural. What this
// test pins down is the RECOVERY contract: a restarted worker reuses the
// existing arena, so data a still-running tap accumulated across the outage
// survives and monitoring resumes.
#include "test_framework.h"

#include <string>
#include <vector>

#include "arena_rt.h"
#include "bundle.h"
#include "shm_arena.h"
#include "worker.h"

using namespace driftmon;

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

TEST(worker_restart_reuses_arena) {
    const char* j = R"({"model_id":"fault","window":{"min_samples":1000000,"max_seconds":600000},
      "features":[{"name":"f","bin_edges":[0,1,2,3,4],"ref_hist":[10,20,30,40]}]})";
    Bundle b; std::string err;
    CHECK(parse_bundle(j, b, err));
    SlotSpec spec = slot_spec_from_bundle(b);
    const std::string name = arena_name("fault");
    arena_unlink(name);   // clean slate

    // --- worker #1 boots and creates the arena ---
    Arena w1; bool reused;
    CHECK(arena_open_or_create(w1, name, spec, reused, err));
    CHECK(!reused);

    // --- a serving tap attaches and accumulates 40 samples ---
    Arena tap;
    CHECK(arena_attach(tap, name, spec.n_bins_total, err));
    inject(tap.slot(0), 0, {0, 40, 0, 0, 0, 0});

    // --- worker #1 CRASHES: its mapping vanishes, but the shm is NOT unlinked
    //     (a crash doesn't run cleanup). The tap is in another "process" and is
    //     wholly unaffected — it keeps writing 10 more samples. ---
    arena_detach(w1);
    inject(tap.slot(0), 0, {0, 10, 0, 0, 0, 0});

    // --- worker #2 restarts and REUSES the existing arena in place ---
    Arena w2;
    CHECK(arena_open_or_create(w2, name, spec, reused, err));
    CHECK(reused);   // recovered the running arena rather than wiping it

    // All 50 samples the tap wrote (before AND after the outage) survived.
    std::vector<Histogram> frozen;
    uint64_t n = slot_swap_read(w2.slot(0), frozen);
    CHECK(n == 50);
    CHECK(frozen[0].counts[1] == 50);

    arena_detach(tap);
    arena_detach(w2);
    arena_unlink(name);
}

int main() { return RUN_ALL(); }
