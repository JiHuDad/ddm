/*
 * worker.h — driftmon-cpp drift worker (SPEC §5.3).
 *
 * The worker creates+owns the shm arena, loads the reference bundle, and loops:
 * swap the active buffer, read+zero the frozen one, accumulate into the current
 * window, and — once the window closes by sample count — run detectors. Below
 * min_samples the verdict is withheld (warm-up guard, G5/AC5).
 *
 * The pieces are exposed as testable units (window decision is a pure function;
 * the swap-read primitive is standalone) rather than buried in a main loop.
 */
#ifndef DRIFTMON_WORKER_H
#define DRIFTMON_WORKER_H

#include <memory>
#include <string>
#include <vector>

#include "bundle.h"
#include "detector.h"
#include "driftmon/shm_abi.h"
#include "shm_arena.h"

namespace driftmon {

// --- Pure helpers (no shm, no time) -----------------------------------------

// Build the shm slot spec (model_id, feature/bin counts, bin_offset prefix)
// from a validated bundle.
SlotSpec slot_spec_from_bundle(const Bundle& b);

// Build per-feature detector references over the PHYSICAL bin layout (interior
// + underflow/overflow). ref_ratios[0] (underflow) and [B+1] (overflow) are 0:
// training data lay within edges, so any runtime out-of-range mass is drift.
std::vector<FeatureRef> feature_refs_from_bundle(const Bundle& b);

enum class WindowDecision {
    Accumulate,   // keep collecting (window open, below min_samples, time left)
    Evaluate,     // window closed by reaching min_samples → produce a verdict
    WarmupClose,  // window closed by time but still below min_samples → WITHHOLD
};

// SPEC R3.3 / AC5: a window closes at min_samples OR max_seconds, whichever
// first; min_samples gates the VERDICT regardless of which closed it.
WindowDecision window_decision(long accumulated_samples, double elapsed_seconds,
                               long min_samples, long max_seconds);

// --- Swap + read (the lock-free reader side, SPEC §6.2) ----------------------

// Freeze the active buffer (publish a swap), drain in-flight writers on the old
// buffer (bounded by grace_spins), read it into `out` (one Histogram per
// feature, physical bins), then zero it for reuse. Returns the frozen buffer's
// sample_count. `grace_spins`==0 means spin unboundedly until drained.
uint64_t slot_swap_read(SlotHeader* s, std::vector<Histogram>& out,
                        uint64_t grace_spins = 0);

// --- Per-model monitor -------------------------------------------------------

struct ModelVerdict {
    bool produced = false;       // false while accumulating or warming up
    bool warming_up = false;     // window closed by time but below min_samples
    long window_samples = 0;     // samples in the closed/active window
    double max_score = 0.0;
    bool alarm = false;
    std::vector<DriftResult> per_feature;
};

class ModelMonitor {
public:
    // Create (or, on worker restart, reuse — AC6) the arena and build the
    // detectors named in bundle.tests for each feature.
    bool init(const Bundle& b, std::string& err);
    bool reused_existing_arena() const { return reused_arena_; }

    // One worker tick: swap-read the slot, accumulate, and decide using the
    // elapsed time since the current window opened. Produces a verdict when the
    // window closes (or withholds while warming up); otherwise keeps accumulating.
    ModelVerdict tick(double elapsed_seconds);

    SlotHeader* slot() { return arena_.slot(0); }
    Arena& arena() { return arena_; }
    const Bundle& bundle() const { return bundle_; }

    ~ModelMonitor();

private:
    void reset_window();
    ModelVerdict evaluate();

    Bundle bundle_;
    Arena arena_;
    std::vector<FeatureRef> refs_;
    // Per-feature list of detectors (PSI/KS/... selected by bundle.tests).
    std::vector<std::vector<std::unique_ptr<Detector>>> detectors_;
    std::vector<Histogram> accum_;   // per-feature accumulated counts this window
    long accum_samples_ = 0;
    bool reused_arena_ = false;
};

// --- Multi-model worker (G3: one process, many model_ids) --------------------
// Loads several bundles; an invalid bundle is GATED (skipped with a recorded
// error) without affecting the others (AC7). New models are added simply by
// providing more bundles — no code change (AC8).
class WorkerSet {
public:
    // Returns the number of models successfully brought up. Invalid/failed ones
    // are recorded in errors() as "path: reason".
    size_t load(const std::vector<std::string>& bundle_paths);
    size_t load_dir(const std::string& dir);   // all *.json in dir

    void tick_all(double elapsed_seconds, std::vector<ModelVerdict>& out);

    size_t size() const { return monitors_.size(); }
    ModelMonitor& at(size_t i) { return *monitors_[i]; }
    const std::vector<std::string>& errors() const { return errors_; }

private:
    std::vector<std::unique_ptr<ModelMonitor>> monitors_;
    std::vector<std::string> errors_;
};

}  // namespace driftmon

#endif  // DRIFTMON_WORKER_H
