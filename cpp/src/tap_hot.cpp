// tap_hot.cpp — driftmon-cpp tap HOT PATH ONLY (SPEC §5.1, §6.1, NFR1/NFR6).
//
// This translation unit must contain NO malloc/new/free/throw/lock/syscall —
// the tap_symbol_audit test proves it by inspecting this object's undefined
// symbols. Everything here is atomic loads on process-local state plus atomic
// RMWs on the mapped shm buffer. The only cross-TU call is tap_noop_tick(),
// which runs ONLY while the tap is degraded (serving unmonitored anyway) and
// lives in the cold TU.
#include "driftmon/tap.h"

#include "arena_rt.h"
#include "tap_state.h"

namespace driftmon {

namespace {

// Increment the histogram for `count` consecutive features starting at global
// feature index `fbase`, reading values v[0..n). O(1) per element.
inline void tap_write(detail::TapState& t, const float* v, size_t n, bool outputs) noexcept {
    if (!t.live.load(std::memory_order_acquire)) {
        detail::tap_noop_tick(t);                    // degraded: rate-limited self-heal
        return;
    }
    const detail::TapConfig* c = t.cfg.load(std::memory_order_acquire);
    SlotHeader* s = t.slot.load(std::memory_order_acquire);
    if (c == nullptr || s == nullptr) return;

    const uint32_t fbase = outputs ? c->n_inputs : 0;
    const uint32_t count = outputs ? c->n_features - c->n_inputs : c->n_inputs;

    const uint32_t idx = writer_acquire(s);          // drain guard (arena_rt.h)
    Counter* buf = hist_buffer(s, idx);
    const uint32_t m = (n < count) ? static_cast<uint32_t>(n) : count;
    for (uint32_t j = 0; j < m; ++j) {
        const uint32_t f  = fbase + j;
        const float* e    = c->edges_ptr + c->edges_off_ptr[f];
        const uint32_t ne = c->edges_off_ptr[f + 1] - c->edges_off_ptr[f];  // interior+1
        const float x = v[j];
        uint32_t k;                                  // physical bin
        if (!(x == x)) {
            k = ne + 1;                              // NaN bin — data-quality signal (v2)
        } else if (x < e[0]) {
            k = 0;                                   // underflow (R1.5: no clamp)
        } else if (x >= e[ne - 1]) {
            k = ne;                                  // overflow
        } else {
            uint32_t lo = 0, hi = ne - 1;            // upper_bound: first e[lo] > x
            while (lo < hi) {
                const uint32_t mid = (lo + hi) >> 1;
                if (e[mid] <= x) lo = mid + 1; else hi = mid;
            }
            k = lo;                                  // physical interior bin
        }
        buf[c->bin_base_ptr[f] + k].fetch_add(1, std::memory_order_relaxed);
    }
    s->sample_count[idx].fetch_add(1, std::memory_order_relaxed);
    writer_release(s, idx);

    // Sample ring (v2, R-R1): keep 1 in sample_every complete INPUT vectors as
    // retraining material. Per selected row: one fetch_add + n_inputs atomic
    // stores — amortized to ~nothing at 1/N. Row protocol is a mini-seqlock
    // (odd=in progress, even=complete); the worker skips torn/odd rows. Floats
    // are stored as atomic u32 bits so individual values can never tear.
    if (!outputs && s->ring_rows != 0 && s->sample_every != 0 && n >= c->n_inputs) {
        const uint64_t sc = t.sample_ctr.fetch_add(1, std::memory_order_relaxed);
        if (sc % s->sample_every == 0) {
            const uint64_t k = s->ring_head.fetch_add(1, std::memory_order_relaxed);
            const uint32_t row = static_cast<uint32_t>(k % s->ring_rows);
            std::atomic<uint64_t>* rowseq = ring_rowseq(s, row);
            auto* dst = reinterpret_cast<std::atomic<uint32_t>*>(ring_rowdata(s, row));
            rowseq->store(2 * k + 1, std::memory_order_relaxed);   // odd: in progress
            std::atomic_thread_fence(std::memory_order_release);
            for (uint32_t j = 0; j < c->n_inputs; ++j) {
                uint32_t bits;
                __builtin_memcpy(&bits, &v[j], sizeof(bits));      // bit-cast, no libc call
                dst[j].store(bits, std::memory_order_relaxed);
            }
            rowseq->store(2 * k + 2, std::memory_order_release);   // even: complete
        }
    }
}

}  // namespace

void tap_update_input(const float* feat, size_t n) noexcept {
    if (feat == nullptr) return;
    tap_write(detail::g_tap, feat, n, /*outputs=*/false);
}

void tap_update_output(const float* out, size_t n) noexcept {
    if (out == nullptr) return;
    tap_write(detail::g_tap, out, n, /*outputs=*/true);
}

void tap_input(TapHandle* h, const float* feat, size_t n) noexcept {
    if (h == nullptr || feat == nullptr) return;
    tap_write(*h, feat, n, /*outputs=*/false);
}

void tap_output(TapHandle* h, const float* out, size_t n) noexcept {
    if (h == nullptr || out == nullptr) return;
    tap_write(*h, out, n, /*outputs=*/true);
}

}  // namespace driftmon
