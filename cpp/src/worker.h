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

// Anti-flapping severity debounce (pure, unit-testable). A raw level must be
// observed ABOVE the reported level for `up_windows` consecutive updates to
// raise it, and BELOW for `down_windows` consecutive updates to lower it.
// With the defaults (up=1, down=2) an alternating alarm/clean sequence reports
// steady alarm instead of flapping — the storm pattern operators learn to ignore.
class Debouncer {
public:
    Debouncer(int up_windows = 1, int down_windows = 2)
        : up_(up_windows), down_(down_windows) {}
    int update(int raw_level);
    int reported() const { return reported_; }

private:
    int up_, down_;
    int reported_ = 0;
    int up_streak_ = 0, down_streak_ = 0;
};

// --- Swap + read (the lock-free reader side, SPEC §6.2) ----------------------

// Freeze the active buffer (publish a swap), drain in-flight writers on the old
// buffer (bounded by grace_spins), read it into `out` (one Histogram per
// feature, physical bins), then zero it for reuse. Returns the frozen buffer's
// sample_count. `grace_spins`==0 means spin unboundedly until drained.
uint64_t slot_swap_read(SlotHeader* s, std::vector<Histogram>& out,
                        uint64_t grace_spins = 0);

// Snapshot the sample ring (v2): the most recent sampled raw INPUT vectors,
// oldest-first. Torn / in-progress rows are skipped. This is the retraining
// material a drift alarm gets dumped with (R-R1) — histograms alone cannot
// reconstruct training data.
std::vector<std::vector<float>> read_ring(SlotHeader* s);

// --- Per-model monitor -------------------------------------------------------

// Per-feature data-quality signals (v2). Quality ≠ drift: a NaN flood or a
// feature going constant means the upstream pipeline broke — retraining on
// such data would be poison, so it is alarmed on a separate channel.
struct FeatureQuality {
    double nan_ratio = 0.0;      // NaN share of all samples in the window
    double oor_ratio = 0.0;      // under+overflow share of non-NaN samples
    bool   constant = false;     // one bin holds ~everything while the ref was spread
    bool   out_of_range = false; // oor_ratio exceeded bundle.oor_ratio_max (drift-kind input)
    bool   alarm = false;        // nan_ratio over threshold OR constant
};

struct ModelVerdict {
    bool produced = false;       // false while accumulating or warming up
    bool warming_up = false;     // window closed by time but below min_samples
    long window_samples = 0;     // samples in the closed/active window
    double max_score = 0.0;
    // Debounced severity: 0 STABLE / 1 WARNING / 2 SIGNIFICANT. Levels come
    // from each feature's own detector thresholds (bundle-driven, P4) — never
    // from hardcoded score cutoffs — then pass the per-feature Debouncer.
    int severity = 0;
    bool alarm = false;          // == (severity == 2)
    bool quality_alarm = false;  // any feature's quality alarm (v2)
    // What KIND of change fired (R2) — routes the off-box response:
    //   "data_quality" → fix the pipeline; retraining on this data is poison
    //   "out_of_range" → new regime beyond the edges; retrain AND re-bin
    //   "abrupt"       → step change (ADWIN); investigate cause, full retrain
    //   "sustained"    → gradual trend (CUSUM); fine-tuning candidate
    //   "distribution" → shape shift within range (PSI/KS); retrain candidate
    //   "none"         → no alarm
    std::string kind = "none";
    std::vector<DriftResult> per_feature;
    std::vector<int> feature_severity;     // debounced, parallel to per_feature
    std::vector<FeatureQuality> quality;   // parallel to per_feature (v2)
    std::vector<Histogram> histograms;   // per-feature snapshot at evaluate time (for export)
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

    // Cumulative samples drained from the tap since this worker started —
    // never reset by window closes. rate(samples_total)==0 while serving is
    // live ⇒ the tap is dead/degraded (monitoring-of-the-monitoring).
    uint64_t samples_total() const { return samples_total_; }

    ~ModelMonitor();

private:
    void reset_window();
    ModelVerdict evaluate();

    // A detector bound to its test name + alarm threshold (threshold 0 for the
    // streaming detectors, whose alarm is internal — they report binary levels).
    struct BoundDetector {
        std::string test;
        double threshold = 0.0;
        std::unique_ptr<Detector> d;
    };

    Bundle bundle_;
    Arena arena_;
    std::vector<FeatureRef> refs_;
    // Per-feature list of detectors (PSI/KS/... selected by bundle.tests).
    std::vector<std::vector<BoundDetector>> detectors_;
    std::vector<Debouncer> debounce_;    // per-feature severity debounce
    std::vector<Histogram> accum_;   // per-feature accumulated counts this window
    long accum_samples_ = 0;
    uint64_t samples_total_ = 0;
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
