# SPEC.md — driftmon (정본 / single source of truth)

> 이 문서가 **유일한 진실의 원천**입니다. 알고리즘·데이터 포맷·계약·빌드 명령·
> 컨벤션은 모두 여기서 결정합니다. 코드와 이 문서가 충돌하면 이 문서가 우선이며,
> 충돌을 발견하면 먼저 이 문서를 고치고 결정 로그에 남깁니다.

---

## 1. 목적 / 비목적 (Purpose / Non-goals)

**목적.** `driftmon`은 입력/특징 분포의 **드리프트(drift)** 를 측정하는 순수 C++
라이브러리다. 핵심은 **PSI(Population Stability Index)** 메트릭을 계산하는 것:
"double 특징 벡터 in → drift 메트릭 out".

**비목적 (명시적으로 하지 않는 것).**
- 추론 수행. 라이브러리는 ONNX Runtime / LibTorch / OpenVINO 등 **어떤 추론엔진도
  모른다.** 헤더·소스에 그 타입을 절대 import하지 않는다.
- 모델 정확도/성능 측정 (그건 별개 관심사).
- 메트릭 export 전송 자체 (Prometheus 노출은 Phase 3의 **분리된 선택 모듈**).

추론엔진과 만나는 유일한 지점은 **application 쪽 glue 몇 줄**이다:
```cpp
auto out = session.Run(...);              // ← 여기만 추론엔진
double f[K]; extract_features(out, f);    // 모니터링 특징 추출
driftmon_observe(mon, f, K);
if (driftmon_ready(mon)) {
    double psi[K], maxp;
    driftmon_compute(mon, psi, &maxp);
    export_metric(maxp);                  // 기존 관측 인프라 재사용
    driftmon_reset(mon);
}
```

---

## 2. 알고리즘 (Algorithm)

### 2.1 PSI 정의
특징별로, 참조(reference) 분포와 현재 윈도우(actual) 분포를 같은 버킷 경계로 나눈 뒤:

```
PSI = Σ_i ( a_i − e_i ) · ln( a_i / e_i )
```
- `e_i` = 참조 분포에서 버킷 `i`의 비율 (reference ratio)
- `a_i` = 현재 윈도우에서 버킷 `i`의 비율 (actual ratio)
- 합은 모든 버킷에 대해

### 2.2 버킷 (Buckets)
- 기본 버킷 수 **10개**. 버킷 경계(`edges`)는 참조 프로파일에 저장된다
  (오프라인에서 학습 데이터로부터 산출 — Phase 4 도구).
- `edges`의 크기는 `num_buckets + 1`. 값 `x`는 `edges[k] <= x < edges[k+1]` 이면
  버킷 `k`. 경계/범위 밖 처리는 2.4 참조.

### 2.3 임계값 (Thresholds)
헤드라인 신호는 특징별 PSI의 **최댓값(max PSI)**:
- `PSI < 0.1` → **안정** (유의한 변화 없음) — `DRIFTMON_STABLE`
- `0.1 ≤ PSI < 0.2` → **주의** (보통 수준의 변화) — `DRIFTMON_WARNING`
- `PSI ≥ 0.2` → **유의** (상당한 드리프트) — `DRIFTMON_SIGNIFICANT`

이 분류는 `driftmon_classify(double psi)`로 노출한다(§4). 임계값 상수의 단일
출처를 라이브러리에 두어 호출자가 0.1/0.2를 하드코딩하지 않게 한다.

### 2.4 수치 안정성 규칙 (Numerical stability — 반드시 준수)
PSI는 `ln(a/e)`와 나눗셈을 포함하므로 0을 다뤄야 한다.
- **Epsilon floor.** 비율이 0이거나 `EPSILON` 미만이면 `EPSILON`으로 바닥을 둔다.
  기본 `EPSILON = 1e-4`. `e_i`, `a_i` 모두에 적용 후 PSI 합산.
