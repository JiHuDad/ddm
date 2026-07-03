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
// v2: NaN bin per feature (EXTRA_BINS 2→3) + sample-ring region for raw
// feature-vector preservation (retraining material) + reserved header space.
inline constexpr uint32_t DRIFTMON_SHM_VERSION = 2;   // bump on ANY layout change

// Hardcoded, NOT std::hardware_destructive_interference_size — that value varies
// by compiler/flags and would silently skew the ABI between tap and worker.
inline constexpr size_t   DRIFTMON_CACHELINE   = 64;

inline constexpr uint32_t DRIFTMON_MAX_MODELID  = 64;
inline constexpr uint32_t DRIFTMON_NUM_BUFFERS  = 2;     // double buffer
inline constexpr uint32_t DRIFTMON_MAX_SLOTS    = 1;     // Phase 1: single model
inline constexpr uint32_t DRIFTMON_MAX_FEATURES = 256;   // inputs + outputs

// Per-feature physical bins = interior bins + 3:
//   [0]=underflow, [1..B]=interior, [B+1]=overflow, [B+2]=NaN.
// R1.5: out-of-range values are NOT clamped — boundary escape is a drift signal.
// The NaN bin (v2) is a DATA-QUALITY signal, not part of the value distribution:
// detectors exclude it; the worker alarms on its ratio separately. A broken
// upstream preprocessor flooding NaNs must not look like a healthy histogram.
inline constexpr uint32_t DRIFTMON_EXTRA_BINS = 3;

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
    uint32_t n_bins_total;                       // Σ (interior_bins + 3)
    uint32_t bin_offset[DRIFTMON_MAX_FEATURES + 1];  // prefix sums; [n_features]=total

    // ---- v2: sample ring config (cold; set by the worker at create time) ----
    // The ring preserves raw INPUT feature vectors (1-in-sample_every systematic
    // sampling) so an alarm can be dumped as retraining material — histograms
    // alone cannot reconstruct training data. ring_rows==0 disables the ring.
    uint32_t ring_rows;                          // capacity in rows (0 = off)
    uint32_t ring_row_stride;                    // bytes per row (8B rowseq + floats, 8B-aligned)
    uint32_t ring_offset;                        // byte offset of ring from slot base
    uint32_t sample_every;                       // tap samples every Nth input vector
    uint32_t n_inputs;                           // floats per ring row
    uint32_t reserved[7];                        // future (pairwise 2D hists, …)

    // ---- control line: hot atomics, isolated from the cold metadata above ----
    alignas(DRIFTMON_CACHELINE)
    std::atomic<uint64_t> seq;                   // seqlock: even=stable, odd=swapping.
                                                 // NOT load-bearing for counts (see SPEC);
                                                 // guards structural/snapshot reads only.
    std::atomic<uint32_t> active_idx;            // 0|1: buffer the WRITER targets
    std::atomic<uint32_t> inflight[DRIFTMON_NUM_BUFFERS];  // writers mid-update per buffer
    std::atomic<uint64_t> sample_count[DRIFTMON_NUM_BUFFERS];
    std::atomic<uint64_t> ring_head;             // v2: monotonically increasing row claims
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

// Ring row = 8-byte rowseq (mini-seqlock) + n_inputs floats, kept 8B-aligned.
inline constexpr uint32_t ring_row_stride_for(uint32_t n_inputs) {
    return static_cast<uint32_t>(round_up(8 + n_inputs * sizeof(float), 8));
}

inline constexpr size_t ring_bytes(uint32_t ring_rows, uint32_t ring_row_stride) {
    return round_up(static_cast<size_t>(ring_rows) * ring_row_stride,
                    DRIFTMON_CACHELINE);
}

inline constexpr size_t slot_bytes(uint32_t n_bins_total, uint32_t ring_rows = 0,
                                   uint32_t ring_row_stride = 0) {
    return slot_header_padded_size()
         + DRIFTMON_NUM_BUFFERS * bins_stride(n_bins_total) * sizeof(uint64_t)
         + ring_bytes(ring_rows, ring_row_stride);
}

inline constexpr size_t arena_bytes(uint32_t n_bins_total, uint32_t ring_rows = 0,
                                    uint32_t ring_row_stride = 0) {
    return round_up(sizeof(ArenaHeader), DRIFTMON_CACHELINE)
         + DRIFTMON_MAX_SLOTS * slot_bytes(n_bins_total, ring_rows, ring_row_stride);
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

// Sample-ring row accessors. A row is [atomic<u64> rowseq][n_inputs floats];
// rowseq protocol: writer stores 2k+1 (in progress), copies floats, then 2k+2
// (complete, release) where k is the claim from ring_head. Readers skip rows
// whose rowseq is odd or changes across the copy.
inline std::atomic<uint64_t>* ring_rowseq(SlotHeader* s, uint32_t row) {
    return reinterpret_cast<std::atomic<uint64_t>*>(
        reinterpret_cast<unsigned char*>(s) + s->ring_offset
        + static_cast<size_t>(row) * s->ring_row_stride);
}
inline float* ring_rowdata(SlotHeader* s, uint32_t row) {
    return reinterpret_cast<float*>(
        reinterpret_cast<unsigned char*>(s) + s->ring_offset
        + static_cast<size_t>(row) * s->ring_row_stride + 8);
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
