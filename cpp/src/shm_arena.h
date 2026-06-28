/*
 * shm_arena.h — POSIX shared-memory arena create/attach (SPEC §5.2, §6.2).
 *
 * The drift worker CREATES and owns the arena; serving processes (tap) ATTACH.
 * Layout is fixed by shm_abi.h. Attach refuses on magic/version/n_bins mismatch
 * so a stale or foreign mapping degrades the tap to no-op rather than corrupting.
 */
#ifndef DRIFTMON_SHM_ARENA_H
#define DRIFTMON_SHM_ARENA_H

#include <cstdint>
#include <string>
#include <vector>

#include "driftmon/shm_abi.h"

namespace driftmon {

// Describes the single slot the worker carves for a model (Phase 1).
struct SlotSpec {
    std::string model_id;
    uint32_t n_features = 0;
    uint32_t n_bins_total = 0;
    std::vector<uint32_t> bin_offset;   // length n_features + 1, prefix sums
};

struct Arena {
    int    fd    = -1;
    void*  base  = nullptr;
    size_t bytes = 0;
    std::string name;
    bool   is_owner = false;

    ArenaHeader* header() const { return reinterpret_cast<ArenaHeader*>(base); }
    SlotHeader*  slot(uint32_t i = 0) const { return slot_at(header(), i); }
    bool valid() const { return base != nullptr; }
};

// Worker side: create (or recreate) the arena and initialize header + slot.
bool arena_create(Arena& a, const std::string& name, const SlotSpec& spec,
                  std::string& err);

// Tap side: attach to an existing arena. `expected_n_bins_total` is the tap's
// own bundle-derived value; a mismatch (or bad magic/version) ⇒ false.
bool arena_attach(Arena& a, const std::string& name,
                  uint32_t expected_n_bins_total, std::string& err);

void arena_detach(Arena& a);          // unmap + close (does not unlink)
void arena_unlink(const std::string& name);  // remove the shm name (owner cleanup)

// Canonical shm name for a model — tap and worker must agree.
inline std::string arena_name(const std::string& model_id) {
    return "/driftmon." + model_id;
}

}  // namespace driftmon

#endif  // DRIFTMON_SHM_ARENA_H
