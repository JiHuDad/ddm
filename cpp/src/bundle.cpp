// bundle.cpp — driftmon-cpp reference bundle parser/validator (SPEC §5.4).
//
// Reuses the audited dm::JsonParser primitives from the core (json_min.h);
// only the object shapes here are bundle-specific. Standard library only.
#include "bundle.h"

#include <fstream>
#include <iterator>
#include <set>

#include "json_min.h"   // dm::JsonParser (core, internal header — NOT the frozen ABI)
#include "driftmon/shm_abi.h"

namespace driftmon {

long Bundle::n_bins_total() const {
    long t = 0;
    for (const auto& f : features)
        t += static_cast<long>(f.interior_bins()) + DRIFTMON_EXTRA_BINS;
    return t;
}

namespace {

// Parse one feature object. `is_output` tags inputs vs outputs in the flat list.
bool parse_feature(dm::JsonParser& jp, BundleFeature& f, bool is_output, int& index_out) {
    if (!jp.consume('{')) return false;
    f.is_output = is_output;
    f.psi_threshold = 0.2;
    index_out = -1;
    bool got_name = false, got_edges = false, got_hist = false;
    while (!jp.peek('}')) {
        std::string key;
        if (!jp.parse_string(key)) return false;
        if (!jp.consume(':')) return false;
        if (key == "name") {
            if (!jp.parse_string(f.name)) return false;
            got_name = true;
        } else if (key == "bin_edges") {
            if (!jp.parse_double_array(f.bin_edges)) return false;
            got_edges = true;
        } else if (key == "ref_hist") {
            if (!jp.parse_int_array(f.ref_hist)) return false;
            got_hist = true;
        } else if (key == "psi_threshold") {
            if (!jp.parse_number(f.psi_threshold)) return false;
        } else if (key == "ks_threshold") {
            if (!jp.parse_number(f.ks_threshold)) return false;
        } else if (key == "index") {
            if (!jp.parse_int(index_out)) return false;
        } else {
            if (!jp.skip_value()) return false;   // dtype, etc.
        }
        if (!jp.consume(',')) break;
    }
    return jp.consume('}') && got_name && got_edges && got_hist;
}

bool parse_feature_array(dm::JsonParser& jp, std::vector<BundleFeature>& features,
                         bool is_output, std::vector<int>& indices) {
    if (!jp.consume('[')) return false;
    if (jp.peek(']')) { jp.consume(']'); return true; }
    do {
        BundleFeature f;
        int idx = -1;
        if (!parse_feature(jp, f, is_output, idx)) return false;
        features.push_back(std::move(f));
        indices.push_back(idx);
    } while (jp.consume(','));
    return jp.consume(']');
}

bool parse_quality(dm::JsonParser& jp, Bundle& b) {
    if (!jp.consume('{')) return false;
    while (!jp.peek('}')) {
        std::string key;
        if (!jp.parse_string(key)) return false;
        if (!jp.consume(':')) return false;
        if (key == "nan_ratio_max") {
            if (!jp.parse_number(b.nan_ratio_max)) return false;
        } else if (key == "oor_ratio_max") {
            if (!jp.parse_number(b.oor_ratio_max)) return false;
        } else {
            if (!jp.skip_value()) return false;
        }
        if (!jp.consume(',')) break;
    }
    return jp.consume('}');
}

bool parse_sampling(dm::JsonParser& jp, Bundle& b) {
    if (!jp.consume('{')) return false;
    while (!jp.peek('}')) {
        std::string key;
        if (!jp.parse_string(key)) return false;
        if (!jp.consume(':')) return false;
        if (key == "every_n") {
            int v; if (!jp.parse_int(v)) return false; b.sample_every = v;
        } else if (key == "ring_rows") {
            int v; if (!jp.parse_int(v)) return false; b.ring_rows = v;
        } else {
            if (!jp.skip_value()) return false;
        }
        if (!jp.consume(',')) break;
    }
    return jp.consume('}');
}

bool parse_window(dm::JsonParser& jp, Bundle& b) {
    if (!jp.consume('{')) return false;
    while (!jp.peek('}')) {
        std::string key;
        if (!jp.parse_string(key)) return false;
        if (!jp.consume(':')) return false;
        if (key == "min_samples") {
            int v; if (!jp.parse_int(v)) return false; b.min_samples = v;
        } else if (key == "max_seconds") {
            int v; if (!jp.parse_int(v)) return false; b.max_seconds = v;
        } else {
            if (!jp.skip_value()) return false;
        }
        if (!jp.consume(',')) break;
    }
    return jp.consume('}');
}

bool parse_string_array(dm::JsonParser& jp, std::vector<std::string>& arr) {
    if (!jp.consume('[')) return false;
    if (jp.peek(']')) { jp.consume(']'); return true; }
    do {
        std::string s;
        if (!jp.parse_string(s)) return false;
        arr.push_back(std::move(s));
    } while (jp.consume(','));
    return jp.consume(']');
}

// Validate per R4.1; on failure set `err` and return false (caller gates).
bool validate(const Bundle& b, const std::vector<int>& indices, std::string& err) {
    if (b.model_id.empty()) { err = "missing required field: model_id"; return false; }
    if (b.features.empty())  { err = "no features (and no outputs) in bundle"; return false; }
    if (b.features.size() > DRIFTMON_MAX_FEATURES) {
        err = "too many features (max " + std::to_string(DRIFTMON_MAX_FEATURES) + ")";
        return false;
    }
    if (b.sample_every < 0 || b.ring_rows < 0) {
        err = "sampling.every_n / ring_rows must be >= 0";
        return false;
    }
    if (b.ring_rows > 0 && b.sample_every == 0) {
        err = "sampling.every_n must be > 0 when ring_rows > 0";
        return false;
    }
    std::set<int> seen_idx;
    for (size_t i = 0; i < b.features.size(); ++i) {
        const auto& f = b.features[i];
        const std::string where = (f.is_output ? "output '" : "feature '") + f.name + "'";
        if (f.ref_hist.empty()) { err = where + ": empty ref_hist"; return false; }
        // R4.1: len(bin_edges) == len(ref_hist) + 1.
        if (f.bin_edges.size() != f.ref_hist.size() + 1) {
            err = where + ": bin_edges length must be ref_hist length + 1";
            return false;
        }
        for (size_t k = 1; k < f.bin_edges.size(); ++k)
            if (f.bin_edges[k] <= f.bin_edges[k - 1]) {
                err = where + ": bin_edges must be strictly increasing";
                return false;
            }
        for (long c : f.ref_hist)
            if (c < 0) { err = where + ": negative ref_hist count"; return false; }
        // `index`, if provided, must be valid (non-negative) and unique.
        const int idx = indices[i];
        if (idx >= 0) {
            if (!seen_idx.insert(idx).second) {
                err = where + ": duplicate feature index " + std::to_string(idx);
                return false;
            }
        }
    }
    return true;
}

}  // namespace

bool parse_bundle(const std::string& json, Bundle& out, std::string& err) {
    Bundle b;
    std::vector<int> in_idx, out_idx;
    dm::JsonParser jp;
    jp.p = json.c_str();
    jp.end = json.c_str() + json.size();

    if (!jp.consume('{')) { err = "bundle is not a JSON object"; return false; }
    while (!jp.peek('}')) {
        std::string key;
        if (!jp.parse_string(key)) { err = "malformed key"; return false; }
        if (!jp.consume(':')) { err = "expected ':'"; return false; }
        bool ok = true;
        if (key == "schema_version")      ok = jp.parse_string(b.schema_version);
        else if (key == "model_id")       ok = jp.parse_string(b.model_id);
        else if (key == "features")       ok = parse_feature_array(jp, b.features, false, in_idx);
        else if (key == "outputs")        ok = parse_feature_array(jp, b.features, true, out_idx);
        else if (key == "window")         ok = parse_window(jp, b);
        else if (key == "quality")        ok = parse_quality(jp, b);
        else if (key == "sampling")       ok = parse_sampling(jp, b);
        else if (key == "tests")          ok = parse_string_array(jp, b.tests);
        else                              ok = jp.skip_value();   // created_at, train_window, mode_tag
        if (!ok) { err = "malformed value for key '" + key + "'"; return false; }
        if (!jp.consume(',')) break;
    }
    if (!jp.consume('}')) { err = "unterminated top-level object"; return false; }
    jp.skip_ws();
    if (jp.p != jp.end) { err = "trailing data after top-level object"; return false; }

    // Inputs were appended first, then outputs — indices line up the same way.
    std::vector<int> indices = std::move(in_idx);
    indices.insert(indices.end(), out_idx.begin(), out_idx.end());

    // Apply window defaults (R3.3) before validation/use.
    if (b.min_samples <= 0) b.min_samples = DRIFTMON_DEFAULT_MIN_SAMPLES;
    if (b.max_seconds <= 0) b.max_seconds = DRIFTMON_DEFAULT_MAX_SECONDS;
    if (b.tests.empty()) b.tests.push_back("psi");   // default detector

    if (!validate(b, indices, err)) return false;
    out = std::move(b);
    return true;
}

bool load_bundle(const std::string& path, Bundle& out, std::string& err) {
    std::ifstream file(path);
    if (!file) { err = "cannot open bundle file: " + path; return false; }
    std::string src((std::istreambuf_iterator<char>(file)),
                     std::istreambuf_iterator<char>());
    return parse_bundle(src, out, err);
}

}  // namespace driftmon
