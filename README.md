# driftmon

추론엔진 비의존(inference-engine agnostic) **drift 모니터링 라이브러리**. 순수 C++로
"double 특징 벡터 in → drift 메트릭(PSI) out"만 책임진다. ONNX Runtime / LibTorch /
OpenVINO 등 어떤 추론엔진도 모르며, application 쪽 **glue 몇 줄**로만 닿는다.

> **시작점은 [SPEC.md](SPEC.md)** — 알고리즘·데이터 포맷·계약·빌드·컨벤션·로드맵의
> 정본(single source of truth). AI 툴 작업 규칙은 [CLAUDE.md](CLAUDE.md) /
> [AGENTS.md](AGENTS.md), 작업 티켓은 [TASKS.md](TASKS.md).

## 빌드 & 테스트
```sh
cmake -S . -B build && cmake --build build
ctest --test-dir build --output-on-failure
```

## 공개 API
C ABI 계약은 [`include/driftmon.h`](include/driftmon.h) (동결). 사용 예:
```cpp
auto out = session.Run(...);              // ← 여기만 추론엔진 (application glue)
double f[K]; extract_features(out, f);
driftmon_observe(mon, f, K);
if (driftmon_ready(mon)) {
    double psi[K], maxp;
    driftmon_compute(mon, psi, &maxp);
    export_metric(maxp);
    driftmon_reset(mon);
}
```

## Prometheus export (선택)
PSI를 Prometheus 텍스트 노출 포맷으로 내보내는 **선택 모듈**(`export/`). 코어는 여기에
의존하지 않으며, 기본 OFF다. 켜서 빌드:
```sh
cmake -S . -B build -DDRIFTMON_ENABLE_PROMETHEUS=ON && cmake --build build
```
사용:
```cpp
#include "driftmon_export.h"
double psi[K], maxp;
driftmon_compute(mon, psi, &maxp);
std::string body = dm::to_prometheus_text(feature_names,
                                          {psi, psi + K}, maxp);
// body를 기존 /metrics HTTP 핸들러로 그대로 서빙 (스크레이프 인프라 재사용).
```
출력 예:
```
driftmon_psi{feature="rsrp"} 0.0203
driftmon_psi_max 0.28
driftmon_drift_severity 2
```
prometheus-cpp 클라이언트(레지스트리/HTTP exposer/pushgateway)를 쓰고 싶으면, 같은
`psi[]`/`maxp` 값을 그쪽 `Gauge().Set(...)`에 넣으면 된다 — 코어/이 어댑터 변경 불필요.

## 상태
Phase 0~3 완료: 계약·문서·빌드 골격(0), PSI 코어(1), 텀블링 윈도우·분류(2),
Prometheus export 어댑터(3). 로드맵은 SPEC.md §8.