- **빈 버킷.** 위 floor로 자연히 처리된다 (0 → EPSILON).
- **범위 밖 값.** `edges` 최솟값 미만은 첫 버킷에, 최댓값 이상은 마지막 버킷에 클램프.
- **NaN/Inf 입력.** 해당 관측의 그 특징은 무시(카운트하지 않음)하되, 다른 특징은 정상
  처리. 관측 자체는 `driftmon_ready` 카운트에 포함된다.
- **유효 관측 0인 특징.** 어떤 특징의 윈도우 내 유효 카운트 합이 0이면(예: 전부
  NaN/Inf) 드리프트는 **정의되지 않으므로 PSI=0**으로 보고한다. (epsilon floor가
  허위 고PSI를 만들지 않도록.) `a_i` 분모는 **특징별** 유효 카운트 합이다.

### 2.5 윈도우 모델 (Windowing)
- 현재 코어는 **텀블링 윈도우**다. `driftmon_observe`가 관측을 누적하고,
  `driftmon_ready`는 누적 관측 수 ≥ `window_size`일 때 nonzero.
- 윈도우를 닫는 책임은 호출자에 있다: `ready`면 `compute` 후 `reset`을 호출해
  다음 윈도우를 새로 시작한다.
- **슬라이딩 윈도우**는 Phase 6에서 `driftmon_create_ex(path, DRIFTMON_SLIDING)`으로
  추가 예정 (§7 결정 로그, §8 로드맵). 슬라이딩 모드에서는 `driftmon_ready`가 최초
  `window_size`개 누적 후 항상 nonzero를 반환하며, `driftmon_reset`은 no-op이다.

---

## 3. 데이터 포맷 (Data Format) — `reference.json`

코어는 **이 한정 스키마만** 파싱한다 (`src/json_min.*`의 최소 자체 파서). 범용 JSON
지원은 불필요.

```json
{
  "version": 1,
  "window_size": 1000,
  "features": [
    {
      "name": "rsrp",
      "edges":      [-120, -110, -100, -90, -80, -70, -60, -50, -40, -30, -20],
      "ref_ratios": [0.05, 0.08, 0.10, 0.12, 0.15, 0.15, 0.12, 0.10, 0.08, 0.05]
    }
  ]
}
```
**불변식 (검증 필수):**
- `version` 은 정수. 현재 지원 버전 = 1.
- `window_size` 는 양의 정수 (윈도우가 찰 관측 수 = `driftmon_ready` 기준).
- 각 feature에 대해 `edges.size() == ref_ratios.size() + 1`.
- `ref_ratios` 합은 약 1.0 (허용 오차 내). 위반 시 `load_reference` 실패(false).
- `features` 가 비면 실패.

---

## 4. 계약 (Contract) — `include/driftmon.h`

> **이 헤더는 동결(FROZEN)되어 있다. 합의 없이 수정 금지.** 변경이 필요하면 먼저 이
> SPEC의 결정 로그(§7)에 제안·합의를 기록한 뒤에만 헤더를 고친다. 내부 구현은
> 자유롭게 바꿔도 되지만 이 시그니처/의미는 양쪽 AI 툴이 의존하는 닻이다.
> 기존 시그니처 변경/삭제는 금지. **하위호환 추가**(새 함수)는 결정 로그에 기록 후 허용.

