/*
 * tap.h — driftmon-cpp tap API (SPEC §6.1).
 *
 * Included/linked by the serving .so. Adds a sample-per-inference O(1) tap that
 * accumulates input/output distributions into a POSIX shm arena owned by the
 * drift worker. The hot-path functions (tap_update_*) are noexcept and perform
 * ONLY bin-index compute + atomic counter increment — no malloc, no lock, no
 * I/O, no syscall, no throw (NFR1/NFR6, verified by tests).
 *
 * Degrade-to-no-op (R1.4): if the worker isn't up, the bundle can't be read, or
 * the arena layout mismatches, tap_init returns false and every tap_update_* is
 * a safe no-op. The serving path must never block or crash because of the tap.
 */
#ifndef DRIFTMON_TAP_H
#define DRIFTMON_TAP_H

#include <cstddef>

namespace driftmon {

// One-time setup at serving-process start. Loads the reference bundle for
// `model_id`, attaches the worker's shm slot, and validates layout agreement.
//   model_id:     identifier matching the bundle's "model_id"
//   bundle_path:  path to the reference bundle JSON (read once, here — never on
//                 the hot path); edges are kept process-local for cache locality.
// Returns true if the tap is live; false ⇒ no-op mode (serving unaffected).
bool tap_init(const char* model_id, const char* bundle_path);

// Per-inference hot path. `feat`/`out` point to the MODEL INPUT/OUTPUT space
// (post-preprocess), length `n`. O(1) per element. Safe to call in no-op mode.
void tap_update_input(const float* feat, size_t n) noexcept;
void tap_update_output(const float* out, size_t n) noexcept;

// Optional teardown at serving-process shutdown. Detaches the arena. Safe to
// call even if tap_init failed or was never called.
void tap_shutdown() noexcept;

}  // namespace driftmon

#endif  // DRIFTMON_TAP_H
