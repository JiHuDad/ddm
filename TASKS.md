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

## Phase 4 — reference 생성 도구 (오프라인)
- [ ] **구현:** 학습 데이터에서 버킷 경계+비율 산출 → `reference.json` 생성 (Python
      또는 C++ 소도구).
- [ ] **테스트:** 생성된 JSON을 `json_min`이 왕복 로드.

## Phase 5 — 통합 예제
- [ ] **구현:** ONNX RT C++ 추론 래퍼 + glue 5줄 샘플. 여기서만 추론엔진과 만남.
- [ ] **데모:** DeepMIMO 요약 특징(RSRP·주경로 지연·최강 경로 각도)으로 Zone A→E
      drift 재현.