```c
typedef struct driftmon driftmon_t;

/* 드리프트 심각도 — §2.3 임계값 */
typedef enum {
    DRIFTMON_STABLE      = 0,  /* PSI < 0.1 */
    DRIFTMON_WARNING     = 1,  /* 0.1 <= PSI < 0.2 */
    DRIFTMON_SIGNIFICANT = 2   /* PSI >= 0.2 */
} driftmon_severity_t;

/* 윈도우 누적 모델 — §2.5 */
typedef enum {
    DRIFTMON_TUMBLING = 0, /* 기본: window_size 누적 후 reset 호출까지 유지 */
    DRIFTMON_SLIDING  = 1  /* rolling: 항상 마지막 window_size 관측을 반영  */
} driftmon_window_mode_t;

driftmon_t* driftmon_create(const char* reference_json_path);  // 실패 시 NULL; TUMBLING 모드
driftmon_t* driftmon_create_ex(const char* reference_json_path,
                                driftmon_window_mode_t mode);  // 실패·비유효 mode 시 NULL
int  driftmon_num_features(const driftmon_t* m);              // psi_out 길이 산정용
void driftmon_observe (driftmon_t* m, const double* feats, int n); // 관측 1건 누적
int  driftmon_ready   (const driftmon_t* m);                 // 윈도우 찼으면 nonzero
void driftmon_compute (driftmon_t* m, double* psi_out, double* max_psi);
driftmon_severity_t driftmon_classify(double psi);           // 임계값 분류(무상태)

/* 알림 콜백 — compute 직후 호출; fn=NULL이면 해제 (Phase 6b, §7) */
typedef void (*driftmon_callback_t)(driftmon_t* m, double max_psi,
                                    driftmon_severity_t severity, void* user_data);
void driftmon_set_callback(driftmon_t* m, driftmon_callback_t fn, void* user_data);

void driftmon_reset   (driftmon_t* m);                       // 텀블링: 윈도우 초기화; 슬라이딩: no-op
void driftmon_destroy (driftmon_t* m);                       // NULL 안전
```

**의미 (semantics):**
- `driftmon_create`: 파일 없음/파싱 실패/스키마 불일치 시 `NULL`. `driftmon_create_ex(path, DRIFTMON_TUMBLING)`과 동일.
- `driftmon_create_ex`: `mode`가 유효하지 않은 값(0/1 이외)이면 `NULL` 반환.
- `driftmon_num_features`: 참조의 특징 수. caller가 `psi_out`을 안전히 할당하는 데 사용.
- `driftmon_observe`: `feats`는 `n`개의 double. `n`은 `driftmon_num_features(m)`와 일치해야 함.
- `driftmon_ready`: 누적 관측 수가 `window_size` 이상이면 nonzero. 슬라이딩 모드에서는 한 번 nonzero가 되면 계속 유지됨.
- `driftmon_compute`: `psi_out`(길이 ≥ num_features)에 특징별 PSI, `max_psi`에 최댓값.
  두 out 인자 모두 NULL 가능(건너뜀). 유효 관측 0인 특징은 PSI=0 (§2.4).
- `driftmon_classify`: PSI 값을 §2.3 임계값으로 분류. 무상태 순수 함수(핸들 불필요).
- `driftmon_set_callback`: `fn`이 비-NULL이면 `driftmon_compute`가 `psi_out`/`max_psi`를 확정한 직후 호출. STABLE 포함 모든 severity에서 호출됨(필터링은 caller 책임). `fn=NULL`이면 콜백 해제. `m=NULL`이면 no-op.
- `driftmon_reset`: **텀블링** — 참조는 그대로 두고 현재 윈도우 누적만 초기화. **슬라이딩** — no-op, rolling buffer 유지 (§2.5).
- `driftmon_destroy`: 모든 자원 해제. NULL 호출 안전.

---

## 5. 빌드 / 테스트 명령 (Build / Test) — 박제

```sh
cd /path/to/ddm
cmake -S . -B build && cmake --build build      # 경고 없이 빌드
ctest --test-dir build --output-on-failure       # 미니 러너 테스트 green
```
- 외부 의존성 0. 표준 라이브러리 + C++17만.
- **테스트 게이트:** 어떤 변경도 커밋 전에 위 빌드+ctest가 green이어야 한다.

---

## 6. 컨벤션 (Conventions)

- **언어/표준:** C++17. 코어는 표준 라이브러리만. 추론엔진 의존 금지.
- **헤더:** 공개 API는 `include/driftmon.h` (C ABI, `extern "C"`). 내부 헤더는 `src/`.
- **네이밍:** 공개 C API는 `driftmon_` 접두사 + snake_case. 내부 C++는 `dm`
  네임스페이스(불투명 `struct driftmon` 태그와 충돌 회피), 타입은 `PascalCase`,
  함수/변수는 `snake_case`.
