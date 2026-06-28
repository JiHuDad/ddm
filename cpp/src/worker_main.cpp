// worker_main.cpp — driftmon-cpp drift worker entry point (SPEC §5.3, G3).
//
// Thin driver: parse args → load+validate bundles (gate the bad ones) → create
// arenas → tick all models each period. A single worker process handles every
// model_id. Restart-safe: a re-launched worker reuses the existing arenas, so
// running serving taps keep accumulating uninterrupted (AC6).
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "worker.h"

namespace {
volatile std::sig_atomic_t g_stop = 0;
void on_signal(int) { g_stop = 1; }
}  // namespace

int main(int argc, char** argv) {
    std::vector<std::string> bundles;
    std::string bundle_dir;
    int period_ms = 100;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--bundle") == 0 && i + 1 < argc) bundles.push_back(argv[++i]);
        else if (std::strcmp(argv[i], "--bundle-dir") == 0 && i + 1 < argc) bundle_dir = argv[++i];
        else if (std::strcmp(argv[i], "--period-ms") == 0 && i + 1 < argc) period_ms = std::atoi(argv[++i]);
        else { std::fprintf(stderr, "unknown arg: %s\n", argv[i]); return 2; }
    }
    if (bundles.empty() && bundle_dir.empty()) {
        std::fprintf(stderr,
            "usage: %s (--bundle <path>)... | --bundle-dir <dir> [--period-ms <n>]\n", argv[0]);
        return 2;
    }

    driftmon::WorkerSet ws;
    size_t up = bundle_dir.empty() ? ws.load(bundles) : ws.load_dir(bundle_dir);
    for (const std::string& e : ws.errors())   // R4.2: clear errors, monitoring disabled
        std::fprintf(stderr, "bundle gated (monitoring disabled): %s\n", e.c_str());
    if (up == 0) {
        std::fprintf(stderr, "no models could be monitored\n");
        return 1;
    }
    std::fprintf(stderr, "driftmon worker up: %zu model(s) monitored\n", up);

    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);

    // One window clock per model (windows close independently).
    std::vector<std::chrono::steady_clock::time_point> win_start(
        ws.size(), std::chrono::steady_clock::now());

    std::vector<driftmon::ModelVerdict> verdicts;
    while (!g_stop) {
        std::this_thread::sleep_for(std::chrono::milliseconds(period_ms));
        const auto now = std::chrono::steady_clock::now();
        for (size_t i = 0; i < ws.size(); ++i) {
            const double elapsed = std::chrono::duration<double>(now - win_start[i]).count();
            driftmon::ModelVerdict v = ws.at(i).tick(elapsed);
            const std::string& id = ws.at(i).bundle().model_id;
            if (v.produced) {
                std::printf("model=%s window_samples=%ld max_score=%.4f severity=%d\n",
                            id.c_str(), v.window_samples, v.max_score, v.alarm ? 2 : 0);
                std::fflush(stdout);
                win_start[i] = std::chrono::steady_clock::now();
            } else if (v.warming_up) {
                std::fprintf(stderr, "model=%s warming up (window_samples=%ld)\n",
                             id.c_str(), v.window_samples);
                win_start[i] = std::chrono::steady_clock::now();
            }
        }
    }
    std::fprintf(stderr, "driftmon worker stopping\n");
    return 0;
}
