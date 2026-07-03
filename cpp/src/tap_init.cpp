// tap_init.cpp — driftmon-cpp tap COLD PATH (setup / maintenance / teardown).
//
// Owns the g_tap storage and does all file I/O / allocation / syscalls. The hot
// path (tap_hot.cpp) only reads the atomics this publishes.
//
// Self-heal (A1): tap_maintain() recovers a degraded tap (worker came up after
// serving) and re-attaches when the worker rebuilt the arena (bundle update —
// detected via arena_is_stale). It is also invoked automatically from the hot
// path while degraded, rate-limited by g_tap_retry_calls, so monitoring comes
// back without any integration change. All cold operations are serialized by a
// mutex; the hot path never takes it.
//
// Reconfiguration safety: hot threads may be inside tap_write while we
// reconfigure. We therefore (a) flip `live` to false before touching anything,
// (b) never munmap an abandoned arena mapping and never delete a replaced
// TapConfig — in-flight writers finish harmlessly against the old (ghost)
// memory. The leak is bounded: one mapping + one config per reconfiguration
// event (i.e. per worker bundle update), not per call.
#include "driftmon/tap.h"

#include <unistd.h>

#include <mutex>
#include <string>

#include "bundle.h"
#include "shm_arena.h"
#include "tap_state.h"

namespace driftmon {
namespace detail {

TapState g_tap;                          // storage (kept out of the hot TU)
uint32_t g_tap_retry_calls = 1u << 20;   // degraded auto-retry interval (calls)

namespace {

std::mutex g_maintain_mu;                // serializes init/maintain/shutdown

// Build an immutable TapConfig from a validated bundle. Heap-allocated so it
// can be atomically published and safely abandoned (never freed) on replace.
TapConfig* build_config(const Bundle& b) {
    auto* c = new TapConfig();
    c->n_features = static_cast<uint32_t>(b.features.size());
    c->n_inputs = 0;
    for (const auto& f : b.features) if (!f.is_output) ++c->n_inputs;

    c->edges_off.assign(c->n_features + 1, 0);
    c->bin_base.assign(c->n_features, 0);
    uint32_t bin_acc = 0;
    for (uint32_t f = 0; f < c->n_features; ++f) {
        const auto& feat = b.features[f];
        c->bin_base[f] = bin_acc;
        bin_acc += static_cast<uint32_t>(feat.interior_bins()) + DRIFTMON_EXTRA_BINS;
        c->edges_off[f + 1] = c->edges_off[f] + static_cast<uint32_t>(feat.bin_edges.size());
    }
    c->edges.reserve(c->edges_off[c->n_features]);
    for (const auto& feat : b.features)
        for (double ev : feat.bin_edges)
            c->edges.push_back(static_cast<float>(ev));

    c->edges_ptr     = c->edges.data();
    c->edges_off_ptr = c->edges_off.data();
    c->bin_base_ptr  = c->bin_base.data();
    return c;
}

// Abandon the current mapping without munmap (in-flight writers may still be
// inside it); close the fd so descriptors don't accumulate.
void abandon_arena_locked() {
    if (g_tap.arena.fd >= 0) ::close(g_tap.arena.fd);
    g_tap.arena = Arena{};   // mapping intentionally leaked (bounded)
}

// Full (re)initialization from g_tap.model_id / bundle_path. Caller holds the
// maintenance mutex. Returns true if the tap ends up live.
bool reinit_locked() {
    g_tap.live.store(false, std::memory_order_release);   // degrade during swap

    Bundle b;
    std::string err;
    if (!load_bundle(g_tap.bundle_path, b, err)) return false;   // R4.2 gate → no-op
    if (b.model_id != g_tap.model_id) return false;              // bundle/serving mismatch

    Arena fresh;
    const uint32_t n_bins_total = static_cast<uint32_t>(b.n_bins_total());
    if (!arena_attach(fresh, arena_name(b.model_id), n_bins_total, err))
        return false;                                            // R1.4 → stay no-op

    abandon_arena_locked();
    g_tap.arena = fresh;

    TapConfig* cfg = build_config(b);                            // old cfg abandoned
    g_tap.cfg.store(cfg, std::memory_order_release);
    g_tap.slot.store(g_tap.arena.slot(0), std::memory_order_release);
    g_tap.live.store(true, std::memory_order_release);
    return true;
}

}  // namespace

void tap_noop_tick() noexcept {
    // Degraded-mode self-heal: about once every g_tap_retry_calls no-op calls,
    // one caller pays for a recovery attempt. Exact-match on the counter so
    // concurrent threads elect a single retrier.
    const uint32_t n = g_tap.noop_calls.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n % g_tap_retry_calls == 0) (void)tap_maintain();
}

}  // namespace detail

bool tap_init(const char* model_id, const char* bundle_path) {
    if (model_id == nullptr || bundle_path == nullptr) return false;
    std::lock_guard<std::mutex> lk(detail::g_maintain_mu);
    detail::g_tap.model_id = model_id;
    detail::g_tap.bundle_path = bundle_path;
    return detail::reinit_locked();
}

bool tap_maintain() noexcept {
    try {
        std::lock_guard<std::mutex> lk(detail::g_maintain_mu);
        if (detail::g_tap.bundle_path.empty()) return false;   // never initialized
        if (detail::g_tap.live.load(std::memory_order_acquire) &&
            !arena_is_stale(detail::g_tap.arena))
            return true;                                       // healthy — nothing to do
        return detail::reinit_locked();                        // degraded or stale
    } catch (...) {
        return false;   // allocation failure etc. — stay degraded, never throw
    }
}

void tap_shutdown() noexcept {
    // Caller contract: invoke only when serving threads are quiesced (no
    // concurrent tap_update_* in flight) — this unmaps the arena for real.
    std::lock_guard<std::mutex> lk(detail::g_maintain_mu);
    detail::g_tap.live.store(false, std::memory_order_release);
    detail::g_tap.slot.store(nullptr, std::memory_order_release);
    if (detail::g_tap.arena.valid()) arena_detach(detail::g_tap.arena);
    // cfg intentionally not freed (a late reader may still hold it).
}

}  // namespace driftmon
