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
#include <ctime>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "affinity.h"
#include "export.h"
#include "worker.h"

namespace {
// Build an export record from a model's verdict (snapshot + scores).
driftmon::ExportRecord make_record(driftmon::ModelMonitor& mon,
                                   const driftmon::ModelVerdict& v,
                                   int64_t ts, uint64_t gen) {
    driftmon::ExportRecord r;
    r.model_id = mon.bundle().model_id;
    r.timestamp = ts;
    r.generation = gen;
    r.max_score = v.max_score;
    r.severity = v.severity;   // bundle-driven, debounced (P4) — never hardcoded
    r.samples_total = mon.samples_total();
    r.quality_alarm = v.quality_alarm;
    r.kind = v.kind;
    for (size_t f = 0; f < v.per_feature.size(); ++f) {
        driftmon::ExportFeature ef;
        ef.name = mon.bundle().features[f].name;
        ef.score = v.per_feature[f].score;
        ef.alarm = v.per_feature[f].alarm;
        if (f < v.quality.size()) {
            ef.nan_ratio = v.quality[f].nan_ratio;
            ef.oor_ratio = v.quality[f].oor_ratio;
            ef.quality_alarm = v.quality[f].alarm;
        }
        if (f < v.histograms.size()) ef.hist = v.histograms[f].counts;
        r.features.push_back(std::move(ef));
    }
    return r;
}

// Dump the sample ring as CSV retraining material on alarm (R-R1). One file
// per verdict generation; best-effort like all export paths.
void dump_samples(driftmon::ModelMonitor& mon, const std::string& dir, uint64_t gen) {
    auto rows = driftmon::read_ring(mon.slot());
    if (rows.empty()) return;
    std::string path = dir + "/driftmon_" + mon.bundle().model_id +
                       "_gen" + std::to_string(gen) + "_samples.csv";
    std::FILE* f = std::fopen(path.c_str(), "w");
    if (!f) { std::fprintf(stderr, "sample dump failed: %s\n", path.c_str()); return; }
    bool first = true;
    for (const auto& feat : mon.bundle().features) {
        if (feat.is_output) continue;
        std::fprintf(f, "%s%s", first ? "" : ",", feat.name.c_str());
        first = false;
    }
    std::fprintf(f, "\n");
    for (const auto& row : rows) {
        for (size_t j = 0; j < row.size(); ++j)
            std::fprintf(f, "%s%.9g", j ? "," : "", static_cast<double>(row[j]));
        std::fprintf(f, "\n");
    }
    std::fclose(f);
    std::fprintf(stderr, "dumped %zu sampled input vectors → %s\n", rows.size(), path.c_str());
}
}  // namespace

namespace {
volatile std::sig_atomic_t g_stop = 0;
void on_signal(int) { g_stop = 1; }
}  // namespace

