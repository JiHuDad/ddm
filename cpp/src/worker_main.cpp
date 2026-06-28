// worker_main.cpp — driftmon-cpp drift worker entry point (SPEC §5.3).
//
// Thin driver: parse args → load+validate bundle → create arena → tick loop.
// Phase 1 is single-model; the loop structure already isolates per-model state
// so Phase 2 can iterate several monitors.
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>

#include "bundle.h"
#include "worker.h"

namespace {
volatile std::sig_atomic_t g_stop = 0;
void on_signal(int) { g_stop = 1; }
}  // namespace

int main(int argc, char** argv) {
    std::string bundle_path, model_id;
    int period_ms = 100;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--bundle") == 0 && i + 1 < argc) bundle_path = argv[++i];
        else if (std::strcmp(argv[i], "--model") == 0 && i + 1 < argc) model_id = argv[++i];
        else if (std::strcmp(argv[i], "--period-ms") == 0 && i + 1 < argc) period_ms = std::atoi(argv[++i]);
        else { std::fprintf(stderr, "unknown arg: %s\n", argv[i]); return 2; }
    }
    if (bundle_path.empty()) {
        std::fprintf(stderr, "usage: %s --bundle <path> [--model <id>] [--period-ms <n>]\n", argv[0]);
        return 2;
    }

    driftmon::Bundle b;
    std::string err;
    if (!driftmon::load_bundle(bundle_path, b, err)) {
        std::fprintf(stderr, "bundle load failed (monitoring disabled): %s\n", err.c_str());
        return 1;   // R4.2 gate: clear error, no silent wrong-value operation
    }
    if (!model_id.empty() && b.model_id != model_id) {
        std::fprintf(stderr, "model_id mismatch: bundle=%s arg=%s\n", b.model_id.c_str(), model_id.c_str());
        return 1;
    }

    driftmon::ModelMonitor mon;
    if (!mon.init(b, err)) {
        std::fprintf(stderr, "arena init failed: %s\n", err.c_str());
        return 1;
    }
    std::fprintf(stderr, "driftmon worker up: model=%s features=%zu window={min=%ld,max_s=%ld}\n",
                 b.model_id.c_str(), b.features.size(), b.min_samples, b.max_seconds);

    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);

    auto window_start = std::chrono::steady_clock::now();
    while (!g_stop) {
        std::this_thread::sleep_for(std::chrono::milliseconds(period_ms));
        const double elapsed =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - window_start).count();
        driftmon::ModelVerdict v = mon.tick(elapsed);
        if (v.produced) {
            std::printf("model=%s window_samples=%ld max_psi=%.4f severity=%d\n",
                        b.model_id.c_str(), v.window_samples, v.max_score, v.alarm ? 2 : 0);
            std::fflush(stdout);
            window_start = std::chrono::steady_clock::now();
        } else if (v.warming_up) {
            std::fprintf(stderr, "model=%s warming up (window_samples=%ld < min)\n",
                         b.model_id.c_str(), v.window_samples);
            window_start = std::chrono::steady_clock::now();
        }
    }
    std::fprintf(stderr, "driftmon worker stopping\n");
    return 0;
}
