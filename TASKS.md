# TASKS.md — 작업 티켓 (작고 검증 가능한 단위)

> 규칙: 항목 하나 = 커밋 하나(또는 소수). 완료 시 `[x]`. 커밋 전 빌드+ctest green
> (SPEC.md §5). 헤더 동결 준수(§4). 교차 분담: 한 툴이 **구현**하면 다른 툴이
> **테스트·리뷰·엣지케이스**를 채운다.

---

## Phase 0 — 부트스트랩 (완료)
- [x] `include/driftmon.h` 동결 계약
- [x] `SPEC.md` 정본 (알고리즘·포맷·계약·빌드·컨벤션·결정 로그·로드맵)
- [x] `CLAUDE.md` / `AGENTS.md` (둘 다 "Read SPEC.md first.")
- [x] `CMakeLists.txt` 빌드 골격 (의존성 0, C++17, ctest 연동)
- [x] `tests/test_framework.h` 무의존 미니 러너 + 스모크 테스트
- [x] `src/driftmon.cpp` / `src/json_min.*` 스텁 (컴파일·링크·green)

## Phase 1 — 코어 (의존성 없는 순수 C++) ✓
- [x] **구현:** `json_min` 최소 파서 — `reference.json` 한정 스키마 파싱 + 불변식 검증
      (SPEC §3). 실패 시 false.
- [x] **테스트:** reference 왕복/검증 — 유효 JSON 로드, 잘못된 스키마(엣지/비율 불일치,
      합≠1, 빈 features) 거부.
- [x] **구현:** 히스토그램 누적 — `driftmon_observe`로 특징별 버킷 카운트. 범위 밖
      클램프, NaN/Inf 무시 (SPEC §2.4).
- [x] **구현:** PSI 계산 — `driftmon_compute` 특징별 PSI + max_psi, epsilon-floor 적용.
- [x] **테스트:** 알려진 분포로 PSI 값 검증 — 동일 분포 ≈ 0; 시프트 분포 > 0.2.
- [x] **테스트:** 버킷 경계/빈 버킷/0 나눗셈 케이스 (SPEC §2.4).

## Phase 2 — 윈도우 / 집계 ✓
- [x] **구현:** 윈도우 관리 — `driftmon_ready`(window_size 기준), `driftmon_reset`.
      **텀블링 윈도우로 확정** (SPEC §2.5, 결정 로그).
- [x] **구현:** 임계값 플래그 — `driftmon_classify` + `driftmon_severity_t` 추가
      (하위호환, 결정 로그). 임계값 0.1/0.2 단일 출처를 라이브러리에 둠.
- [x] **테스트:** 윈도우 경계, reset 후 상태, 연속 텀블링 윈도우, 분류 경계값.
- [x] **버그 수정:** 유효 관측 0 특징이 epsilon floor로 허위 고PSI(≈8) 내던 문제 →
      PSI=0 보고 (SPEC §2.4, 회귀 테스트 추가).

## Phase 3 — export 어댑터 (코어와 분리, 선택) ✓
- [x] **구현:** PSI를 Prometheus 텍스트 노출 포맷으로 렌더링 (`export/`). 의존성 없음,
      `DRIFTMON_ENABLE_PROMETHEUS` 빌드 옵션(기본 OFF), 코어는 비의존(export→core 단방향).
      severity gauge는 `driftmon_classify` 재사용. 라벨 이스케이프·설정 가능한 메트릭명.
- [x] **테스트:** OFF 빌드에서 코어 무영향(export 타깃 미생성, 22 테스트 그대로),
      ON 빌드에서 export 모듈 빌드+테스트 5개 통과. 포맷/severity/이스케이프/길이불일치 검증.

## Phase 4 — reference 생성 도구 (오프라인) ✓
- [x] **구현:** `tools/make_reference.py` — stdlib-only Python 도구. CSV 입력으로 특징별
      등빈도(equal-frequency) 버킷 경계+비율 산출 → `reference.json` 생성 (SPEC §3).
      `--self-test` 모드, NaN/Inf 필터링, 단조성 보정, CLI 인터페이스.
- [x] **테스트:** Python 단위 13개(`tests/test_make_reference.py`) + CMake 통합:
      `ref_generate`(Python→JSON) → `ref_load_cpp`(`check_reference` C++ 왕복,
      DRIFTMON_ENABLE_TOOLS=ON). 전 옵션 조합 5/5 통과.

## Phase 5 — 통합 예제 ✓
- [x] **구현:** ONNX RT C++ 추론 래퍼 + glue 5줄 샘플 (`examples/onnx_integration/onnx_glue.cpp`).
      여기서만 추론엔진과 만남. `#ifdef ONNXRUNTIME_AVAILABLE` 가드로 의존성 선택적.
- [x] **데모:** DeepMIMO 요약 특징(RSRP·주경로 지연·최강 경로 각도)으로 Zone A→E
      drift 재현. `demo_driver.cpp` + `run_demo.py` + `gen_zones.py`. ctest `deepmimo_e2e` green.
      `DRIFTMON_ENABLE_EXAMPLES=ON` 빌드 옵션 추가.

## Phase 6 — 고급 모니터링 기능 (계획)

> API 설계는 SPEC.md §7 결정 로그(2026-06-04 항목 3건)에 확정됨. 헤더 변경은 모두
> 하위호환 추가(기존 시그니처 불변). 한 툴이 구현 → 다른 툴이 테스트·엣지케이스.

- [x] **구현:** 슬라이딩 윈도우 — `driftmon_window_mode_t` enum +
      `driftmon_create_ex(path, mode)` 추가. `DRIFTMON_SLIDING` 모드: 길이 `window_size`의
      circular buffer에 버킷 인덱스 저장. `driftmon_create(path)` 하위호환 유지.
- [x] **테스트:** 슬라이딩 윈도우 — `window_size` 이상 누적 후 `driftmon_ready` nonzero;
      `driftmon_reset` no-op; 연속 `driftmon_compute`가 rolling 분포를 반영하는지 검증.
      TUMBLING 모드 기존 동작 회귀 없음.
- [x] **구현:** 알림 콜백 — `driftmon_callback_t` 타입 + `driftmon_set_callback(m, fn, user_data)`.
      `driftmon_compute`가 `psi_out` 확정 직후 콜백 호출. `fn=NULL`이면 해제.
- [x] **테스트:** 콜백 호출 타이밍(compute 직후), NULL 해제 안전성, STABLE/WARNING/SIGNIFICANT
      각각에서 호출 확인, 콜백 미등록 시 crash 없음.
- [ ] **구현:** 다중 레퍼런스 프로파일 — `driftmon_create_multi(paths, n)`. n개 레퍼런스
      로드; `driftmon_compute`의 `psi_out[j]` = feature j의 n개 레퍼런스 max PSI;
      스키마 불일치(feature 수·버킷 수·window_size 다름) 시 NULL 반환.
- [ ] **테스트:** `n=1` 단일(기존 `driftmon_create`와 동일 결과), `n=2` max 선택 검증,
      스키마 불일치(feature 수 다름·window_size 다름) 거부, NULL 경로 안전성.
