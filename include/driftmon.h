/*
 * driftmon.h — drift monitoring library, C ABI contract.
 *
 * FROZEN CONTRACT. Do not change this header without explicit agreement
 * recorded in SPEC.md (see "Decision Log"). Implementations on either side
 * (Claude Code / Codex) depend on this staying stable.
 *
 * This header references ZERO inference-engine types. The library only maps
 * "double feature vectors in -> drift metrics (PSI) out". It must never
 * include or depend on ONNX Runtime / LibTorch / OpenVINO / etc.
 *
 * See SPEC.md for the algorithm, data format, and semantics.
 */
#ifndef DRIFTMON_H
#define DRIFTMON_H

#ifdef __cplusplus
extern "C" {
#endif

/* Opaque handle. Layout is private to the implementation. */
typedef struct driftmon driftmon_t;

/* Drift severity per SPEC.md thresholds (§2.3). C ABI: values are stable ints. */
typedef enum {
    DRIFTMON_STABLE      = 0, /* PSI < 0.1  : no significant change */
    DRIFTMON_WARNING     = 1, /* 0.1 <= PSI < 0.2 : moderate change */
    DRIFTMON_SIGNIFICANT = 2  /* PSI >= 0.2 : significant drift     */
} driftmon_severity_t;

/*
 * Create a monitor from a reference profile (reference.json).
 * Runtime config (window_size, bucket layout, ref ratios) lives in the JSON,
 * keeping this header minimal and stable.
 * Returns NULL on failure (file missing, parse error, schema mismatch).
 */
driftmon_t* driftmon_create(const char* reference_json_path);

/*
 * Number of features the monitor expects. Callers use this to size the
 * `psi_out` buffer for driftmon_compute and to validate their feature count.
 */
int driftmon_num_features(const driftmon_t* m);

/*
 * Accumulate one observation. `feats` points to `n` doubles; `n` must equal
 * driftmon_num_features(m). Out-of-range/NaN handling is defined in SPEC.md.
 */
void driftmon_observe(driftmon_t* m, const double* feats, int n);

/* Nonzero once the current window has accumulated enough observations. */
int driftmon_ready(const driftmon_t* m);

/*
 * Compute PSI for the current window.
 * `psi_out` must point to at least driftmon_num_features(m) doubles and
 * receives the per-feature PSI. `max_psi` receives the maximum across
 * features (the headline drift signal). Both out-params may be NULL to skip.
 */
void driftmon_compute(driftmon_t* m, double* psi_out, double* max_psi);

/*
 * Classify a PSI value into a drift severity per SPEC.md thresholds.
 * Stateless pure function (no handle needed); keeps the 0.1/0.2 thresholds in
 * one place so callers don't hard-code them.
 */
driftmon_severity_t driftmon_classify(double psi);

/* Clear the current window's accumulated observations (reference untouched). */
void driftmon_reset(driftmon_t* m);

/* Free all resources. Safe to call with NULL. */
void driftmon_destroy(driftmon_t* m);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* DRIFTMON_H */