- **포맷:** 들여쓰기 4 spaces(헤더 가독성 맞춤), 80~100열 권장.
- **에러:** 생성/로드 실패는 NULL/false로 신호. 코어는 예외를 던지지 않는다.
- **커밋:** 작은 단위로. 메시지는 무엇을/왜. 커밋 전 테스트 통과.

---

## 7. 결정 로그 (Decision Log)

진행 중 내려진 결정과 헤더/스키마 변경 합의를 시간순으로 남긴다.

- **2026-06-02 — Phase 0 부트스트랩 확정.** 범위는 계약·문서·빌드 골격·무의존
  테스트 러너까지. 테스트는 무의존 미니 러너(`tests/test_framework.h`), reference
  파싱은 최소 자체 JSON 파서(`src/json_min.*`). 헤더 동결. 원안 대비
  `driftmon_num_features` 추가(psi_out 길이 안전 산정).
- **2026-06-03 — Phase 1 코어 구현.** PSI/히스토그램/JSON 로더 완성. EPSILON=1e-4 확정.
- **2026-06-03 — 유효 관측 0 특징 처리 확정(버그 수정).** 모든 관측이 NaN/Inf로
  스킵된 특징은 epsilon floor가 PSI≈8을 만들어 max_psi를 오염시켰다. 이제 유효
  카운트 합이 0이면 PSI=0으로 보고(§2.4). 분모는 특징별 유효 카운트로 명시.
- **2026-06-03 — Phase 2: 윈도우 모델 = 텀블링(§2.5) 확정.** 슬라이딩은 비목적.
- **2026-06-03 — 헤더 하위호환 추가: `driftmon_classify` + `driftmon_severity_t`.**
  기존 시그니처 불변, 새 무상태 함수만 추가. 임계값(0.1/0.2) 단일 출처를 라이브러리에
  둠. C ABI 호환 유지(enum은 int).
- **2026-06-03 — Phase 4: reference 생성 도구 — 등빈도 quantile 방식으로 확정.**
  학습 데이터 크기에 무관하게 각 버킷이 비슷한 샘플 수를 가지므로 PSI 수치 안정성이
  좋음. stdlib-only Python(numpy 불필요). `DRIFTMON_ENABLE_TOOLS` CMake 옵션 추가.
- **2026-06-03 — Phase 3: export 어댑터를 의존성 없는 Prometheus 텍스트 포맷으로 구현.**
  원안의 "prometheus-cpp gauge"를 텍스트 노출 포맷 렌더러로 구체화 — 외부 의존성 없이
  실제 Prometheus 스크레이프 포맷을 생성하고 "기존 관측 인프라 재사용" 원칙에 더 부합.
  prometheus-cpp 클라이언트(레지스트리/HTTP exposer)는 같은 값을 Gauge API에 넘기는
  선택적 얇은 층으로 남김. 코어는 비의존, 옵션 OFF면 코어 빌드 무영향.
- **2026-06-04 — Phase 6: 슬라이딩 윈도우 API 설계 확정.** 헤더 하위호환 추가:
  `driftmon_window_mode_t` enum + `driftmon_create_ex(path, mode)` 신규. `driftmon_create`는
  `DRIFTMON_TUMBLING` 모드의 편의 래퍼로 유지(기존 호출 코드 무변경).
  `DRIFTMON_SLIDING` 모드: 길이 `window_size`의 circular buffer에 버킷 인덱스를 저장하고,
  `window_size` 이상 누적되면 `driftmon_ready`가 항상 nonzero를 반환.
  `driftmon_reset`은 슬라이딩 모드에서 no-op(rolling buffer 유지).

  ```c
  typedef enum {
      DRIFTMON_TUMBLING = 0,   /* 기본: window_size 채운 뒤 reset 호출까지 대기 */
      DRIFTMON_SLIDING  = 1,   /* rolling: 항상 마지막 window_size 관측을 반영 */
  } driftmon_window_mode_t;

  driftmon_t* driftmon_create_ex(const char* reference_json_path,
                                  driftmon_window_mode_t mode);
  /* driftmon_create(path) == driftmon_create_ex(path, DRIFTMON_TUMBLING) */
  ```

