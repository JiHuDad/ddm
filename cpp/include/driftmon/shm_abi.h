/*
 * shm_abi.h — driftmon-cpp shared-memory binary ABI (SPEC §6.2).
 *
 * THE CONTRACT between the tap (writer, inside the serving .so) and the drift
 * worker (reader, separate process). Both compile this exact header; any layout
 * change MUST bump DRIFTMON_SHM_VERSION (attach is refused on mismatch).
 *
 * Mapped structs are POD / standard-layout. The only "rich" members are
 * std::atomic<uintN_t>, which must be ALWAYS lock-free for cross-process use
 * (asserted below) — a lock-free atomic is just the underlying integer plus
 * atomic CPU ops, safe to place in shared memory two processes map.
 *
 * Phase 1 is single-model (one slot). The arena is laid out as an array so
 * Phase 2 multi-model is a constant bump, not a redesign.
 */
#ifndef DRIFTMON_SHM_ABI_H
#define DRIFTMON_SHM_ABI_H

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace driftmon {

// "DRFT" + version nibble; arbitrary 64-bit sentinel to reject foreign mappings.
inline constexpr uint64_t DRIFTMON_SHM_MAGIC   = 0x4452465431000001ULL;
inline constexpr uint32_t DRIFTMON_SHM_VERSION = 1;   // bump on ANY layout change

// Hardcoded, NOT std::hardware_destructive_interference_size — that value varies
// by compiler/flags and would silently skew the ABI between tap and worker.
inline constexpr size_t   DRIFTMON_CACHELINE   = 64;

inline constexpr uint32_t DRIFTMON_MAX_MODELID  = 64;
inline constexpr uint32_t DRIFTMON_NUM_BUFFERS  = 2;     // double buffer
inline constexpr uint32_t DRIFTMON_MAX_SLOTS    = 1;     // Phase 1: single model
inline constexpr uint32_t DRIFTMON_MAX_FEATURES = 256;   // inputs + outputs

// Per-feature physical bins = interior bins + 2 (underflow at 0, overflow at B+1).
// R1.5: out-of-range values are NOT clamped — boundary escape is a drift signal.
inline constexpr uint32_t DRIFTMON_EXTRA_BINS = 2;

// ---- Arena header: at byte offset 0 of the mapped region --------------------
struct ArenaHeader {
    uint64_t magic;                              // DRIFTMON_SHM_MAGIC
    uint32_t version;                            // DRIFTMON_SHM_VERSION
    uint32_t slot_count;                         // active slots (1 in Phase 1)
    uint64_t slot_offset[DRIFTMON_MAX_SLOTS];    // byte offset of each slot
};

// ---- Per-model slot header --------------------------------------------------
// Hot atomics live on their own cache line (alignas) so writer RMWs don't false-
// share with the cold structural metadata above. The two uint64 counter buffers
// are NOT members (n_bins_total is runtime-variable); they are appended after
// the header and reached via hist_buffer() below.
struct SlotHeader {
    char     model_id[DRIFTMON_MAX_MODELID];     // NUL-terminated
    uint32_t n_features;                         // n_inputs + n_outputs
    uint32_t n_bins_total;                       // Σ (interior_bins + 2)
    uint32_t bin_offset[DRIFTMON_MAX_FEATURES + 1];  // prefix sums; [n_features]=total

    // ---- control line: hot atomics, isolated from the cold metadata above ----
    alignas(DRIFTMON_CACHELINE)
    std::atomic<uint64_t> seq;                   // seqlock: even=stable, odd=swapping.
                                                 // NOT load-bearing for counts (see SPEC);
                                                 // guards structural/snapshot reads only.
    std::atomic<uint32_t> active_idx;            // 0|1: buffer the WRITER targets
    std::atomic<uint32_t> inflight[DRIFTMON_NUM_BUFFERS];  // writers mid-update per buffer
    std::atomic<uint64_t> sample_count[DRIFTMON_NUM_BUFFERS];
};

// std::atomic<uint64_t> counter element type for the histogram buffers.
using Counter = std::atomic<uint64_t>;

// ---- Layout math ------------------------------------------------------------
inline constexpr size_t round_up(size_t n, size_t a) { return (n + a - 1) / a * a; }

// Header rounded up to a cache line so buffer 0 starts aligned.
inline constexpr size_t slot_header_padded_size() {
    return round_up(sizeof(SlotHeader), DRIFTMON_CACHELINE);
}

// Per-buffer element stride, padded so buffer 1 also starts on a cache line.
inline constexpr size_t bins_stride(uint32_t n_bins_total) {
    return round_up(static_cast<size_t>(n_bins_total) * sizeof(uint64_t),
                    DRIFTMON_CACHELINE) / sizeof(uint64_t);
}

inline constexpr size_t slot_bytes(uint32_t n_bins_total) {
    return slot_header_padded_size()
         + DRIFTMON_NUM_BUFFERS * bins_stride(n_bins_total) * sizeof(uint64_t);
}

inline constexpr size_t arena_bytes(uint32_t n_bins_total) {
    return round_up(sizeof(ArenaHeader), DRIFTMON_CACHELINE)
         + DRIFTMON_MAX_SLOTS * slot_bytes(n_bins_total);
}

// Pointer to counter buffer `buf` of a slot (buf in [0, NUM_BUFFERS)).
inline Counter* hist_buffer(SlotHeader* s, uint32_t buf) {
    auto* base = reinterpret_cast<unsigned char*>(s) + slot_header_padded_size();
    return reinterpret_cast<Counter*>(
        base + buf * bins_stride(s->n_bins_total) * sizeof(uint64_t));
}
inline const Counter* hist_buffer(const SlotHeader* s, uint32_t buf) {
    auto* base = reinterpret_cast<const unsigned char*>(s) + slot_header_padded_size();
    return reinterpret_cast<const Counter*>(
        base + buf * bins_stride(s->n_bins_total) * sizeof(uint64_t));
}

inline SlotHeader* slot_at(ArenaHeader* a, uint32_t i) {
    return reinterpret_cast<SlotHeader*>(
        reinterpret_cast<unsigned char*>(a) + a->slot_offset[i]);
}

// ---- ABI safety: caught at compile time -------------------------------------
static_assert(std::is_standard_layout<ArenaHeader>::value, "ArenaHeader must be POD-mappable");
static_assert(std::is_standard_layout<SlotHeader>::value,  "SlotHeader must be POD-mappable");
static_assert(std::atomic<uint64_t>::is_always_lock_free,  "need lock-free u64 atomics for shm");
static_assert(std::atomic<uint32_t>::is_always_lock_free,  "need lock-free u32 atomics for shm");
// Hot control block must begin on its own cache line.
static_assert(offsetof(SlotHeader, seq) % DRIFTMON_CACHELINE == 0, "seq not cache-aligned");
// Golden sizes — any accidental layout change fails the build (and test_shm_abi).
static_assert(sizeof(ArenaHeader) == 24, "ArenaHeader layout changed — bump SHM_VERSION");
static_assert(sizeof(SlotHeader) == 1216, "SlotHeader layout changed — bump SHM_VERSION");

}  // namespace driftmon

#endif  // DRIFTMON_SHM_ABI_H
