// worker.cpp — driftmon-cpp drift worker (SPEC §5.3, §6.2).
#include "worker.h"

#include <atomic>
#include <set>
#include <thread>

#include <dirent.h>

#include "adwin_detector.h"
#include "cusum_detector.h"
#include "ks_detector.h"
#include "psi_detector.h"

namespace driftmon {

SlotSpec slot_spec_from_bundle(const Bundle& b) {
    SlotSpec spec;
    spec.model_id = b.model_id;
    spec.n_features = static_cast<uint32_t>(b.features.size());
    spec.bin_offset.assign(spec.n_features + 1, 0);
    uint32_t acc = 0;
    for (uint32_t f = 0; f < spec.n_features; ++f) {
        spec.bin_offset[f] = acc;
        acc += static_cast<uint32_t>(b.features[f].interior_bins()) + DRIFTMON_EXTRA_BINS;
    }
    spec.bin_offset[spec.n_features] = acc;
    spec.n_bins_total = acc;
    // v2 sample ring: input vectors only.
    uint32_t n_inputs = 0;
    for (const auto& f : b.features) if (!f.is_output) ++n_inputs;
    spec.n_inputs = n_inputs;
    spec.ring_rows = n_inputs > 0 ? static_cast<uint32_t>(b.ring_rows) : 0;
    spec.sample_every = static_cast<uint32_t>(b.sample_every);
    return spec;
}

std::vector<std::vector<float>> read_ring(SlotHeader* s) {
    std::vector<std::vector<float>> rows;
    if (s->ring_rows == 0 || s->n_inputs == 0) return rows;
    const uint64_t head = s->ring_head.load(std::memory_order_acquire);
    const uint64_t avail = head < s->ring_rows ? head : s->ring_rows;
    rows.reserve(avail);
    // Oldest-first over the last `avail` claims; skip torn/in-progress rows.
    for (uint64_t k = head - avail; k < head; ++k) {
        const uint32_t row = static_cast<uint32_t>(k % s->ring_rows);
        std::atomic<uint64_t>* rowseq = ring_rowseq(s, row);
        const uint64_t s1 = rowseq->load(std::memory_order_acquire);
        if (s1 & 1) continue;                        // writer mid-copy
        std::vector<float> vals(s->n_inputs);
        auto* src = reinterpret_cast<std::atomic<uint32_t>*>(ring_rowdata(s, row));
        for (uint32_t j = 0; j < s->n_inputs; ++j) {
            const uint32_t bits = src[j].load(std::memory_order_relaxed);
            float f;
            __builtin_memcpy(&f, &bits, sizeof(f));
            vals[j] = f;
        }
        std::atomic_thread_fence(std::memory_order_acquire);
        if (rowseq->load(std::memory_order_relaxed) != s1) continue;  // overwritten mid-read
        rows.push_back(std::move(vals));
    }
    return rows;
}

std::vector<FeatureRef> feature_refs_from_bundle(const Bundle& b) {
    std::vector<FeatureRef> refs;
    refs.reserve(b.features.size());
    for (const auto& f : b.features) {
        FeatureRef r;
        r.name = f.name;
        r.threshold = f.psi_threshold;
        // Detectors see DISTRIBUTION bins only: [underflow, interior..., overflow].
        // The NaN bin (physical index B+2) is a quality signal, deliberately
        // excluded — a NaN flood must raise the quality alarm, not fake drift.
        long total = 0;
        for (long c : f.ref_hist) total += c;
        r.ref_ratios.assign(f.interior_bins() + 2, 0.0);
        if (total > 0) {
            const double denom = static_cast<double>(total);
            for (size_t k = 0; k < f.ref_hist.size(); ++k)
                r.ref_ratios[k + 1] = static_cast<double>(f.ref_hist[k]) / denom;
        }
        refs.push_back(std::move(r));
    }
    return refs;
}

WindowDecision window_decision(long accumulated_samples, double elapsed_seconds,
                               long min_samples, long max_seconds) {
    if (accumulated_samples >= min_samples) return WindowDecision::Evaluate;
    if (elapsed_seconds >= static_cast<double>(max_seconds))
        return WindowDecision::WarmupClose;
    return WindowDecision::Accumulate;
}

int Debouncer::update(int raw_level) {
    if (raw_level > reported_) {
        down_streak_ = 0;
        if (++up_streak_ >= up_) { reported_ = raw_level; up_streak_ = 0; }
    } else if (raw_level < reported_) {
        up_streak_ = 0;
        if (++down_streak_ >= down_) { reported_ = raw_level; down_streak_ = 0; }
    } else {
        up_streak_ = 0;
        down_streak_ = 0;
    }
    return reported_;
}

uint64_t slot_swap_read(SlotHeader* s, std::vector<Histogram>& out,
                        uint64_t grace_spins) {
    const uint32_t old = s->active_idx.load(std::memory_order_seq_cst);
    const uint32_t nw = 1u - old;

    s->seq.fetch_add(1, std::memory_order_release);          // odd: swap in progress
    s->active_idx.store(nw, std::memory_order_seq_cst);      // publish new active

    // Drain: wait for writers that already entered on `old` to finish. Bounded
    // by grace_spins (0 = unbounded) so a stalled serving thread can't hang us.
    uint64_t spins = 0;
    while (s->inflight[old].load(std::memory_order_acquire) != 0) {
        if (grace_spins != 0 && ++spins > grace_spins) break;  // residual drop, tolerated
        std::this_thread::yield();
    }

    Counter* buf = hist_buffer(s, old);
    const uint32_t nf = s->n_features;
    out.resize(nf);
    for (uint32_t f = 0; f < nf; ++f) {
        const uint32_t base = s->bin_offset[f];
        const uint32_t nb = s->bin_offset[f + 1] - base;
        out[f].counts.resize(nb);
        uint64_t total = 0;
        for (uint32_t k = 0; k < nb; ++k) {
            const uint64_t c = buf[base + k].load(std::memory_order_relaxed);
            out[f].counts[k] = c;
            total += c;
        }
        out[f].total = total;
    }
    const uint64_t sample_count = s->sample_count[old].load(std::memory_order_relaxed);

    // Zero the frozen buffer so it is clean for the NEXT swap (bounds counts to
    // one window → uint64 overflow impossible).
    for (uint32_t i = 0; i < s->n_bins_total; ++i)
        buf[i].store(0, std::memory_order_relaxed);
    s->sample_count[old].store(0, std::memory_order_relaxed);

    s->seq.fetch_add(1, std::memory_order_release);          // even: stable
    return sample_count;
}

bool ModelMonitor::init(const Bundle& b, std::string& err) {
    bundle_ = b;
    SlotSpec spec = slot_spec_from_bundle(b);
    if (!arena_open_or_create(arena_, arena_name(b.model_id), spec, reused_arena_, err))
        return false;

    refs_ = feature_refs_from_bundle(b);   // ref_ratios over physical bins
    detectors_.clear();
    detectors_.resize(refs_.size());       // default-construct (vectors not copyable)
    debounce_.assign(refs_.size(), Debouncer(b.up_windows, b.down_windows));
    for (size_t f = 0; f < refs_.size(); ++f) {
        for (const std::string& test : b.tests) {
            FeatureRef r = refs_[f];
            BoundDetector bd;
            bd.test = test;
            if (test == "psi") {
                r.threshold = bd.threshold = b.features[f].psi_threshold;
                bd.d = std::make_unique<PsiDetector>();
            } else if (test == "ks") {
                r.threshold = bd.threshold = b.features[f].ks_threshold;
                bd.d = std::make_unique<KsDetector>();
            } else if (test == "cusum") {
                bd.d = std::make_unique<CusumDetector>();   // streaming (R3.4)
            } else if (test == "adwin") {
                bd.d = std::make_unique<AdwinDetector>();    // streaming (R3.4)
            } else {
                continue;   // unknown detector name ignored (forward-compat)
            }
            bd.d->configure(r);
            detectors_[f].push_back(std::move(bd));
        }
    }
    reset_window();
    return true;
}

void ModelMonitor::reset_window() {
    accum_samples_ = 0;
    accum_.assign(refs_.size(), Histogram{});
    // Accumulate over the FULL physical layout (incl. the NaN bin).
    for (size_t f = 0; f < refs_.size(); ++f)
        accum_[f].counts.assign(
            bundle_.features[f].interior_bins() + DRIFTMON_EXTRA_BINS, 0);
}

ModelVerdict ModelMonitor::evaluate() {
    ModelVerdict v;
    v.produced = true;
    v.window_samples = accum_samples_;
    v.per_feature.resize(detectors_.size());
    v.feature_severity.resize(detectors_.size());
    v.quality.resize(detectors_.size());
    std::set<std::string> alarmed_tests;   // which detector kinds fired (R2)
    bool any_out_of_range_alarm = false;
    for (size_t f = 0; f < detectors_.size(); ++f) {
        const Histogram& full = accum_[f];              // physical: [... overflow, nan]
        const uint64_t nan_count = full.counts.empty() ? 0 : full.counts.back();

        // Distribution view for detectors: NaN bin stripped, total adjusted.
        Histogram dist;
        dist.counts.assign(full.counts.begin(),
                           full.counts.empty() ? full.counts.end()
                                               : full.counts.end() - 1);
        dist.total = full.total - nan_count;

        DriftResult feat;   // worst detector for this feature
        int raw_level = 0;  // 0/1/2 from each detector's OWN threshold (P4)
        for (auto& det : detectors_[f]) {
            DriftResult r = det.d->eval(dist);
            if (r.score > feat.score) feat.score = r.score;
            if (r.alarm) {
                feat.alarm = true;
                alarmed_tests.insert(det.test);   // feeds drift-kind (R2)
            }
            int lvl = 0;
            if (r.alarm) lvl = 2;
            else if (det.threshold > 0.0 &&
                     r.score >= bundle_.warn_ratio * det.threshold) lvl = 1;
            if (lvl > raw_level) raw_level = lvl;
        }
        v.per_feature[f] = feat;
        v.feature_severity[f] = debounce_[f].update(raw_level);
        if (v.feature_severity[f] > v.severity) v.severity = v.feature_severity[f];
        if (feat.score > v.max_score) v.max_score = feat.score;

        // Data-quality signals (v2): pipeline breakage, not drift.
        FeatureQuality& q = v.quality[f];
        if (full.total > 0)
            q.nan_ratio = static_cast<double>(nan_count) /
                          static_cast<double>(full.total);
        if (dist.total > 0) {
            const uint64_t oor = dist.counts.front() +
                                 (dist.counts.size() > 1 ? dist.counts.back() : 0);
            q.oor_ratio = static_cast<double>(oor) / static_cast<double>(dist.total);
            // Constant feature: everything lands in one bin while the reference
            // was spread — upstream likely feeding a stuck/default value.
            uint64_t max_bin = 0;
            for (uint64_t c : dist.counts) if (c > max_bin) max_bin = c;
            double ref_max = 0.0;
            for (double r : refs_[f].ref_ratios) if (r > ref_max) ref_max = r;
            q.constant =
                static_cast<double>(max_bin) / static_cast<double>(dist.total) >= 0.99 &&
                ref_max <= 0.9;
        }
        q.out_of_range = q.oor_ratio > bundle_.oor_ratio_max;
        q.alarm = q.nan_ratio > bundle_.nan_ratio_max || q.constant;
        if (q.alarm) v.quality_alarm = true;
        if (q.out_of_range && feat.alarm) any_out_of_range_alarm = true;
    }
    v.alarm = (v.severity == 2);   // model alarm is the DEBOUNCED significant level

    // Classify the change kind (R2) — precedence: broken pipeline beats drift
    // explanations; regime escape beats shape analysis; the streaming shape
    // (abrupt vs sustained) beats the generic distribution verdict.
    if (v.quality_alarm)                          v.kind = "data_quality";
    else if (!v.alarm)                            v.kind = "none";
    else if (any_out_of_range_alarm)              v.kind = "out_of_range";
    else if (alarmed_tests.count("adwin"))        v.kind = "abrupt";
    else if (alarmed_tests.count("cusum"))        v.kind = "sustained";
    else                                          v.kind = "distribution";

    v.histograms = accum_;   // snapshot for export (R5.1)
    return v;
}

ModelVerdict ModelMonitor::tick(double elapsed_seconds) {
    std::vector<Histogram> frozen;
    const uint64_t n = slot_swap_read(slot(), frozen);

    // Accumulate this snapshot into the current window.
    for (size_t f = 0; f < accum_.size() && f < frozen.size(); ++f) {
        const size_t nb = accum_[f].counts.size();
        for (size_t k = 0; k < nb && k < frozen[f].counts.size(); ++k)
            accum_[f].counts[k] += frozen[f].counts[k];
        uint64_t total = 0;
        for (uint64_t c : accum_[f].counts) total += c;
        accum_[f].total = total;
    }
    accum_samples_ += static_cast<long>(n);
    samples_total_ += n;

    switch (window_decision(accum_samples_, elapsed_seconds,
                            bundle_.min_samples, bundle_.max_seconds)) {
        case WindowDecision::Accumulate: {
            ModelVerdict v;            // not produced yet
            v.window_samples = accum_samples_;
            return v;
        }
        case WindowDecision::Evaluate: {
            ModelVerdict v = evaluate();
            reset_window();
            return v;
        }
        case WindowDecision::WarmupClose: {
            ModelVerdict v;
            v.warming_up = true;       // window closed by time, verdict withheld
            v.window_samples = accum_samples_;
            reset_window();
            return v;
        }
    }
    return ModelVerdict{};
}

ModelMonitor::~ModelMonitor() {
    if (arena_.is_owner) {
        const std::string nm = arena_.name;
        arena_detach(arena_);
        arena_unlink(nm);
    } else {
        arena_detach(arena_);
    }
}

// --- WorkerSet ---------------------------------------------------------------

size_t WorkerSet::load(const std::vector<std::string>& bundle_paths) {
    for (const std::string& path : bundle_paths) {
        Bundle b;
        std::string err;
        if (!load_bundle(path, b, err)) {            // R4.2 gate: skip, record, continue
            errors_.push_back(path + ": " + err);
            continue;
        }
        auto mon = std::make_unique<ModelMonitor>();
        if (!mon->init(b, err)) {
            errors_.push_back(path + ": " + err);
            continue;
        }
        monitors_.push_back(std::move(mon));
    }
    return monitors_.size();
}

size_t WorkerSet::load_dir(const std::string& dir) {
    std::vector<std::string> paths;
    if (DIR* d = opendir(dir.c_str())) {
        while (struct dirent* e = readdir(d)) {
            const std::string name = e->d_name;
            if (name.size() > 5 && name.compare(name.size() - 5, 5, ".json") == 0)
                paths.push_back(dir + "/" + name);
        }
        closedir(d);
    } else {
        errors_.push_back(dir + ": cannot open bundle directory");
    }
    return load(paths);
}

void WorkerSet::tick_all(double elapsed_seconds, std::vector<ModelVerdict>& out) {
    out.clear();
    out.reserve(monitors_.size());
    for (auto& m : monitors_)
        out.push_back(m->tick(elapsed_seconds));   // independent: no cross-model coupling
}

}  // namespace driftmon
