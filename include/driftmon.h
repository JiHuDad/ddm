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

/* Window accumulation model (Phase 6, §2.5). */
typedef enum {
    DRIFTMON_TUMBLING = 0, /* default: accumulate until window_size, then reset  */
    DRIFTMON_SLIDING  = 1  /* rolling: always reflect last window_size observations */
} driftmon_window_mode_t;

/*
 * Create a monitor from a reference profile (reference.json).
 * Equivalent to driftmon_create_ex(path, DRIFTMON_TUMBLING).
 * Returns NULL on failure (file missing, parse error, schema mismatch).
 */
driftmon_t* driftmon_create(const char* reference_json_path);

/*
 * Create a monitor with an explicit window mode.
 * Runtime config (window_size, bucket layout, ref ratios) lives in the JSON,
 * keeping this header minimal and stable.
 * Returns NULL on failure.
 */
driftmon_t* driftmon_create_ex(const char* reference_json_path,
                                driftmon_window_mode_t mode);

/*
 * Create a monitor that computes PSI against multiple reference profiles.
 * All references must share the same feature names, bucket counts per feature,
 * and window_size; returns NULL on any mismatch or load failure.
 * psi_out[j] from driftmon_compute = max PSI for feature j across all n refs.
 * n == 1 gives identical behaviour to driftmon_create(paths[0]).
 * Window mode defaults to DRIFTMON_TUMBLING.
 */
driftmon_t* driftmon_create_multi(const char** paths, int n);

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

/* Nonzero once the current window has accumulated enough observations.
 * Tumbling: resets to 0 after driftmon_reset().
 * Sliding:  stays nonzero once first full window has been seen. */
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

/*
 * Notification callback — fired by driftmon_compute immediately after psi_out
 * and max_psi are finalised.  Called for every severity level (STABLE included);
 * filtering is the caller's responsibility.  fn == NULL unregisters.
 * Safe to call with m == NULL (no-op).
 */
typedef void (*driftmon_callback_t)(driftmon_t*         m,
                                    double              max_psi,
                                    driftmon_severity_t severity,
                                    void*               user_data);
void driftmon_set_callback(driftmon_t*         m,
                           driftmon_callback_t fn,
                           void*               user_data);

/* Tumbling mode: clear the current window's observations (reference untouched).
 * Sliding mode: no-op — the rolling buffer continues uninterrupted. */
void driftmon_reset(driftmon_t* m);

/* Free all resources. Safe to call with NULL. */
void driftmon_destroy(driftmon_t* m);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* DRIFTMON_H */
