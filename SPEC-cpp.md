# SPEC: driftmon-cpp — C/C++ 서빙 환경 공용 드리프트 모니터링 프레임워크

| 항목 | 값 |
|---|---|
| Status | Draft v0.1 — Phase 1·2·3 구현 완료 |
| Owner | MLOps / 서빙 운영 |
| 대상 구현자 | Claude Code (SDD 워크플로우) |
| 언어/환경 | C/C++ (C++17), Linux, POSIX shm, 잠재적 RT 커널 |
| 비고 | 에어갭(폐쇄망). 외부 패키지 다운로드 불가 가정. |

> 이 문서는 핵심 PSI 라이브러리 정본인 [SPEC.md](SPEC.md) 위에 얹는 **서빙 프레임워크**
> 의 정본이다. 코어(`include/driftmon.h`, `src/`)는 불변이며, 본 프레임워크는 새 `cpp/`
> 트리에 격리되어 코어의 검증된 JSON 프리미티브(`dm::JsonParser`)와 PSI 공식만 재사용한다.

---

## 1. 배경 & 문제 정의

RAN 서빙 노드에서 C/C++ 메인 프로그램이 서빙 라이브러리(`.so`)를 링킹하고, 그 `.so` 내부에서
ONNX Runtime으로 추론을 수행한다. 운영 관점에서 이 서빙에 **모델 입력/출력 드리프트
모니터링**을 공용(共用)으로 얹어야 한다.

핵심 제약은 두 가지다.

- **추론 hot path를 방해하면 안 된다.** line-rate 추론 경로에 무거운 통계 계산·메모리
  할당·락을 넣을 수 없다.
- **운영은 모델별 수치(추론 레이트, feature 개수, 분포)를 모른다.** 이 값들은 학습 데이터를
  가진 모델 개발팀만 알 수 있으며, **reference 번들(설정 아티팩트)을 통해 주입**받아야 한다.
  따라서 프레임워크는 수치를 하드코딩하지 않고 *번들에서 읽어 동작하는 일반 기계*여야 한다.

모니터링 대상 모델은 현재 **3~5개**이며, **모델 간 의존성은 없다**. 따라서 모델별
귀속(attribution)은 단순 병렬 처리로 충분하다.

이 프레임워크는 상위 3계층 방어 전략 중 **tier-1(입력/출력 드리프트의 빠른 로컬 조기경보)**
에 해당한다. 정밀 비교·다변량·ground-truth 확정·상태머신은 off-box 중앙이 담당하며 본 SPEC의
범위가 아니다.

---

## 2. 목표 (Goals)

- G1. 서빙 `.so`의 ONNX 추론 경로에 **샘플당 O(1)** 비용의 탭(tap)을 추가해 입력 feature·
  출력값의 분포를 히스토그램으로 누적한다.
- G2. 탭이 누적한 데이터를 **별도 장수(long-lived) worker 프로세스**가 비동기로 읽어
  PSI/KS/ADWIN 기반 드리프트를 판정한다. 서빙을 한 번도 블로킹하지 않는다.
- G3. **단일 worker 프로세스**가 3~5개 모델을 `model_id`로 분기해 모두 처리한다.
- G4. 모델별 수치는 코드가 아니라 **reference 번들**에서 로드한다. 새 모델 추가가 코드 수정
  없이 가능해야 한다.
- G5. 데이터가 부족한 초기 구간에서 헛알람을 내지 않는다(**warm-up 가드**).
- G6. worker가 누적 히스토그램 스냅샷 + 드리프트 점수를 주기적으로 off-box로 **export**한다.

---

## 3. 비목표 (Non-Goals)

- N1. concept drift 판정 / ground-truth 라벨 수집 — tier-2/3 책임.
- N2. 모델 간 의존성 그래프 기반 귀속 — 미구현(단 `model_id` 분리는 유지, P2).
- N3. 중앙 상태머신 / 재학습 트리거 — off-box.
- N4. Evidently 등 Python 무거운 tier — 별도 시스템.
- N5. ONNX Runtime 그래프 수정 / custom op 삽입 — 금지. ORT는 안 건드리고 얇은 래퍼에서 탭만.
- N6. 드리프트 라이브러리 외부 의존 — ADWIN/PSI/CUSUM은 자체 구현(header-only, 단위 테스트 가능).

---

## 4. 아키텍처 개요

