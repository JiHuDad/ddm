// check_reference.cpp — integration test helper.
// Loads a reference.json via the core library and reports success/failure.
// Used by the CMake 'reference_roundtrip' test (DRIFTMON_ENABLE_TOOLS=ON).
//
// Usage: check_reference <path-to-reference.json>
// Exit 0 = success, 1 = load failure, 2 = bad arguments.
#include "driftmon.h"

#include <cstdio>

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: check_reference <reference.json>\n");
        return 2;
    }
    const char* path = argv[1];
    driftmon_t* m = driftmon_create(path);
    if (m == nullptr) {
        std::fprintf(stderr, "FAIL: driftmon_create returned NULL for '%s'\n", path);
        return 1;
    }
    std::printf("OK: %d feature(s) loaded from '%s'\n",
                driftmon_num_features(m), path);
    driftmon_destroy(m);
    return 0;
}
