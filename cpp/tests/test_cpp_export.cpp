// test_cpp_export.cpp — export rendering + sinks, best-effort (AC9, R5.x).
#include "test_framework.h"

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "export.h"

using namespace driftmon;

static ExportRecord sample() {
    ExportRecord r;
    r.model_id = "beam";
    r.timestamp = 1717200000;
    r.generation = 7;
    r.max_score = 0.34;
    r.severity = 2;
    ExportFeature f;
    f.name = "sinr";
    f.score = 0.34;
    f.alarm = true;
    f.hist = {0, 10, 20, 5, 0};
    r.features.push_back(f);
    return r;
}

static bool contains(const std::string& hay, const std::string& needle) {
    return hay.find(needle) != std::string::npos;
}

TEST(prometheus_text_contains_metrics) {
    std::string txt = to_prometheus_text({sample()});
    CHECK(contains(txt, "driftmon_drift_score{model=\"beam\",feature=\"sinr\"} 0.34"));
    CHECK(contains(txt, "driftmon_drift_severity{model=\"beam\"} 2"));
    CHECK(contains(txt, "driftmon_drift_score_max{model=\"beam\"} 0.34"));
    CHECK(contains(txt, "driftmon_generation{model=\"beam\"} 7"));
    CHECK(contains(txt, "driftmon_export_timestamp{model=\"beam\"} 1717200000"));
    CHECK(contains(txt, "driftmon_window_hist{model=\"beam\",feature=\"sinr\",bin=\"2\"} 20"));
}

TEST(json_record_roundtrips_fields) {
    std::string j = to_json(sample());
    CHECK(contains(j, "\"model_id\":\"beam\""));
    CHECK(contains(j, "\"severity\":2"));
    CHECK(contains(j, "\"alarm\":true"));
    CHECK(contains(j, "\"hist\":[0,10,20,5,0]"));
}

TEST(prometheus_exporter_writes_file_atomically) {
    const std::string path = "export_test_metrics.prom";
    std::remove(path.c_str());
    PrometheusExporter ex(path);
    CHECK(ex.write({sample()}));

    std::ifstream f(path);
    CHECK(f.good());
    std::stringstream ss; ss << f.rdbuf();
    CHECK(contains(ss.str(), "driftmon_drift_severity{model=\"beam\"} 2"));
    std::remove(path.c_str());
    std::remove((path + ".tmp").c_str());
}

TEST(export_is_best_effort_on_bad_target) {
    // Writing into a non-existent directory must fail gracefully (no throw),
    // returning false so the worker can log and keep running.
    PrometheusExporter ex("/no/such/dir/metrics.prom");
    CHECK(!ex.write({sample()}));

    FileExporter fx("/no/such/dir");
    CHECK(!fx.write({sample()}));
}

TEST(make_exporter_factory) {
    std::string err;
    CHECK(make_exporter("prometheus", "x.prom", err) != nullptr);
    CHECK(make_exporter("file", ".", err) != nullptr);
    CHECK(make_exporter("bogus", "x", err) == nullptr);
    CHECK(!err.empty());
}

int main() { return RUN_ALL(); }
