// test_arena_concurrency.cpp — lock-free protocol: no lost/duplicated/corrupted
// counts under concurrent writers + a swapping reader (AC2).
#include "test_framework.h"

#include <atomic>
#include <thread>
#include <vector>

#include "arena_rt.h"
#include "shm_arena.h"
#include "worker.h"

using namespace driftmon;

TEST(no_corruption_under_concurrent_swap) {
    SlotSpec spec;
    spec.model_id = "conc";
    spec.n_features = 1;
    spec.bin_offset = {0, 6};      // 4 interior + underflow + overflow
    spec.n_bins_total = 6;

    Arena arena;
    std::string err;
    CHECK(arena_create(arena, "/driftmon.test_conc", spec, err));
    SlotHeader* s = arena.slot(0);

    constexpr int K = 4;           // writer threads
    constexpr int M = 50000;       // increments per writer
    const uint64_t expected = static_cast<uint64_t>(K) * M;

    std::atomic<bool> done{false};
    std::atomic<uint64_t> read_samples{0};
    std::atomic<uint64_t> read_bins{0};

    // Reader: repeatedly freeze+drain (unbounded grace ⇒ zero loss) and tally.
    std::thread reader([&] {
        std::vector<Histogram> frozen;
        while (!done.load(std::memory_order_acquire)) {
            uint64_t n = slot_swap_read(s, frozen, /*grace_spins=*/0);
            read_samples += n;
            for (uint64_t c : frozen[0].counts) read_bins += c;
            std::this_thread::yield();
        }
    });

    // Writers: each does M full writer critical sections incrementing one bin.
    std::vector<std::thread> writers;
    for (int w = 0; w < K; ++w) {
        writers.emplace_back([&] {
            for (int i = 0; i < M; ++i) {
                const uint32_t idx = writer_acquire(s);
                hist_buffer(s, idx)[s->bin_offset[0] + 1].fetch_add(1, std::memory_order_relaxed);
                s->sample_count[idx].fetch_add(1, std::memory_order_relaxed);
                writer_release(s, idx);
            }
        });
    }
    for (auto& t : writers) t.join();
    done.store(true, std::memory_order_release);
    reader.join();

    // Drain whatever remained in the live buffer after the reader stopped.
    std::vector<Histogram> frozen;
    uint64_t tail = slot_swap_read(s, frozen, /*grace_spins=*/0);
    read_samples += tail;
    for (uint64_t c : frozen[0].counts) read_bins += c;

    // Every increment accounted for exactly once: no loss, no double-count.
    CHECK(read_samples.load() == expected);
    CHECK(read_bins.load() == expected);

    arena_detach(arena);
    arena_unlink("/driftmon.test_conc");
}

int main() { return RUN_ALL(); }
