// test_export.cpp — tests for the optional Prometheus export adapter.
// Built only when DRIFTMON_ENABLE_PROMETHEUS=ON.
#include "driftmon_export.h"
#include "test_framework.h"

#include <string>

// Does `haystack` contain `needle`?
static bool contains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

TEST(export_basic_structure) {
    std::vector<std::string> names = {"rsrp", "delay"};
    std::vector<double> psi = {0.0203, 0.15};
    std::string text = dm::to_prometheus_text(names, psi, 0.15);

    // HELP/TYPE headers present.
    CHECK(contains(text, "# TYPE driftmon_psi gauge"));
    CHECK(contains(text, "# TYPE driftmon_psi_max gauge"));
    CHECK(contains(text, "# TYPE driftmon_drift_severity gauge"));

    // Per-feature lines with labels.
    CHECK(contains(text, "driftmon_psi{feature=\"rsrp\"} 0.0203"));
    CHECK(contains(text, "driftmon_psi{feature=\"delay\"} 0.15"));

    // Headline max.
    CHECK(contains(text, "driftmon_psi_max 0.15"));
}

TEST(export_severity_reflects_max_psi) {
    std::vector<std::string> names = {"x"};

    // Stable: max < 0.1 → severity 0.
    CHECK(contains(dm::to_prometheus_text(names, {0.05}, 0.05),
                   "driftmon_drift_severity 0"));
    // Warning: 0.1 <= max < 0.2 → severity 1.
    CHECK(contains(dm::to_prometheus_text(names, {0.15}, 0.15),
                   "driftmon_drift_severity 1"));
    // Significant: max >= 0.2 → severity 2.
    CHECK(contains(dm::to_prometheus_text(names, {0.30}, 0.30),
                   "driftmon_drift_severity 2"));
}

TEST(export_escapes_label_values) {
    // A feature name with a quote and backslash must be escaped.
    std::vector<std::string> names = {"a\"b\\c"};
    std::string text = dm::to_prometheus_text(names, {0.1}, 0.1);
    CHECK(contains(text, "feature=\"a\\\"b\\\\c\""));
}

TEST(export_custom_config) {
    dm::ExportConfig cfg;
    cfg.psi_metric = "myapp_psi";
    cfg.feature_label = "feat";
    std::string text = dm::to_prometheus_text({"f1"}, {0.42}, 0.42, cfg);
    CHECK(contains(text, "myapp_psi{feat=\"f1\"} 0.42"));
}

TEST(export_length_mismatch_is_robust) {
    // 2 names but 1 psi value: only the matched prefix is emitted per-feature,
    // and max/severity are still present. Must not crash.
    std::vector<std::string> names = {"a", "b"};
    std::vector<double> psi = {0.1};
    std::string text = dm::to_prometheus_text(names, psi, 0.1);
    CHECK(contains(text, "driftmon_psi{feature=\"a\"} 0.1"));
    CHECK(!contains(text, "feature=\"b\""));
    CHECK(contains(text, "driftmon_psi_max 0.1"));
    CHECK(contains(text, "driftmon_drift_severity 1"));
}

int main() { return RUN_ALL(); }
