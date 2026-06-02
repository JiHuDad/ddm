// json_min.cpp — minimal JSON parser for reference.json (Phase 0 stub).
//
// See SPEC.md "Data Format" for the exact schema this must accept.
// Phase 1: implement a small recursive-descent parser over the limited schema
// (objects, arrays, strings, numbers) — no general JSON support needed.
#include "json_min.h"

namespace dm {

bool load_reference(const std::string& path, ReferenceProfile& out) {
    (void)path;
    (void)out;
    // Phase 1: TODO — parse reference.json into `out`, validate that every
    // feature has edges.size() == ref_ratios.size() + 1 and ratios sum ~1.0.
    return false;
}

}  // namespace dm
