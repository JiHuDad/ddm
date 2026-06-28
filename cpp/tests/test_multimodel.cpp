// test_multimodel.cpp — multi-model gate (AC7) + code-free registration (AC8).
#include "test_framework.h"

#include <fstream>
#include <string>
#include <vector>

#include "worker.h"

using namespace driftmon;

static std::string valid_bundle(const char* id) {
    return std::string(R"({"model_id":")") + id + R"(","window":{"min_samples":50,"max_seconds":60},
      "features":[{"name":"f","bin_edges":[0,1,2,3,4],"ref_hist":[10,20,30,40]}]})";
}

static void write_file(const std::string& path, const std::string& content) {
    std::ofstream(path) << content;
}

TEST(gate_isolates_bad_bundle) {     // AC7
    write_file("mm_good_a.json", valid_bundle("good_a"));
    write_file("mm_bad.json",
               R"({"model_id":"bad","features":[{"name":"f","bin_edges":[0,1],"ref_hist":[5,5]}]})");
    write_file("mm_good_b.json", valid_bundle("good_b"));

    WorkerSet ws;
    size_t up = ws.load({"mm_good_a.json", "mm_bad.json", "mm_good_b.json"});

    // Only the two valid models are monitored; the bad one is gated, not fatal.
    CHECK(up == 2);
    CHECK(ws.size() == 2);
    CHECK(ws.errors().size() == 1);
    CHECK(ws.errors()[0].find("mm_bad.json") != std::string::npos);
}

TEST(code_free_model_registration) {     // AC8
    write_file("mm_r1.json", valid_bundle("reg1"));
    write_file("mm_r2.json", valid_bundle("reg2"));

    WorkerSet ws;
    CHECK(ws.load({"mm_r1.json", "mm_r2.json"}) == 2);

    // "Adding a model" is purely providing another bundle — no code change.
    write_file("mm_r3.json", valid_bundle("reg3"));
    CHECK(ws.load({"mm_r3.json"}) == 3);   // cumulative: now 3 models monitored
    CHECK(ws.size() == 3);

    // Each model got its own slot/feature set.
    CHECK(ws.at(0).bundle().model_id == "reg1");
    CHECK(ws.at(2).bundle().model_id == "reg3");
}

int main() { return RUN_ALL(); }
