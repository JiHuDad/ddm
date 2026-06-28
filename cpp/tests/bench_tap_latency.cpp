// bench_tap_latency.cpp — AC1: tap_update_input hot-path latency + zero-alloc.
//
// Not a gating ctest (timing is environment-sensitive). Run manually:
//   ./bench_tap_latency
// Prints p50/p99 ns/call and FAILS only if a heap allocation occurs on the hot
// path (a hard correctness invariant) or latency blows past a generous ceiling.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <new>
#include <string>
#include <vector>

#include "driftmon/tap.h"
#include "bundle.h"
#include "worker.h"

// --- allocation sentinel: trip a flag if the hot path allocates --------------
static std::atomic<bool> g_alloc_armed{false};
static std::atomic<uint64_t> g_alloc_count{0};
void* operator new(std::size_t n) {
    if (g_alloc_armed.load(std::memory_order_relaxed))
        g_alloc_count.fetch_add(1, std::memory_order_relaxed);
    void* p = std::malloc(n ? n : 1);
    if (!p) throw std::bad_alloc();
    return p;
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }

using namespace driftmon;

int main() {
    const char* path = "bench_bundle.json";
    const char* json = R"({"model_id":"bench","window":{"min_samples":100000000,"max_seconds":100000},
      "features":[{"name":"a","bin_edges":[0,1,2,3,4,5,6,7,8,9,10],"ref_hist":[1,1,1,1,1,1,1,1,1,1]},
                  {"name":"b","bin_edges":[0,1,2,3,4,5,6,7,8,9,10],"ref_hist":[1,1,1,1,1,1,1,1,1,1]},
                  {"name":"c","bin_edges":[0,1,2,3,4,5,6,7,8,9,10],"ref_hist":[1,1,1,1,1,1,1,1,1,1]}]})";
    std::ofstream(path) << json;

    Bundle b; std::string err;
    if (!parse_bundle(json, b, err)) { std::fprintf(stderr, "bundle: %s\n", err.c_str()); return 1; }
    ModelMonitor mon;
    if (!mon.init(b, err)) { std::fprintf(stderr, "init: %s\n", err.c_str()); return 1; }
    if (!tap_init("bench", path)) { std::fprintf(stderr, "tap_init failed\n"); return 1; }

    float feat[3] = {3.5f, 7.2f, 0.4f};

    // Warm up.
    for (int i = 0; i < 100000; ++i) tap_update_input(feat, 3);

    constexpr int N = 2000000;
    std::vector<uint32_t> ns;
    ns.reserve(N);

    g_alloc_armed.store(true, std::memory_order_relaxed);
    const uint64_t alloc_before = g_alloc_count.load();
    for (int i = 0; i < N; ++i) {
        feat[0] = static_cast<float>(i % 11);          // vary the bin
        auto t0 = std::chrono::steady_clock::now();
        tap_update_input(feat, 3);
        auto t1 = std::chrono::steady_clock::now();
        ns.push_back(static_cast<uint32_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count()));
    }
    const uint64_t allocs = g_alloc_count.load() - alloc_before;
    g_alloc_armed.store(false, std::memory_order_relaxed);

    std::sort(ns.begin(), ns.end());
    const uint32_t p50 = ns[ns.size() / 2];
    const uint32_t p99 = ns[ns.size() * 99 / 100];
    std::printf("tap_update_input over %d calls: p50=%u ns  p99=%u ns  hot-path allocs=%llu\n",
                N, p50, p99, static_cast<unsigned long long>(allocs));

    tap_shutdown();

    int rc = 0;
    if (allocs != 0) { std::fprintf(stderr, "FAIL: hot path allocated %llu times\n",
                                    static_cast<unsigned long long>(allocs)); rc = 1; }
    // Generous ceiling: catches gross regressions without flaking on slow CI.
    if (p50 > 500) { std::fprintf(stderr, "FAIL: p50 %u ns exceeds 500 ns ceiling\n", p50); rc = 1; }
    std::printf("%s\n", rc == 0 ? "BENCH OK" : "BENCH FAIL");
    return rc;
}