- **2026-06-04 — Phase 6: 알림 콜백 API 설계 확정.** 헤더 추가:
  `driftmon_callback_t` 함수 포인터 타입 + `driftmon_set_callback`. `driftmon_compute`가
  `psi_out`을 확정한 직후 콜백 호출. `fn=NULL`이면 해제. STABLE일 때도 호출되며
  severity 필터링은 caller 책임(정책·라이브러리 분리).

  ```c
  typedef void (*driftmon_callback_t)(driftmon_t*          m,
                                      double               max_psi,
                                      driftmon_severity_t  severity,
                                      void*                user_data);
  void driftmon_set_callback(driftmon_t*          m,
                             driftmon_callback_t  fn,
                             void*                user_data);
  ```

- **2026-06-04 — Phase 6: 다중 레퍼런스 프로파일 API 설계 확정.** 헤더 추가:
  `driftmon_create_multi(paths, n)`. n개 레퍼런스를 로드하며 모두 동일
  feature_names·num_buckets·window_size이어야 함(불일치 시 NULL). `driftmon_compute`는
  `psi_out[j]` = feature j에 대한 n개 레퍼런스 중 max PSI. `n=1`이면 `driftmon_create`와
  동일. `driftmon_create(path) == driftmon_create_multi(&path, 1)`.

  ```c
  driftmon_t* driftmon_create_multi(const char** paths, int n);
  ```

---

## 8. 로드맵 (Roadmap)

- **Phase 0 (완료):** 계약(`driftmon.h`)·SPEC·`CLAUDE.md`/`AGENTS.md`·`TASKS.md`·
  CMake 골격·무의존 테스트 러너. 컴파일·ctest green인 스텁.
- **Phase 1 (완료):** 코어 — 히스토그램 누적, `reference.json` 로더(최소 파서), PSI 계산.
  표준 라이브러리만. 단위 테스트 동봉.
- **Phase 2 (완료):** 윈도우/집계 — 텀블링 윈도우, 특징별 PSI→max PSI, 임계값 플래그
  (`driftmon_classify` + `driftmon_severity_t`).
- **Phase 3 (완료):** export 어댑터 — PSI를 **Prometheus 텍스트 노출 포맷**으로 렌더링
  (`export/`). 의존성 없음(표준 라이브러리만), `driftmon_classify` 재사용으로 severity
  gauge 포함. `DRIFTMON_ENABLE_PROMETHEUS` 빌드 옵션(**기본 OFF**) — OFF면 코어 빌드
  무영향. 의존 방향은 export→core 단방향.
- **Phase 4 (완료):** `tools/make_reference.py` — stdlib-only, CSV→reference.json.
  등빈도(quantile) 버킷 경계 산출, NaN/Inf 필터링, 단조성 보정, `--self-test` 내장.
  `tools/check_reference.cpp`로 C++ 왕복 통합 테스트. `DRIFTMON_ENABLE_TOOLS` 옵션.
- **Phase 5 (완료):** 통합 예제 — ONNX RT C++ 추론 래퍼에 glue 5줄
  (`examples/onnx_integration/onnx_glue.cpp`). DeepMIMO 요약 특징(RSRP·주경로 지연·
  최강 경로 각도)으로 Zone A→E drift 재현 데모(`examples/deepmimo_demo/`).
  `DRIFTMON_ENABLE_EXAMPLES` 옵션 + `deepmimo_e2e` ctest.
- **Phase 6 (계획):** 고급 모니터링 기능 세 가지 — 헤더 하위호환 추가(§7 결정 로그):
  - **슬라이딩 윈도우**: `driftmon_create_ex` + `driftmon_window_mode_t`. rolling buffer.
  - **알림 콜백**: `driftmon_set_callback`. compute 직후 severity 통보.
  - **다중 레퍼런스 프로파일**: `driftmon_create_multi`. n개 레퍼런스 중 max PSI.
