/*
 * export.h — driftmon-cpp off-box export (SPEC §5.5, AC9).
 *
 * The worker periodically exports per-model {scores, alarm/severity, histogram
 * snapshot, generation, timestamp}. Two interchangeable sinks (R5.2), selected
 * by config: a Prometheus textfile-collector file, or a JSON artifact directory
 * (the MinIO-style path). Export is BEST-EFFORT (R5.3): write() returns false on
 * failure but never throws, and the worker loop keeps running.
 */
#ifndef DRIFTMON_EXPORT_H
#define DRIFTMON_EXPORT_H

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace driftmon {

struct ExportFeature {
    std::string name;
    double score = 0.0;
    bool   alarm = false;
    // Data-quality channel (v2) — separate from drift on purpose.
    double nan_ratio = 0.0;
    double oor_ratio = 0.0;
    bool   quality_alarm = false;
    std::vector<uint64_t> hist;   // physical-bin counts (snapshot)
};

struct ExportRecord {
    std::string model_id;
    int64_t  timestamp = 0;       // unix seconds (injected by caller)
    uint64_t generation = 0;      // monotonically increasing per model
    double   max_score = 0.0;
    int      severity = 0;        // 0 STABLE / 1 WARNING / 2 SIGNIFICANT
    // Tap-liveness: cumulative samples the worker has drained for this model.
    // Off-box rule `rate(driftmon_samples_total)==0 while serving` ⇒ the tap
    // silently degraded to no-op — the failure mode that must never be silent.
    uint64_t samples_total = 0;
    bool quality_alarm = false;   // any feature's pipeline-quality alarm (v2)
    // Change-kind classification (R2): "none" | "data_quality" | "out_of_range"
    // | "abrupt" | "sustained" | "distribution" — routes the off-box response.
    std::string kind = "none";
    std::vector<ExportFeature> features;
};

// Render records as Prometheus text exposition format (pure, testable).
std::string to_prometheus_text(const std::vector<ExportRecord>& recs);

// Render one record as a JSON object (artifact payload).
std::string to_json(const ExportRecord& rec);

class Exporter {
public:
    virtual bool write(const std::vector<ExportRecord>& recs) = 0;  // best-effort
    virtual ~Exporter() = default;
};

// Prometheus textfile collector: atomically replace `path` with the rendered
// metrics each call (write tmp + rename).
class PrometheusExporter : public Exporter {
public:
    explicit PrometheusExporter(std::string path) : path_(std::move(path)) {}
    bool write(const std::vector<ExportRecord>& recs) override;
private:
    std::string path_;
};

// File artifact sink (MinIO-style): one JSON snapshot per call under `dir`.
class FileExporter : public Exporter {
public:
    explicit FileExporter(std::string dir) : dir_(std::move(dir)) {}
    bool write(const std::vector<ExportRecord>& recs) override;
private:
    std::string dir_;
    uint64_t seq_ = 0;
};

// Factory. kind: "prometheus" (target=file path) | "file" (target=dir).
// Returns nullptr + err on unknown kind.
std::unique_ptr<Exporter> make_exporter(const std::string& kind,
                                        const std::string& target, std::string& err);

}  // namespace driftmon

#endif  // DRIFTMON_EXPORT_H
