# driftmon

추론엔진 비의존 **drift 모니터링 라이브러리** / Inference-engine agnostic **drift monitoring library**.

"double 특징 벡터 in → PSI(drift 메트릭) out"만 책임진다. ONNX Runtime / LibTorch /
OpenVINO 등 **어떤 추론엔진도 모르며**, application 쪽 **glue 몇 줄**로만 닿는다.

> **정본(single source of truth): [SPEC.md](SPEC.md)** — 알고리즘·데이터 포맷·계약·
> 빌드·컨벤션·결정 로그·로드맵. AI 툴 작업 규칙은 [CLAUDE.md](CLAUDE.md) /
> [AGENTS.md](AGENTS.md), 작업 티켓은 [TASKS.md](TASKS.md).

---

## driftmon이란? / What is driftmon?

**Data drift** occurs when the statistical distribution of model inputs shifts from the
training distribution, degrading model performance silently.
`driftmon` detects this shift using **PSI (Population Stability Index)**:

```
PSI = Σ (actual_ratio − expected_ratio) × ln(actual_ratio / expected_ratio)
```

- PSI < 0.1  → **STABLE** (no significant drift)
- 0.1 ≤ PSI < 0.2 → **WARNING** (moderate drift, monitor closely)
- PSI ≥ 0.2  → **SIGNIFICANT** (action recommended)

---

## 퀵스타트 / Quick Start

### Step 1 — Generate a reference profile from training data

```sh
python3 tools/make_reference.py \
    --input zone_a_train.csv \
    --buckets 10 \
    --window-size 500 \
    --output reference.json
```

### Step 2 — Build the library

```sh
cmake -S . -B build && cmake --build build
ctest --test-dir build --output-on-failure   # 22 core tests pass
```

### Step 3 — Link and integrate (5-line glue pattern)

```cpp
// One-time setup
driftmon_t* mon = driftmon_create("reference.json");
int K = driftmon_num_features(mon);

// Per-inference loop
double feats[K];
extract_features(session.Run(...), feats);   // ← only line that touches your engine
driftmon_observe(mon, feats, K);

if (driftmon_ready(mon)) {
    double psi[K], max_psi;
    driftmon_compute(mon, psi, &max_psi);
    log_metric("drift_severity", driftmon_classify(max_psi));
    driftmon_reset(mon);
}

// Teardown
driftmon_destroy(mon);
```

---

## 빌드 옵션 / Build Options

All optional modules are **OFF by default** — the core library always builds clean with
zero external dependencies.

| CMake option | Default | What it adds |
|---|---|---|
| `DRIFTMON_ENABLE_PROMETHEUS` | OFF | `driftmon_export` library + 5 export tests |
| `DRIFTMON_ENABLE_TOOLS` | OFF | `check_reference` binary + 13 Python unit tests |
| `DRIFTMON_ENABLE_EXAMPLES` | OFF | `demo_driver` binary + `deepmimo_e2e` ctest |
| (all ON) | — | 6 ctest suites, all pass |

Enable multiple options at once:

```sh
cmake -S . -B build \
    -DDRIFTMON_ENABLE_PROMETHEUS=ON \
    -DDRIFTMON_ENABLE_TOOLS=ON \
    -DDRIFTMON_ENABLE_EXAMPLES=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

---

## DeepMIMO 데모 실행 / Running the DeepMIMO Demo

Reproduces Zone A (LOS, reference) → Zone E (NLOS, drifted) drift detection using
synthetic channel statistics (RSRP · main path delay · strongest angle).

```sh
cmake -S . -B build -DDRIFTMON_ENABLE_EXAMPLES=ON
cmake --build build
python3 examples/deepmimo_demo/run_demo.py --build-dir build
```

Expected output:

```
=== Step 3: Zone A test (same distribution — expect STABLE) ===
WINDOW 1: max_psi=0.0XXX  severity=STABLE       psi=[...]

=== Step 4: Zone E test (NLOS drift — expect SIGNIFICANT) ===
WINDOW 1: max_psi=0.XXXX  severity=SIGNIFICANT  psi=[...]

PASS  Zone A: max_psi=... < 0.1  (STABLE ✓)
PASS  Zone E: max_psi=... > 0.2  (SIGNIFICANT ✓)
```

---

## 레퍼런스 프로파일 생성 / Generating a Reference Profile

`tools/make_reference.py` — stdlib-only Python (no numpy/pandas required).

```sh
# From a CSV file (auto-discovers feature columns from header):
python3 tools/make_reference.py --input training_data.csv --output reference.json

