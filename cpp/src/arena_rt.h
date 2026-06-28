/*
 * arena_rt.h — lock-free writer/reader primitives shared by the tap and tests.
 *
 * Keeping the writer drain-guard in ONE inline place means the concurrency test
 * exercises the exact code the tap runs (not a lookalike). All operations are
 * pure atomics on the mapped slot — no malloc/lock/throw/syscall (hot-path safe).
 */
#ifndef DRIFTMON_ARENA_RT_H
#define DRIFTMON_ARENA_RT_H

#include <atomic>

#include "driftmon/shm_abi.h"

namespace driftmon {

// Enter a writer critical section: pick the active buffer and mark in-flight on
// it, then RE-CHECK the active index. If the worker swapped under us, back out
// and retry so we never write a buffer the worker is about to freeze+zero.
// Returns the buffer index the caller must use until writer_release().
inline uint32_t writer_acquire(SlotHeader* s) noexcept {
    for (;;) {
        const uint32_t idx = s->active_idx.load(std::memory_order_seq_cst);
        s->inflight[idx].fetch_add(1, std::memory_order_seq_cst);
        if (s->active_idx.load(std::memory_order_seq_cst) == idx) return idx;
        s->inflight[idx].fetch_sub(1, std::memory_order_seq_cst);  // swapped; retry
    }
}

// Leave the writer critical section. Caller bumps sample_count itself (it knows
// whether a whole sample completed); release ordering publishes the increments.
inline void writer_release(SlotHeader* s, uint32_t idx) noexcept {
    s->inflight[idx].fetch_sub(1, std::memory_order_release);
}

}  // namespace driftmon

#endif  // DRIFTMON_ARENA_RT_H