int main(int argc, char** argv) {
    std::vector<std::string> bundles;
    std::string bundle_dir, export_kind, export_target, cpu_list, sample_dir;
    int period_ms = 100;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--bundle") == 0 && i + 1 < argc) bundles.push_back(argv[++i]);
        else if (std::strcmp(argv[i], "--bundle-dir") == 0 && i + 1 < argc) bundle_dir = argv[++i];
        else if (std::strcmp(argv[i], "--period-ms") == 0 && i + 1 < argc) period_ms = std::atoi(argv[++i]);
        else if (std::strcmp(argv[i], "--export") == 0 && i + 1 < argc) export_kind = argv[++i];
        else if (std::strcmp(argv[i], "--export-target") == 0 && i + 1 < argc) export_target = argv[++i];
        else if (std::strcmp(argv[i], "--sample-dir") == 0 && i + 1 < argc) sample_dir = argv[++i];
        else if (std::strcmp(argv[i], "--cpu") == 0 && i + 1 < argc) cpu_list = argv[++i];
        else { std::fprintf(stderr, "unknown arg: %s\n", argv[i]); return 2; }
    }
    if (bundles.empty() && bundle_dir.empty()) {
        std::fprintf(stderr,
            "usage: %s (--bundle <path>)... | --bundle-dir <dir>\n"
            "          [--period-ms <n>] [--export prometheus|file --export-target <path>]\n"
            "          [--sample-dir <dir>] [--cpu <list e.g. 2,3>]\n", argv[0]);
        return 2;
    }

    // NFR3: pin to housekeeping cores (best-effort).
    if (!cpu_list.empty()) {
        std::vector<int> cpus;
        std::string err;
        if (!driftmon::parse_cpu_list(cpu_list, cpus))
            std::fprintf(stderr, "bad --cpu list: %s\n", cpu_list.c_str());
        else if (!driftmon::pin_to_cpus(cpus, err))
            std::fprintf(stderr, "cpu pin failed (continuing): %s\n", err.c_str());
        else
            std::fprintf(stderr, "pinned worker to cpus %s\n", cpu_list.c_str());
    }

    // Export sink (optional; best-effort).
    std::unique_ptr<driftmon::Exporter> exporter;
    if (!export_kind.empty()) {
        std::string err;
        exporter = driftmon::make_exporter(export_kind, export_target, err);
        if (!exporter) { std::fprintf(stderr, "export config error: %s\n", err.c_str()); return 2; }
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

    // One window clock + export generation counter per model (independent).
    // last_record holds each model's latest exported state so the heartbeat can
    // re-publish it with fresh samples_total even when no window closed — the
    // tap dying silently must be observable off-box (rate(samples_total)==0).
    std::vector<std::chrono::steady_clock::time_point> win_start(
        ws.size(), std::chrono::steady_clock::now());
    std::vector<uint64_t> generation(ws.size(), 0);
    std::vector<driftmon::ExportRecord> last_record(ws.size());
    for (size_t i = 0; i < ws.size(); ++i)
        last_record[i].model_id = ws.at(i).bundle().model_id;
    auto last_export = std::chrono::steady_clock::now();
    const auto heartbeat = std::chrono::milliseconds(1000);

    while (!g_stop) {
        std::this_thread::sleep_for(std::chrono::milliseconds(period_ms));
        const auto now = std::chrono::steady_clock::now();
        const int64_t ts = static_cast<int64_t>(std::time(nullptr));
        bool verdict_this_round = false;
        for (size_t i = 0; i < ws.size(); ++i) {
            const double elapsed = std::chrono::duration<double>(now - win_start[i]).count();
            driftmon::ModelVerdict v = ws.at(i).tick(elapsed);
            const std::string& id = ws.at(i).bundle().model_id;
            if (v.produced) {
                std::printf("model=%s window_samples=%ld max_score=%.4f severity=%d kind=%s quality_alarm=%d\n",
                            id.c_str(), v.window_samples, v.max_score, v.severity,
                            v.kind.c_str(), v.quality_alarm ? 1 : 0);
                std::fflush(stdout);
                last_record[i] = make_record(ws.at(i), v, ts, ++generation[i]);
                verdict_this_round = true;
                // Alarm ⇒ dump raw sampled inputs as retraining material.
                if ((v.alarm || v.quality_alarm) && !sample_dir.empty())
                    dump_samples(ws.at(i), sample_dir, generation[i]);
                win_start[i] = std::chrono::steady_clock::now();
            } else if (v.warming_up) {
                std::fprintf(stderr, "model=%s warming up (window_samples=%ld)\n",
                             id.c_str(), v.window_samples);
                win_start[i] = std::chrono::steady_clock::now();
            }
        }
        // Export on every verdict, plus a heartbeat with fresh liveness counters.
        if (exporter && (verdict_this_round || now - last_export >= heartbeat)) {
            for (size_t i = 0; i < ws.size(); ++i) {
                last_record[i].timestamp = ts;
                last_record[i].samples_total = ws.at(i).samples_total();
            }
            if (!exporter->write(last_record))   // best-effort (R5.3)
                std::fprintf(stderr, "export write failed (continuing)\n");
            last_export = now;
        }
    }
    std::fprintf(stderr, "driftmon worker stopping\n");
    return 0;
}