```
서빙 프로세스(메인+.so) ── preprocess → session.Run() ── tap(bin++ O(1)) ── 직접 쓰기
        │                                                                       │
        ▼  POSIX 공유메모리 arena (worker가 생성/소유, 서빙은 attach) ◀──────────┘
   slot[Mi]{ 더블버퍼 hist[2], seq, active_idx, inflight[2], sample_count[2] }
        │ tick마다 active↔frozen swap + drain, frozen read+zero
        ▼
   drift worker (먼저 기동, 1개) — PSI/KS/ADWIN, reference 비교·임계, warm-up
        │ export (hist 스냅샷 + score)
        ▼  off-box: MinIO/MLflow + KFP 배치 + 상태머신
```

**기동 순서:** ① worker 먼저 기동 → shm arena 생성(주인) → reference 로드 → 슬롯 확보.
② 서빙 프로세스 기동 → `.so` init → 자기 `model_id` 슬롯에 **attach** → reference bin
경계로 히스토그램 정렬. worker가 죽고 서빙이 살아있으면 탭은 attach 실패를 감지해 **조용히
no-op으로 degrade**(서빙은 절대 안 죽음)하고, worker 재기동 시 재attach.

---

## 5~10 (원본 SPEC)

구성요소별 요구사항(§5: 탭/arena/worker/번들/export), 인터페이스 정의(§6: 탭 API·shm ABI·
detector), NFR(§7), 수용 기준(§8), Phasing(§9), Open Questions(§10)은 최초 SPEC 초안을 따른다.
요지:

- **§5.1 탭(R1.x):** session.Run 앞/뒤 모델 입력/출력 공간에서 히스토그램 누적. hot path는
  bin 인덱스 + 카운터 증가만(malloc/lock/IO/throw 금지). reference 미로드/attach 실패 시 no-op.
  경계 밖 값은 **underflow/overflow bin에 별도 카운트(클램프 금지)**.
- **§5.2 arena(R2.x):** shm_open+mmap. worker 소유, 서빙 attach. 모델별 슬롯 = 더블버퍼 2개 +
  seq 카운터. writer lock-free 쓰기, worker가 active↔frozen swap 후 frozen만 읽음. 고정 arena.
- **§5.3 worker(R3.x):** 장수 프로세스. 루프마다 frozen 확보 → 윈도우(N개 OR T초) 검사 →
  충족 시 PSI 계산 → threshold 비교 → 알람/점수. 모델별 reference·detector 상태 분리.
- **§5.4 번들(R4.x):** 모델 개발팀이 학습 종료 시 산출하는 JSON 1파일(아래 §6 참고). worker가
  부팅 시 검증; 스키마 위반 시 해당 모델만 모니터링 비활성 + 명확 에러(서빙 영향 0).
- **§6.1 탭 API:** `bool tap_init(model_id, bundle_path)`,
  `void tap_update_input/output(const float*, size_t) noexcept`, `void tap_shutdown() noexcept`.
- **§6.2 shm ABI:** arena 헤더 + 슬롯(model_id, n_features, n_bins_total, seq, active_idx,
  더블버퍼 카운터, sample_count). 헤더 상수로 ABI 고정, version mismatch 시 attach 거부.
- **§6.3 detector:** `struct DriftResult{double score; bool alarm;}`,
  `class Detector{ virtual void configure(const FeatureRef&); virtual DriftResult eval(const Histogram&); }`.
- **§8 수용 기준:** AC1~AC10.

---

## 11. Phase 1 (P0 코어) 구현 노트 — 구현 시 확정된 결정

Phase 1 범위(SPEC §9): shm arena + 탭 API(no-op degrade) + 단일 모델 히스토그램 누적 +
worker PSI 1종 + warm-up. **AC1·AC2·AC5 충족.** 코드는 `cpp/` 트리, `DRIFTMON_ENABLE_CPP=ON`
(기본 OFF). 빌드: `cmake -S . -B build -DDRIFTMON_ENABLE_CPP=ON && cmake --build build &&
ctest --test-dir build`.

구현 중 SPEC을 앞서지 않도록(§11 주의) 다음 설계 결정을 확정·기록한다:

1. **No-clamp 비닝 (R1.5).** 코어 `driftmon_observe`(SPEC.md §2.4)는 범위 밖 값을 첫/끝 버킷에
   클램프하지만, 탭은 클램프하지 않고 **전용 underflow(물리 bin 0)/overflow(물리 bin B+1)**에
   별도 카운트한다. 따라서 피처당 물리 bin = 내부 bin + 2. 탭은 코어 비닝을 재사용하지 않는다.

2. **"Atomic swap" = swap + drain.** `active_idx` 단일 플립만으로는 in-flight writer를 보장
   못 한다. writer는 `inflight[idx]++` 후 active_idx를 **재확인**(바뀌었으면 back-out·retry)
   하고, worker는 swap(active=new) 후 `inflight[old]==0`까지 **drain**한 뒤 frozen을 읽고
   제로화한다. 이로써 lost count는 없다(무손상·무중복 — 동시성 테스트로 검증). drain은 유계
   grace로 상한을 둬 병적 stall에도 worker가 멈추지 않는다(타임아웃 시 in-flight만 드롭).

