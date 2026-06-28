// export.cpp — Prometheus text + JSON artifact sinks (best-effort, R5.3).
#include "export.h"

#include <cstdio>
#include <fstream>
#include <sstream>

namespace driftmon {

std::string to_prometheus_text(const std::vector<ExportRecord>& recs) {
    std::ostringstream o;
    o << "# HELP driftmon_drift_score Per-feature drift score (worst detector).\n";
    o << "# TYPE driftmon_drift_score gauge\n";
    o << "# HELP driftmon_drift_severity Model drift severity (0 stable,1 warn,2 significant).\n";
    o << "# TYPE driftmon_drift_severity gauge\n";
    for (const auto& r : recs) {
        for (const auto& f : r.features) {
            o << "driftmon_drift_score{model=\"" << r.model_id
              << "\",feature=\"" << f.name << "\"} " << f.score << "\n";
            o << "driftmon_drift_alarm{model=\"" << r.model_id
              << "\",feature=\"" << f.name << "\"} " << (f.alarm ? 1 : 0) << "\n";
            for (size_t k = 0; k < f.hist.size(); ++k)
                o << "driftmon_window_hist{model=\"" << r.model_id
                  << "\",feature=\"" << f.name << "\",bin=\"" << k << "\"} "
                  << f.hist[k] << "\n";
        }
        o << "driftmon_drift_score_max{model=\"" << r.model_id << "\"} " << r.max_score << "\n";
        o << "driftmon_drift_severity{model=\"" << r.model_id << "\"} " << r.severity << "\n";
        o << "driftmon_generation{model=\"" << r.model_id << "\"} " << r.generation << "\n";
        o << "driftmon_export_timestamp{model=\"" << r.model_id << "\"} " << r.timestamp << "\n";
    }
    return o.str();
}

std::string to_json(const ExportRecord& r) {
    std::ostringstream o;
    o << "{\"model_id\":\"" << r.model_id << "\",\"timestamp\":" << r.timestamp
      << ",\"generation\":" << r.generation << ",\"max_score\":" << r.max_score
      << ",\"severity\":" << r.severity << ",\"features\":[";
    for (size_t i = 0; i < r.features.size(); ++i) {
        const auto& f = r.features[i];
        if (i) o << ",";
        o << "{\"name\":\"" << f.name << "\",\"score\":" << f.score
          << ",\"alarm\":" << (f.alarm ? "true" : "false") << ",\"hist\":[";
        for (size_t k = 0; k < f.hist.size(); ++k) { if (k) o << ","; o << f.hist[k]; }
        o << "]}";
    }
    o << "]}";
    return o.str();
}

bool PrometheusExporter::write(const std::vector<ExportRecord>& recs) {
    // Atomic replace: write tmp then rename, so a scraping collector never reads
    // a half-written file.
    const std::string tmp = path_ + ".tmp";
    {
        std::ofstream f(tmp, std::ios::trunc);
        if (!f) return false;
        f << to_prometheus_text(recs);
        if (!f) return false;
    }
    return std::rename(tmp.c_str(), path_.c_str()) == 0;
}

bool FileExporter::write(const std::vector<ExportRecord>& recs) {
    bool ok = true;
    for (const auto& r : recs) {
        std::ostringstream name;
        name << dir_ << "/driftmon_" << r.model_id << "_" << seq_++ << ".json";
        std::ofstream f(name.str(), std::ios::trunc);
        if (!f) { ok = false; continue; }
        f << to_json(r) << "\n";
        if (!f) ok = false;
    }
    return ok;
}

std::unique_ptr<Exporter> make_exporter(const std::string& kind,
                                        const std::string& target, std::string& err) {
    if (kind == "prometheus") return std::make_unique<PrometheusExporter>(target);
    if (kind == "file")       return std::make_unique<FileExporter>(target);
    err = "unknown export kind: " + kind + " (use 'prometheus' or 'file')";
    return nullptr;
}

}  // namespace driftmon
