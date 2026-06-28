// worker.cpp — driftmon-cpp drift worker (SPEC §5.3, §6.2).
#include "worker.h"

#include <atomic>
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
    return spec;
}

std::vector<FeatureRef> feature_refs_from_bundle(const Bundle& b) {
    std::vector<FeatureRef> refs;
    refs.reserve(b.features.size());
    for (const auto& f : b.features) {
        FeatureRef r;
        r.name = f.name;
        r.threshold = f.psi_threshold;
        // Physical layout: [underflow, interior..., overflow]. Normalize ref_hist
        // counts to ratios over the interior; underflow/overflow stay 0.
        long total = 0;
        for (long c : f.ref_hist) total += c;
        r.ref_ratios.assign(f.interior_bins() + DRIFTMON_EXTRA_BINS, 0.0);
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
    for (size_t f = 0; f < refs_.size(); ++f) {
        for (const std::string& test : b.tests) {
            FeatureRef r = refs_[f];
            std::unique_ptr<Detector> d;
            if (test == "psi") {
                r.threshold = b.features[f].psi_threshold;
                d = std::make_unique<PsiDetector>();
            } else if (test == "ks") {
                r.threshold = b.features[f].ks_threshold;
                d = std::make_unique<KsDetector>();
            } else if (test == "cusum") {
                d = std::make_unique<CusumDetector>();   // streaming (R3.4)
            } else if (test == "adwin") {
                d = std::make_unique<AdwinDetector>();    // streaming (R3.4)
            } else {
                continue;   // unknown detector name ignored (forward-compat)
            }
            d->configure(r);
            detectors_[f].push_back(std::move(d));
        }
    }
    reset_window();
    return true;
}

void ModelMonitor::reset_window() {
    accum_samples_ = 0;
    accum_.assign(refs_.size(), Histogram{});
    for (size_t f = 0; f < refs_.size(); ++f)
        accum_[f].counts.assign(refs_[f].ref_ratios.size(), 0);
}

ModelVerdict ModelMonitor::evaluate() {
    ModelVerdict v;
    v.produced = true;
    v.window_samples = accum_samples_;
    v.per_feature.resize(detectors_.size());
    for (size_t f = 0; f < detectors_.size(); ++f) {
        DriftResult feat;   // worst detector for this feature
        for (auto& det : detectors_[f]) {
            DriftResult r = det->eval(accum_[f]);
            if (r.score > feat.score) feat.score = r.score;
            if (r.alarm) feat.alarm = true;
        }
        v.per_feature[f] = feat;
        if (feat.score > v.max_score) v.max_score = feat.score;
        if (feat.alarm) v.alarm = true;
    }
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
