/*
 * tap_state.h — internal tap state shared by the cold (tap_init.cpp) and hot
 * (tap_hot.cpp) translation units.
 *
 * The split is deliberate: tap_init.cpp owns the storage and does all
 * allocation/I/O; tap_hot.cpp only READS the raw pointers below, so its object
 * file carries NO references to malloc/new/throw — which the symbol-audit test
 * asserts (NFR1/NFR6).
 */
#ifndef DRIFTMON_TAP_STATE_H
#define DRIFTMON_TAP_STATE_H

#include <cstdint>
#include <vector>

#include "shm_arena.h"

namespace driftmon {
namespace detail {

struct TapState {
    bool live = false;
    Arena arena;
    SlotHeader* slot = nullptr;

    uint32_t n_features = 0;   // inputs + outputs
    uint32_t n_inputs = 0;

    // Process-local hot-path data; the hot path reads only the raw pointers.
    std::vector<float>    edges;       // concatenated bin_edges (interior+1 per feature)
    std::vector<uint32_t> edges_off;   // length n_features+1
    std::vector<uint32_t> bin_base;    // length n_features
    const float*    edges_ptr     = nullptr;
    const uint32_t* edges_off_ptr = nullptr;
    const uint32_t* bin_base_ptr  = nullptr;
};

// Storage defined in tap_init.cpp (the cold TU) so tap_hot.o has no static
// ctor/dtor for the std::vector members (which would pull in new/delete).
extern TapState g_tap;

}  // namespace detail
}  // namespace driftmon

#endif  // DRIFTMON_TAP_STATE_H
