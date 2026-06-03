// driftmon_export.cpp — Prometheus text exposition format renderer.
// Dependency-free (standard library only). Reuses the core's driftmon_classify.
#include "driftmon_export.h"

#include <cstdio>

#include "driftmon.h"  // core: driftmon_classify, driftmon_severity_t

namespace dm {

namespace {

// Escape a Prometheus label value: backslash, double-quote, and newline.
std::string escape_label(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (char c : s) {
        switch (c) {
            case '\\': out += "\\\\"; break;
            case '"':  out += "\\\""; break;
            case '\n': out += "\\n"; break;
            default:   out += c; break;
        }
    }
    return out;
}

// Format a double the way Prometheus expects (compact, round-trippable enough).
std::string fmt(double v) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.10g", v);
    return std::string(buf);
}

}  // namespace

std::string to_prometheus_text(const std::vector<std::string>& feature_names,
                               const std::vector<double>& psi,
                               double max_psi,
                               const ExportConfig& cfg) {
    std::string out;

    // Per-feature PSI gauge.
    out += "# HELP " + cfg.psi_metric +
           " Population Stability Index per feature.\n";
    out += "# TYPE " + cfg.psi_metric + " gauge\n";
    const size_t n =
        feature_names.size() < psi.size() ? feature_names.size() : psi.size();
    for (size_t i = 0; i < n; ++i) {
        out += cfg.psi_metric + "{" + cfg.feature_label + "=\"" +
               escape_label(feature_names[i]) + "\"} " + fmt(psi[i]) + "\n";
    }

    // Headline max PSI gauge.
    out += "# HELP " + cfg.max_metric +
           " Maximum PSI across all features.\n";
    out += "# TYPE " + cfg.max_metric + " gauge\n";
    out += cfg.max_metric + " " + fmt(max_psi) + "\n";

    // Severity gauge (0=stable, 1=warning, 2=significant), via the core's
    // single source of thresholds.
    const int sev = static_cast<int>(driftmon_classify(max_psi));
    out += "# HELP " + cfg.severity_metric +
           " Drift severity: 0=stable, 1=warning, 2=significant.\n";
    out += "# TYPE " + cfg.severity_metric + " gauge\n";
    out += cfg.severity_metric + " " + std::to_string(sev) + "\n";

    return out;
}

}  // namespace dm
