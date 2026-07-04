# driftmon-cpp — 이어서 작업하기 (Handoff / Roadmap)

정본은 [../SPEC-cpp.md](../SPEC-cpp.md). 이 문서는 **다음에 이어받는 도구/사람**이 바로
시작할 수 있도록 남은 작업과 진입점을 정리한다. 코어 PSI 라이브러리(`../SPEC.md`)는 별개로
완료 상태다.

## 현재 상태 (2026-07-04)

- **Phase 1·2·3 완료 → SPEC-cpp AC1~AC10 전부 충족.**
- **보강 라운드(P1~P5, [REVIEW.md](REVIEW.md)) 완료** — SPEC-cpp §14 결정 로그 참고:
  탭 자기치유+생존 export(P1) / ABI v2: NaN bin+품질 채널(P2) / 샘플 링: raw 입력
  보존+알람 덤프(P3) / severity 번들화+디바운스(P4) / 다모델 TapHandle+출력 E2E+
  drift_kind 분류(P5).
- 코드: `cpp/` 트리, `DRIFTMON_ENABLE_CPP=ON`(기본 OFF, 에어갭). 빌드:
  ```sh
  cmake -S . -B build -DDRIFTMON_ENABLE_CPP=ON && cmake --build build
  ctest --test-dir build --output-on-failure   # cpp 18 테스트 + audit + E2E
  ```
- 동결 헤더 `include/driftmon.h` 불변. shm ABI(`cpp/include/driftmon/shm_abi.h`)는
  `DRIFTMON_SHM_VERSION=2`(NaN bin + 샘플 링). 탭 hot-path는 `nm` 심볼 audit로
  malloc/lock/throw/syscall 부재 보장 (p50 ~72ns).

### 컴포넌트 지도

| 영역 | 파일 | 비고 |
|---|---|---|
| shm ABI | `cpp/include/driftmon/shm_abi.h` | POD 레이아웃·golden static_assert. **변경 시 VERSION bump.** |
| 탭(핫/콜드) | `cpp/src/tap_hot.cpp`, `tap_init.cpp`, `tap_state.h`, `arena_rt.h` | hot TU 분리 유지(audit). |
| arena | `cpp/src/shm_arena.{h,cpp}` | create/attach/open_or_create(AC6 재사용). |
| 번들 | `cpp/src/bundle.{h,cpp}` | §5.4 스키마, R4.2 게이트. `dm::JsonParser` 재사용. |
| detector | `detector.h`, `psi_/ks_/cusum_/adwin_detector.*` | `Detector` ABC. |
| worker | `cpp/src/worker.{h,cpp}`, `worker_main.cpp` | `ModelMonitor`, `WorkerSet`(다모델). |
| export | `cpp/src/export.{h,cpp}` | Prometheus/JSON sink, best-effort. |
| 코어 핀 | `cpp/src/affinity.{h,cpp}` | `sched_setaffinity`. |

## 다음 작업 — Phase 4 (P2 확장 여지, SPEC §9)

> **모델 의존성 메타데이터 슬롯만 예약(귀속 로직은 미구현).** 현재 모델 간 의존성이 없어
> 단순 병렬 처리로 충분하나, 향후 "한 모델 출력이 다른 모델 입력" 구조가 생기면 귀속이 필요.

- [ ] **shm ABI 확장:** `SlotHeader`에 의존성 메타데이터 필드 추가(예: `uint32_t
      upstream_slot[K]` + count). v2의 `reserved[7]`을 소모하면 sizeof 불변으로 가능하나
      의미 변경이므로 `DRIFTMON_SHM_VERSION=3`으로 bump + `test_shm_abi` 갱신.
      **귀속 로직은 넣지 않음** — 필드만 예약.
- [ ] **번들 스키마:** optional `depends_on: [model_id...]` 파싱(검증만, 동작 없음).
- [ ] **문서:** SPEC-cpp §9 Phase 4 항목 + 결정 로그에 "예약만, 미구현" 명시.

## 운영 연계 / 향후 개선 (범위 밖이거나 후속)

우선순위 순. 각 항목은 독립적으로 착수 가능.