3. **seqlock은 카운트 보호용이 아니다.** count 정합성은 이중버퍼 + active_idx 플립 + drain이
   책임진다. 슬롯의 `seq`는 SPEC 요구 필드로 유지하되, 구조/스냅샷 읽기(향후 외부 리더)용
   일관성 봉투로만 쓰며 hot path엔 영향이 없다.

4. **edges는 shm이 아니라 프로세스 메모리에.** 탭이 `tap_init`에서 번들을 1회 로드해
   process-local 배열로 보유 → hot path는 L1/L2의 로컬 edges + shm 카운터만 접근. 탭이 자기
   번들로 `n_bins_total`을 재계산해 슬롯값과 불일치하면 attach 거부(no-op).

5. **번들 window 디폴트.** `window.min_samples`/`max_seconds` 미설정 시 1000 / 60초.

6. **출력도 탭.** 입력+출력을 입력 먼저인 하나의 평탄 피처공간으로 모델링(`n_features =
   n_in + n_out`), hot path 동일.

## 12. Phase 2 (P0 다중·계약) 구현 노트

Phase 2 범위(SPEC §9): 다모델 슬롯 + 번들 검증·게이트(다모델) + KS detector + 장애 격리.
**AC3·AC4·AC6·AC7·AC8 충족.** 추가 결정:

7. **모델별 arena (슬롯 1개) × N.** §4 다이어그램은 단일 arena에 다중 슬롯을 그리지만,
   구현은 **단일 worker 프로세스가 모델별 arena(`/driftmon.<model_id>`, 슬롯 1개)를 N개
   소유**한다. 기능 동일하되 (a) 장애 격리가 모델 단위로 자연스럽고, (b) 모델 추가(AC8)가
   새 shm 하나로 끝나며, (c) Phase 1 ABI/hot-path를 안 건드린다. `slot_count`/`slot_offset[]`
   는 향후 in-arena 다중 슬롯 확장 여지로 보존. `WorkerSet`이 G3(단일 worker, 다 model_id)을 담당.
8. **재기동 안전 reuse (AC6).** worker는 `arena_open_or_create`로 기동한다 — 레이아웃·model_id가
   일치하는 기존 arena가 있으면 **제로화 없이 그대로 인수**(실행 중 탭이 쌓던 데이터 보존),
   없으면 새로 생성. worker crash는 별도 프로세스라 서빙에 전파되지 않고(구조적 격리), 재기동
   시 기존 arena를 재연결해 모니터링이 복구된다.
9. **다중 detector / KS.** `bundle.tests`가 피처별 detector(PSI/KS)를 선택. 미지정 시 `["psi"]`.
   KS = 물리 bin 누적분포 최대격차, 임계 `ks_threshold`(미설정 0.1). 피처 점수 = 그 피처의
   detector 중 최대, 모델 알람 = 임의 detector 알람.
10. **다모델 게이트 (AC7).** `WorkerSet.load`는 잘못된 번들을 **건너뛰고 `errors()`에 기록**한
    뒤 나머지 모델을 정상 가동 — 한 모델의 스키마 위반이 형제 모델·서빙에 영향 없음.

## 13. Phase 3 (P1 운영화) 구현 노트

Phase 3 범위(SPEC §9): export + ADWIN/CUSUM 스트리밍 + 코어 핀 + 벤치 하네스.
**AC9·AC10 충족.** 추가 결정:

11. **스트리밍 detector (R3.4).** PSI/KS는 윈도우 배치, CUSUM/ADWIN은 **평가된 윈도우 간
    상태를 유지**하는 스트리밍 변형. 둘 다 신호로 **물리 bin 평균 인덱스**(`histogram_mean_bin`)를
    사용한다(에지 불필요). CUSUM = 기준 평균 대비 양/음 누적합, `h` 초과 시 알람 후 리셋.
    ADWIN = 최근 신호의 bounded 윈도우를 분할해 두 부분평균 차가 Hoeffding 한계를 넘으면
    변화로 보고 오래된 부분을 버림(단순화 변형, 전체 지수 히스토그램 알고리즘은 아님).
    `bundle.tests`에 `"cusum"`/`"adwin"` 추가로 활성화. 단순화상 윈도우 단위 갱신(원 SPEC의
    raw 스냅샷 단위가 아니라) — tier-1 조기경보에 충분.

