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

## 상태
Phase 0 부트스트랩(계약·문서·빌드 골격·무의존 테스트 러너). 코어 알고리즘은 Phase 1+
에서 구현 — 자세한 로드맵은 SPEC.md §8.