# Specify features and bucket count explicitly:
python3 tools/make_reference.py \
    --input training_data.csv \
    --features rsrp delay angle \
    --buckets 10 \
    --window-size 1000 \
    --output reference.json

# Self-test with synthetic data (no CSV needed):
python3 tools/make_reference.py --self-test
```

The tool uses **equal-frequency (quantile) binning** — each bucket captures approximately
the same number of training samples, giving stable PSI values across bucket sizes.

---

## 공개 API / Public API

C ABI contract: [`include/driftmon.h`](include/driftmon.h) (frozen — see SPEC.md §4).

```c
// Lifecycle
driftmon_t* driftmon_create(const char* reference_json_path);  // NULL on failure
void        driftmon_destroy(driftmon_t* m);

// Observation
int  driftmon_num_features(const driftmon_t* m);               // allocate psi_out safely
void driftmon_observe(driftmon_t* m, const double* feats, int n);

// Window
int  driftmon_ready(const driftmon_t* m);   // nonzero when window is full

// Compute (call after ready, then reset)
void driftmon_compute(driftmon_t* m, double* psi_out, double* max_psi);
void driftmon_reset(driftmon_t* m);

// Classification (stateless, reusable anywhere)
typedef enum {
    DRIFTMON_STABLE      = 0,  // max_psi < 0.1
    DRIFTMON_WARNING     = 1,  // 0.1 <= max_psi < 0.2
    DRIFTMON_SIGNIFICANT = 2,  // max_psi >= 0.2
} driftmon_severity_t;
driftmon_severity_t driftmon_classify(double psi);
```

NaN/Inf observations are silently ignored. Features with zero valid observations
report PSI = 0 (no spurious drift). See SPEC.md §2.4 for numerical stability rules.

---

## Prometheus 내보내기 / Prometheus Export

Optional adapter (`export/`) that renders PSI as Prometheus text exposition format.
Zero external dependencies — output can be served directly from any existing HTTP handler.

```sh
cmake -S . -B build -DDRIFTMON_ENABLE_PROMETHEUS=ON && cmake --build build
```

```cpp
#include "driftmon_export.h"

double psi[K], max_psi;
driftmon_compute(mon, psi, &max_psi);
std::string body = dm::to_prometheus_text(feature_names, {psi, psi + K}, max_psi);
// Serve `body` from your /metrics endpoint — scrape infrastructure unchanged.
```

Sample output:

```
driftmon_psi{feature="rsrp"} 0.0203
driftmon_psi{feature="delay"} 0.0071
driftmon_psi_max 0.0203
driftmon_drift_severity 0
```

To use prometheus-cpp (registry / HTTP exposer / pushgateway), pass the same
`psi[]`/`max_psi` values to `Gauge().Set(...)` — no changes to core or this adapter.

---

## ONNX Runtime 연동 / ONNX Runtime Integration

`examples/onnx_integration/onnx_glue.cpp` shows the integration pattern.
The library never depends on ONNX Runtime; only the application glue does.

`onnx_glue.cpp` defines `run_with_ort()` but no `main` — it is meant to be
**compiled into your application**, not linked as a standalone binary:

```sh
# Compile the glue as an object file to link into your own app:
g++ -std=c++17 -c -DONNXRUNTIME_AVAILABLE \
    -I/opt/onnxruntime/include -I./include \
    examples/onnx_integration/onnx_glue.cpp \
    -o onnx_glue.o

# Link with your app (which provides main):
g++ -std=c++17 your_app.cpp onnx_glue.o \
    -L/opt/onnxruntime/lib -lonnxruntime \
    -L./build -ldriftmon -o my_monitor
```

The key pattern (engine-agnostic side):

```cpp
driftmon_t* dm = driftmon_create("reference.json");
// ... run inference ...
for (int i = 0; i < nf; ++i) feats[i] = static_cast<double>(ort_output[i]);
driftmon_observe(dm, feats, nf);              // only these 5 lines touch driftmon
if (driftmon_ready(dm)) {
    driftmon_compute(dm, psi, &max_psi);
    handle_drift(driftmon_classify(max_psi));
    driftmon_reset(dm);
}
```

---

## 상태 / Status

| Phase | Description | Status |
|---|---|---|
| 0 | Contract · SPEC · build skeleton · test runner | ✓ Complete |
| 1 | PSI core · histogram · JSON loader | ✓ Complete |
| 2 | Tumbling window · severity classification | ✓ Complete |
| 3 | Prometheus export adapter (optional) | ✓ Complete |
| 4 | `make_reference.py` reference generation tool | ✓ Complete |
| 5 | DeepMIMO demo · ONNX RT glue pattern | ✓ Complete |
| 6 | Sliding window · notification callback · multi-reference | Planned |
