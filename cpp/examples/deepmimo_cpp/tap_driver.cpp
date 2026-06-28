// tap_driver.cpp — driftmon-cpp E2E demo: a stand-in "serving process".
//
// Links the real tap and replays a CSV of feature rows through
// tap_update_input — exactly as a serving .so would feed model-input vectors.
// The framework is engine-agnostic, so no ONNX model is needed: a CSV of
// realistic feature values (e.g. DeepMIMO Zone A / Zone E channel stats) is a
// faithful stand-in for the post-preprocess model-input space.
//
// The drift worker (separate process) must already be running (it owns the shm
// arena). If it isn't, tap_init returns false and the tap degrades to no-op —
// the driver reports that and exits nonzero so the demo can fail loudly.
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "driftmon/tap.h"
#include "bundle.h"   // reuse the bundle parser to learn model_id + input order

namespace {

std::vector<std::string> split_csv(const std::string& line_in) {
    // Strip a trailing CR (Python csv writes CRLF) so the last column matches.
    std::string line = line_in;
    while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
    std::vector<std::string> out;
    std::stringstream ss(line);
    std::string cell;
    while (std::getline(ss, cell, ',')) out.push_back(cell);
    return out;
}

}  // namespace

int main(int argc, char** argv) {
    std::string bundle_path, csv_path;
    int loops = 1;
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--bundle" && i + 1 < argc) bundle_path = argv[++i];
        else if (std::string(argv[i]) == "--csv" && i + 1 < argc) csv_path = argv[++i];
        else if (std::string(argv[i]) == "--loops" && i + 1 < argc) loops = std::atoi(argv[++i]);
        else { std::fprintf(stderr, "unknown arg: %s\n", argv[i]); return 2; }
    }
    if (bundle_path.empty() || csv_path.empty()) {
        std::fprintf(stderr, "usage: %s --bundle <json> --csv <csv> [--loops N]\n", argv[0]);
        return 2;
    }

    // Learn model_id and the ordered input feature names from the bundle.
    driftmon::Bundle b;
    std::string err;
    if (!driftmon::load_bundle(bundle_path, b, err)) {
        std::fprintf(stderr, "bundle load failed: %s\n", err.c_str());
        return 1;
    }
    std::vector<std::string> inputs;
    for (const auto& f : b.features)
        if (!f.is_output) inputs.push_back(f.name);

    // Attach the tap (worker must be up). Failure ⇒ no-op mode ⇒ demo fails.
    if (!driftmon::tap_init(b.model_id.c_str(), bundle_path.c_str())) {
        std::fprintf(stderr, "tap_init failed (is the worker running?) — model=%s\n",
                     b.model_id.c_str());
        return 1;
    }

    std::ifstream f(csv_path);
    if (!f) { std::fprintf(stderr, "cannot open csv: %s\n", csv_path.c_str()); return 1; }
    std::string header;
    if (!std::getline(f, header)) { std::fprintf(stderr, "empty csv\n"); return 1; }
    const std::vector<std::string> cols = split_csv(header);

    // Map each input feature to its CSV column index.
    std::vector<int> col_of(inputs.size(), -1);
    for (size_t k = 0; k < inputs.size(); ++k)
        for (size_t c = 0; c < cols.size(); ++c)
            if (cols[c] == inputs[k]) { col_of[k] = static_cast<int>(c); break; }
    for (size_t k = 0; k < inputs.size(); ++k)
        if (col_of[k] < 0) {
            std::fprintf(stderr, "csv missing input column '%s'\n", inputs[k].c_str());
            driftmon::tap_shutdown();
            return 1;
        }

    // Read all rows once, then replay `loops` times (to exceed min_samples).
    std::vector<std::vector<float>> rows;
    std::string line;
    while (std::getline(f, line)) {
        if (line.empty()) continue;
        const std::vector<std::string> cells = split_csv(line);
        std::vector<float> feats(inputs.size());
        bool ok = true;
        for (size_t k = 0; k < inputs.size(); ++k) {
            try { feats[k] = std::stof(cells.at(col_of[k])); }
            catch (...) { ok = false; break; }
        }
        if (ok) rows.push_back(std::move(feats));
    }

    long fed = 0;
    for (int l = 0; l < loops; ++l)
        for (const auto& r : rows) {
            driftmon::tap_update_input(r.data(), r.size());
            ++fed;
        }

    driftmon::tap_shutdown();
    std::fprintf(stderr, "tap_driver: fed %ld samples (model=%s) from %s\n",
                 fed, b.model_id.c_str(), csv_path.c_str());
    return 0;
}
