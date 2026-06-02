// json_min.h — minimal JSON parser for driftmon's reference.json ONLY.
//
// This is NOT a general-purpose JSON library. It parses exactly the limited
// schema documented in SPEC.md ("Data Format"). Keeping it tiny preserves the
// zero-dependency, standard-library-only philosophy of the core.
//
// Phase 0: declarations + stub. Phase 1 fills in the parser.
#ifndef DRIFTMON_JSON_MIN_H
#define DRIFTMON_JSON_MIN_H

#include <string>
#include <vector>

// Internal namespace is `dm`, not `driftmon`, to avoid colliding with the
// opaque `struct driftmon` tag fixed by the frozen public header.
namespace dm {

// Per-feature reference profile: bucket edges and the reference ratios
// (fraction of reference samples falling in each bucket).
struct FeatureRef {
    std::string name;
    std::vector<double> edges;       // size = num_buckets + 1
    std::vector<double> ref_ratios;  // size = num_buckets, sums to ~1.0
};

// Full reference profile loaded from reference.json.
struct ReferenceProfile {
    int version = 0;
    int window_size = 0;
    std::vector<FeatureRef> features;
};

// Parse the reference.json at `path` into `out`.
// Returns true on success; false on I/O error, parse error, or schema mismatch.
// Phase 0: stub returns false (no parsing yet). Phase 1: implement.
bool load_reference(const std::string& path, ReferenceProfile& out);

}  // namespace dm

#endif  // DRIFTMON_JSON_MIN_H