1. **다모델 단일 arena 옵션 (선택).** 현재는 모델별 arena(슬롯 1개) × N. SPEC §4 다이어그램의
   단일 arena·다중 슬롯이 필요하면 `DRIFTMON_MAX_SLOTS` 상향 + `arena_create_multi`(여러
   SlotSpec로 사이징) + 탭이 model_id로 슬롯 탐색. ABI VERSION bump 필요. 진입점:
   `shm_arena.cpp`, `tap_init.cpp`(슬롯 탐색), `shm_abi.h`(slot_offset[] 활용).
2. **모델 hot-add (재시작 없이).** 현재 AC8은 "번들 추가 + worker 재기동". 무중단 추가는
   `WorkerSet`에 디렉토리 watch(inotify) + 새 `ModelMonitor` 동적 추가. 진입점: `worker.cpp`.
3. **실제 export 전송.** 현재는 파일 아티팩트까지(Prometheus textfile / JSON). 실제 HTTP
   `/metrics` 노출이나 MinIO 업로드는 off-box. zero-dep 유지하려면 별도 옵션 모듈로(코어 비의존).
   진입점: `export.h`의 `Exporter` 인터페이스에 새 sink 추가.
4. **ADWIN 정식 알고리즘.** 현재는 단순화 변형(bounded 윈도우 분할). 전체 지수 히스토그램
   기반 ADWIN2로 교체 시 메모리·정확도 개선. 진입점: `adwin_detector.cpp`(인터페이스 불변).
5. **KS 임계 통계화.** 현재 `ks_threshold` 절대값. 표본수 기반 KS 임계(p-value, Dn,α)로
   바꾸면 더 원칙적. 진입점: `ks_detector.cpp` + 번들에 유의수준 필드.
6. **벤치 확장.** `bench_tap_latency`(탭) 외에 worker swap-read 처리량·다writer 스케일 벤치.
7. **RT 검증 (SPEC §10 Q4).** isolcpus/SCHED_FIFO 실노드에서 NFR1·NFR3 실측. 배포 환경 의존.
8. **다변량/상관 드리프트 (REVIEW A5).** 단변량 히스토그램은 피처 간 상관 붕괴를 못 본다.
   핵심 피처쌍 2~3개의 coarse 2D 히스토그램(예: 8×8) 슬롯 추가 — v2 `reserved[]` 활용 가능.
   전체 다변량은 off-box 몫. 진입점: `shm_abi.h`, `worker.cpp`.
9. **번들 hot-reload (REVIEW R4).** 번들 갱신 시 worker 재기동 없이 arena 재생성 + 탭은
   자기치유(P1)로 자동 재연결 — worker 쪽 reload 신호(SIGHUP/inotify)만 남음. 재학습 루프의
   "모델↔번들 버전 원자적 교체"와 연동. 진입점: `worker_main.cpp`, `bundle.h`(`model_version`).
10. **예측 신뢰도 채널 (REVIEW ②).** 분류 모델의 softmax 최대값/엔트로피 분포를 출력 피처
    하나로 탭 — 라벨 없이 concept drift를 근사하는 가장 싼 지표. 코드 변경 없이 번들
    `outputs`에 confidence 항목 추가로 가능(운영 패턴) — 예제/문서화만 남음.

## 미해결 질문 (SPEC §10 — 합의 필요, 코드 비블로킹)

- **Q1 (개발팀):** reference 번들을 학습 파이프라인 말미 `export_reference()`로 자동 생성?
  — Phase 2 전 합의 항목이었으나 프레임워크는 스키마만 알면 되므로 현재 비블로킹.
- **Q3 (운영):** export 타깃 우선순위(Prometheus vs MinIO) — 둘 다 구현됨, 구성으로 선택.
- **Q5 (개발팀):** 출력 텐서 의미(분류 logits vs 회귀) — 번들 `outputs`에 타입 힌트 필요 가능.
  현재 출력도 입력과 동일하게 히스토그램 탭(평탄 피처공간). 의미 구분이 필요하면 detector
  선택을 출력 타입별로.

## 작업 규칙 (이어받는 도구용)

- 매 변경마다 `cmake --build` + `ctest` 그린 유지(CLAUDE.md 테스트 게이트).
- shm ABI/탭 hot-path 변경 시: golden static_assert 갱신 + `tap_symbol_audit` 통과 확인.
- SPEC을 코드보다 먼저 갱신(SPEC-cpp §11~13 결정 로그에 기록).
- 개발 브랜치: `claude/driftmon-library-design-eguf8`.
