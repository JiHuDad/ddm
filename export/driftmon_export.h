// driftmon_export.h — optional Prometheus export adapter.
//
// SEPARATE, OPTIONAL module (built only when DRIFTMON_ENABLE_PROMETHEUS=ON).
// The core (include/driftmon.h, src/) does NOT depend on this; the dependency
// direction is export -> core (we reuse driftmon_classify), never the reverse.
//
// This adapter is dependency-free: it renders PSI metrics into the Prometheus
// *text exposition format* — exactly what a Prometheus server scrapes. The
// resulting string plugs into whatever HTTP/scrape infrastructure already
// exists ("기존 관측 인프라 재사용"). If you prefer the prometheus-cpp client
// (registry + HTTP exposer / pushgateway), feed these same values into its
// Gauge API; see README for a sketch.
#ifndef DRIFTMON_EXPORT_H
#define DRIFTMON_EXPORT_H

#include <string>
#include <vector>

namespace dm {

// Metric/label names are configurable so the output matches local conventions.
struct ExportConfig {
    std::string psi_metric = "driftmon_psi";            // per-feature gauge
    std::string max_metric = "driftmon_psi_max";        // headline gauge
    std::string severity_metric = "driftmon_drift_severity";  // 0/1/2 gauge
    std::string feature_label = "feature";              // per-feature label key
};

// Render PSI metrics in Prometheus text exposition format.
//
// `feature_names` and `psi` are parallel arrays (typically psi from
// driftmon_compute, names from your reference). `max_psi` is the headline
// value; its severity (0/1/2) is derived via driftmon_classify so the 0.1/0.2
// thresholds stay defined in one place (the core).
//
// Robust to a names/psi length mismatch: only the matched prefix is emitted
// per-feature; max and severity are always emitted.
std::string to_prometheus_text(const std::vector<std::string>& feature_names,
                               const std::vector<double>& psi,
                               double max_psi,
                               const ExportConfig& cfg = ExportConfig());

}  // namespace dm

#endif  // DRIFTMON_EXPORT_H
