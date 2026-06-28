// tap_init.cpp — driftmon-cpp tap COLD PATH (setup/teardown).
//
// Owns the g_tap storage and does all file I/O / allocation / arena attach. The
// hot path (tap_hot.cpp) only reads what this sets up. Degrade-to-no-op (R1.4):
// any failure leaves g_tap.live == false and every tap_update_* is a safe no-op.
#include "driftmon/tap.h"

#include <string>

#include "bundle.h"
#include "shm_arena.h"
#include "tap_state.h"

namespace driftmon {
namespace detail {

TapState g_tap;   // storage (kept out of the hot TU on purpose)

}  // namespace detail

bool tap_init(const char* model_id, const char* bundle_path) {
    detail::g_tap = detail::TapState{};   // idempotent reset
    if (model_id == nullptr || bundle_path == nullptr) return false;

    Bundle b;
    std::string err;
    if (!load_bundle(bundle_path, b, err)) return false;   // R4.2 gate → no-op
    if (b.model_id != model_id) return false;              // bundle/serving mismatch

    detail::TapState& t = detail::g_tap;
    t.n_features = static_cast<uint32_t>(b.features.size());
    t.n_inputs = 0;
    for (const auto& f : b.features) if (!f.is_output) ++t.n_inputs;

    t.edges_off.assign(t.n_features + 1, 0);
    t.bin_base.assign(t.n_features, 0);
    uint32_t bin_acc = 0;
    for (uint32_t f = 0; f < t.n_features; ++f) {
        const auto& feat = b.features[f];
        t.bin_base[f] = bin_acc;
        bin_acc += static_cast<uint32_t>(feat.interior_bins()) + DRIFTMON_EXTRA_BINS;
        t.edges_off[f + 1] = t.edges_off[f] + static_cast<uint32_t>(feat.bin_edges.size());
    }
    t.edges.reserve(t.edges_off[t.n_features]);
    for (const auto& feat : b.features)
        for (double ev : feat.bin_edges)
            t.edges.push_back(static_cast<float>(ev));

    const uint32_t n_bins_total = static_cast<uint32_t>(b.n_bins_total());
    if (!arena_attach(t.arena, arena_name(b.model_id), n_bins_total, err))
        return false;                                      // R1.4 → no-op

    t.slot = t.arena.slot(0);
    t.edges_ptr     = t.edges.data();
    t.edges_off_ptr = t.edges_off.data();
    t.bin_base_ptr  = t.bin_base.data();
    t.live = true;
    return true;
}

void tap_shutdown() noexcept {
    if (detail::g_tap.arena.valid()) arena_detach(detail::g_tap.arena);
    detail::g_tap.live = false;
    detail::g_tap.slot = nullptr;
}

}  // namespace driftmon
