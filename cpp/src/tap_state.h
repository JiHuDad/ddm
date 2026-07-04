/*
 * tap_state.h — internal tap state shared by the cold (tap_init.cpp) and hot
 * (tap_hot.cpp) translation units.
 *
 * The split is deliberate: tap_init.cpp owns storage and does all allocation /
 * I/O / syscalls; tap_hot.cpp only performs atomic loads on the fields below,
 * so its object file carries NO references to malloc/new/throw/lock/syscall —
 * asserted by the tap_symbol_audit test (NFR1/NFR6).
 *
 * Reconfiguration model (A1 self-heal): the hot path reads `live`, `slot` and
 * `cfg` as atomics. Maintenance (tap_maintain / auto-retry) publishes a NEW
 * TapConfig / arena mapping and never frees the old one while the process
 * lives — a bounded leak (one per reconfiguration event, i.e. per worker
 * bundle update) that buys lock-free hot-path safety without refcounting.
 */
#ifndef DRIFTMON_TAP_STATE_H
#define DRIFTMON_TAP_STATE_H

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

#include "shm_arena.h"

namespace driftmon {
namespace detail {

// Immutable after publish. Hot path dereferences exactly one of these.
struct TapConfig {
    uint32_t n_features = 0;   // inputs + outputs
    uint32_t n_inputs = 0;

    std::vector<float>    edges;       // concatenated bin_edges (interior+1 per feature)
    std::vector<uint32_t> edges_off;   // length n_features+1
    std::vector<uint32_t> bin_base;    // length n_features
    const float*    edges_ptr     = nullptr;
    const uint32_t* edges_off_ptr = nullptr;
    const uint32_t* bin_base_ptr  = nullptr;
};

struct TapState {
    // --- read by the hot path (atomic; release-published by cold code) ---
    std::atomic<bool>        live{false};
    std::atomic<SlotHeader*> slot{nullptr};
    std::atomic<TapConfig*>  cfg{nullptr};    // leaked on replace (bounded)
    std::atomic<uint32_t>    noop_calls{0};   // degraded-mode auto-retry counter
    std::atomic<uint64_t>    sample_ctr{0};   // 1-in-N systematic sampling counter

    // --- cold-side only ---
    Arena arena;                      // current mapping; stale ones stay mapped (leaked)
    std::string model_id;             // for re-init in tap_maintain
    std::string bundle_path;
};

// Storage defined in tap_init.cpp (kept out of the hot TU on purpose).
extern TapState g_tap;

// Degraded-mode auto-retry interval, in tap_update_* calls. Default 1<<20;
// tests may lower it. A retry is a full maintenance pass (syscalls) — it only
// ever runs while the tap is a no-op, so serving latency is unaffected when
// monitoring is healthy.
extern uint32_t g_tap_retry_calls;

// Defined in tap_init.cpp; called by the hot TU while a tap is degraded.
// Rate-limits itself via t.noop_calls and re-initializes that tap when due.
void tap_noop_tick(TapState& t) noexcept;

}  // namespace detail

// Opaque public handle (declared in driftmon/tap.h) = one per-model tap state.
struct TapHandle : detail::TapState {};

}  // namespace driftmon

#endif  // DRIFTMON_TAP_STATE_H
