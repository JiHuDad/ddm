// test_bundle.cpp — new bundle schema parse + validation gate (R4.1/R4.2).
#include "test_framework.h"

#include <string>

#include "bundle.h"

using namespace driftmon;

static const char* kValid = R"({
  "schema_version": "1.0",
  "model_id": "beam_predictor_v3",
  "created_at": "2026-06-01T00:00:00Z",
  "mode_tag": "normal",
  "features": [
    {"name": "sinr_db", "dtype": "float32", "index": 0,
     "bin_edges": [-10, -6, -2, 2, 6], "ref_hist": [12, 45, 130, 30],
     "psi_threshold": 0.25}
  ],
  "outputs": [
    {"name": "beam_id", "dtype": "float32", "index": 1,
     "bin_edges": [0, 1, 2, 3], "ref_hist": [100, 50, 25]}
  ],
  "window": {"min_samples": 500, "max_seconds": 30},
  "tests": ["psi", "ks"]
})";

TEST(bundle_valid_parse) {
    Bundle b; std::string err;
    CHECK(parse_bundle(kValid, b, err));
    CHECK(err.empty());
    CHECK(b.model_id == "beam_predictor_v3");
    // inputs (1) then outputs (1) concatenated into one flat feature list.
    CHECK(b.features.size() == 2);
    CHECK(b.features[0].name == "sinr_db");
    CHECK(b.features[0].is_output == false);
    CHECK(b.features[1].name == "beam_id");
    CHECK(b.features[1].is_output == true);
    CHECK(b.features[0].psi_threshold == 0.25);
    CHECK(b.features[1].psi_threshold == 0.2);   // default applied
    CHECK(b.min_samples == 500);
    CHECK(b.max_seconds == 30);
    // physical bins = interior+3 each (under/over/NaN): (4+3) + (3+3) = 13
    CHECK(b.n_bins_total() == 13);
}

TEST(bundle_window_defaults) {
    // No window block ⇒ defaults applied (R3.3).
    const char* j = R"({"model_id":"m","features":[
      {"name":"f","bin_edges":[0,1,2],"ref_hist":[5,5]}]})";
    Bundle b; std::string err;
    CHECK(parse_bundle(j, b, err));
    CHECK(b.min_samples == DRIFTMON_DEFAULT_MIN_SAMPLES);
    CHECK(b.max_seconds == DRIFTMON_DEFAULT_MAX_SECONDS);
}

// Each violation must be REJECTED with a clear error (gate), not silently run.
static bool rejected(const std::string& json, std::string& err) {
    Bundle b;
    return !parse_bundle(json, b, err) && !err.empty();
}

TEST(bundle_gate_rejects_violations) {
    std::string err;
    // missing model_id
    CHECK(rejected(R"({"features":[{"name":"f","bin_edges":[0,1,2],"ref_hist":[5,5]}]})", err));
    // bin_edges length != ref_hist length + 1
    CHECK(rejected(R"({"model_id":"m","features":[{"name":"f","bin_edges":[0,1],"ref_hist":[5,5]}]})", err));
    // non-increasing edges
    CHECK(rejected(R"({"model_id":"m","features":[{"name":"f","bin_edges":[0,2,1],"ref_hist":[5,5]}]})", err));
    // empty ref_hist
    CHECK(rejected(R"({"model_id":"m","features":[{"name":"f","bin_edges":[0],"ref_hist":[]}]})", err));
    // negative count
    CHECK(rejected(R"({"model_id":"m","features":[{"name":"f","bin_edges":[0,1,2],"ref_hist":[-1,5]}]})", err));
    // duplicate index
    CHECK(rejected(R"({"model_id":"m",
      "features":[{"name":"a","index":0,"bin_edges":[0,1,2],"ref_hist":[5,5]}],
      "outputs":[{"name":"b","index":0,"bin_edges":[0,1,2],"ref_hist":[5,5]}]})", err));
    // no features at all
    CHECK(rejected(R"({"model_id":"m","features":[]})", err));
    // trailing garbage
    CHECK(rejected(R"({"model_id":"m","features":[{"name":"f","bin_edges":[0,1,2],"ref_hist":[5,5]}]} junk)", err));
}

int main() { return RUN_ALL(); }