12. **export (R5.x, AC9).** worker가 평가 시점에 모델별 {피처 점수·알람, severity, 히스토그램
    스냅샷, generation, timestamp}를 내보낸다. 두 sink를 구성 가능(`--export`):
    `prometheus`(텍스트를 tmp+rename로 원자적 파일 교체 — node_exporter textfile collector 패턴)
    또는 `file`(모델·seq별 JSON 아티팩트 — MinIO 적재 대상). **best-effort(R5.3)**: write 실패는
    false 반환·로깅만, worker 루프는 계속(throw 없음). 실제 MinIO 업로드/HTTP 노출은 off-box.

13. **코어 핀 (NFR3).** `--cpu 2,3`로 worker를 housekeeping 코어에 `sched_setaffinity` 핀(best-
    effort, 실패는 로깅 후 계속). worker는 SCHED_OTHER 유지(서빙 SCHED_FIFO 선점 불가).

14. **벤치 하네스 (AC1).** `bench_tap_latency`가 hot-path p50/p99 + 힙 할당 0을 측정·게이트.

### 수용 기준 진행 (Phase 1)

- [x] AC1. 탭 hot path 오버헤드 — 벤치 p50 ≈ 수십 ns, hot-path 힙 할당 0(전역 new 후킹).
      tap_update_* 오브젝트의 미정의 심볼에 malloc/new/throw/lock/syscall 없음을 `nm`
      audit로 정적 증명(`tap_symbol_audit`).
- [x] AC2. 단일 모델 히스토그램 누적 정확 — 탭 비닝(underflow/내부/overflow)·동시 writer +
      swapping reader에서 무손실·무손상·무중복(`test_tap_noop`, `test_arena_concurrency`).
- [x] AC5. warm-up 가드 — min_samples 미만이면 판정 보류, 시간이 윈도우를 닫아도 보류 유지
      (`test_warmup_guard`).
### 수용 기준 진행 (Phase 2)

- [x] AC3. reference 대비 입력 분포를 인위적으로 shift시키면 PSI/KS가 threshold 초과 알람
      (`test_detect` true_positive·out_of_range).
- [x] AC4. 정상(reference와 동일) 분포에서는 알람 미발생(`test_detect` false_positive_suppressed).
- [x] AC6. worker crash → 서빙 무영향(별도 프로세스), 재기동 시 기존 arena 재연결로 데이터·
      모니터링 복구(`test_fault_isolation`).
- [x] AC7. 번들 필수 필드 누락 시 해당 모델만 비활성 + 명확 에러, 형제 모델 무영향
      (`test_multimodel` gate_isolates_bad_bundle).
- [x] AC8. 새 모델(번들 추가)을 코드 수정 없이 등록 → worker 인식(`test_multimodel`
      code_free_model_registration; worker_main `--bundle`/`--bundle-dir`).
- [x] AC10(부분). KS 알려진 입력 기대 출력 검증(`test_ks_detector`).

### 수용 기준 진행 (Phase 3)

- [x] AC9. export 메트릭/파일에 모델별 점수·히스토그램·severity·generation·타임스탬프 기록
      — Prometheus 텍스트(원자적 파일 교체) + JSON 아티팩트 sink, best-effort(`test_cpp_export`).
- [x] AC10. PSI/KS/ADWIN/CUSUM 각각 알려진 입력 기대 출력 검증(`test_psi_detector`,
      `test_ks_detector`, `test_streaming_detectors`).

**Phase 1·2·3 = 전 AC(AC1~AC10) 충족.** 운영 연계(실제 MinIO 업로드/HTTP 노출, RT 커널 실측,
번들 산출 파이프라인 §10 Q1)는 off-box·배포 환경 영역으로 본 SPEC 범위 밖.

### 크로스-프로세스 E2E 데모 (실데이터 통합 검증)

`cpp/examples/deepmimo_cpp/` + ctest `deepmimo_cpp_e2e`: **별도 worker 프로세스 + 별도 tap
프로세스**가 진짜 POSIX shm으로 통신하는 전체 파이프라인을 DeepMIMO Zone A(LOS)/Zone E(NLOS)
합성 데이터로 검증한다. `gen_zones.py`(데이터) → `make_bundle.py`(분위수 번들) → `driftmon_worker`
(shm 생성·export) ⇄ `tap_driver`(CSV를 진짜 탭으로 재생). 결과: **Zone A → max PSI≈0.04 →
severity 0(STABLE)**, **Zone E → max PSI≈13.8 → severity 2(SIGNIFICANT)** 를 Prometheus export로
자동 단언. AC3/AC4를 합성 주입이 아니라 **크로스-프로세스 실데이터 흐름으로** 재확인한다.
(엔진 비의존이라 실제 ONNX 모델은 불필요 — CSV 피처 스트림이 모델 입력 공간 대역.)
