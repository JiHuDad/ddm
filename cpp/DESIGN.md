# driftmon-cpp 설계서 — 읽고 이해하는 문서

> 이 문서는 **오늘 만든 것이 무엇이고 왜 그렇게 만들었는지**를 처음 보는 사람도 따라올 수
> 있게 풀어 쓴 설계서다. 형식 규격은 [SPEC-cpp.md](../SPEC-cpp.md), 이어받기용 작업 목록은
> [ROADMAP.md](ROADMAP.md)에 있다. 이 문서는 그 사이의 "해설서"다.

---

## 0. 한 문단 요약

ML 모델을 서비스하는 C/C++ 프로그램이 추론을 돌릴 때, **입력·출력 값의 분포가 학습 때와
달라졌는지(드리프트)를 실시간으로, 추론 속도를 전혀 늦추지 않고 감시**하는 라이브러리를
만들었다. 핵심 아이디어는 두 가지다. ① 추론 경로에는 "값을 막대그래프(히스토그램) 칸에 +1"
하는 **아주 싼 동작(수십 나노초)**만 넣는다. ② 무거운 통계 계산은 **완전히 별개의 프로세스
(worker)**가 공유메모리를 통해 비동기로 읽어서 한다. 그래서 감시 로직이 아무리 무겁거나
죽어도 **서비스 추론은 절대 느려지거나 멈추지 않는다.**

---

## 1. 풀려는 문제와 제약 (왜 이렇게까지 하나)

RAN 서빙 노드에서 메인 프로그램이 서빙 `.so`를 링크하고, 그 안에서 ONNX Runtime으로 추론한다.
여기에 "모델 입력/출력 드리프트 감시"를 공용으로 얹어야 하는데, 두 가지가 절대 조건이다.

1. **추론 hot-path를 방해 금지.** line-rate(초당 수만~수십만 추론) 경로에 무거운 계산·메모리
   할당·락을 넣을 수 없다. 한 번이라도 멈칫하면 서비스 지연이 된다.
2. **운영은 모델별 수치를 모른다.** feature가 몇 개인지, 정상 분포가 어떤지는 **학습 데이터를
   가진 모델 개발팀만** 안다. 그래서 프레임워크는 숫자를 하드코딩하지 않고, 개발팀이 주는
   **reference 번들(설정 파일)을 읽어 동작하는 "일반 기계"**여야 한다.

이 두 제약이 아래 모든 설계 결정의 뿌리다.

---

## 2. 전체 그림 (한 장으로)

```
  ┌───────────────── 서빙 프로세스 (추론 담당, 빠름) ─────────────────┐
  │  preprocess(raw) → feat                                           │
  │  tap_update_input(feat)   ← 여기서 "히스토그램 칸 +1" (수십 ns)   │
  │  out = session.Run(feat)  ← ONNX 추론 (우리는 안 건드림)          │
  │  tap_update_output(out)   ← 여기서도 "칸 +1"                      │
  │  postprocess(out)                                                 │
  └───────────────────────────────┬──────────────────────────────────┘
                                   │ 공유메모리에 직접 기록 (lock-free)
                                   ▼
        ╔═════════ POSIX 공유메모리 arena (worker가 소유) ═══════════╗
        ║  slot: [ 더블버퍼 hist[2] | seq | active_idx | inflight ]  ║
        ╚═══════════════════════════════┬═══════════════════════════╝
                                        │ 주기적으로 active↔frozen 교체 후 읽기
                                        ▼
  ┌──────────────── drift worker 프로세스 (감시 담당, 느려도 됨) ────────────┐
  │  PSI · KS · CUSUM · ADWIN 로 드리프트 점수 계산 → 임계 비교 → 알람        │
  │  reference 번들에서 정상 분포·임계값을 읽어 비교                          │
  │  warm-up 가드: 표본 부족 구간은 판정 보류 (헛알람 방지)                   │
  └───────────────────────────────┬──────────────────────────────────────────┘
                                   │ export (best-effort)
                                   ▼
              off-box: Prometheus 메트릭 파일 / JSON 아티팩트(MinIO 등)
```

**기동 순서가 중요하다:** ① worker가 **먼저** 떠서 공유메모리를 만들고(주인), reference를
로드해 슬롯을 깐다. ② 서빙이 떠서 자기 슬롯에 **attach(연결만)** 한다. worker가 죽어도 서빙은
계속 쓰고(아무도 안 읽을 뿐), worker가 다시 뜨면 그 메모리를 **재사용**해 감시가 복구된다.

---

## 3. 핵심 데이터 구조 — 공유메모리 레이아웃

두 프로세스(서빙=writer, worker=reader)가 같은 메모리를 본다. 그래서 메모리 모양(ABI)을
헤더 하나(`cpp/include/driftmon/shm_abi.h`)로 **컴파일 타임에 고정**하고, 양쪽이 같은 버전이
아니면 attach를 거부한다.

```
[ ArenaHeader ]   magic, version, slot_count, slot_offset[]      (24 bytes)
      │
      ▼
[ SlotHeader ]    model_id[64], n_features, n_bins_total,        (1216 bytes)
                  bin_offset[]            ← 피처별 칸 시작 위치
                  ───── (전용 캐시라인) ─────
                  seq            ← 교체 진행 표시 (짝수=안정/홀수=교체중)
                  active_idx     ← writer가 지금 쓸 버퍼 번호 (0 또는 1)
                  inflight[2]    ← 각 버퍼에 "쓰는 중인 writer 수"
                  sample_count[2]← 각 버퍼의 표본 수
[ hist_buf[0] ]   uint64 카운터 배열  ← 버퍼 0 (더블버퍼)
[ hist_buf[1] ]   uint64 카운터 배열  ← 버퍼 1
```

**왜 더블버퍼?** writer는 한쪽(active) 버퍼에만 쓰고, worker는 주기적으로 "지금부터 반대쪽에
써"라고 바꾼 뒤, 방금까지 쓰던 버퍼(frozen)를 **느긋하게** 읽는다. 두 사람이 같은 칸을 동시에
건드릴 일을 구조적으로 줄인다.

**히스토그램 칸(bin) 배치 — "범위 밖"을 따로 센다.** 피처마다 정상 구간을 N개로 나눈
경계(`bin_edges`)가 있다. 우리는 칸을 **N+2개**로 만든다.

```
   bin 0        bin 1..N            bin N+1
 ┌────────┬──────┬──────┬──────┬──────────┐
 │underflow│      │ ...  │      │ overflow │
 └────────┴──────┴──────┴──────┴──────────┘
   경계보다 작음   정상 구간들      경계보다 큼
```

경계를 벗어난 값을 첫/끝 칸에 욱여넣지(clamp) 않고 **전용 underflow/overflow 칸에 따로
센다.** "값이 학습 때 보던 범위를 벗어났다"는 것 자체가 드리프트 신호이기 때문이다. (이게
기존 코어 PSI 라이브러리와 다른 점이라 설계 로그에 명시했다.)

---

## 4. 동작 흐름 ① — 서빙이 값을 기록한다 (hot path, 수십 ns)

`tap_update_input(feat, n)` 한 번이 하는 일 (`cpp/src/tap_hot.cpp`):

```
1. 지금 쓸 버퍼 번호(active_idx)를 읽고, "나 이 버퍼 쓰는 중"이라고 inflight++ 표시
2. 각 feature 값 x 마다:
     - NaN/Inf면 건너뜀 (값 손상에 안전)
     - x가 어느 칸에 들어가는지 이진탐색으로 계산 (underflow/overflow 포함)
     - 그 칸 카운터를 원자적으로 +1
3. 표본 수 +1, inflight-- (쓰기 끝 표시)
```

이게 전부다. **메모리 할당 없음, 락 없음, 파일 입출력 없음, 예외 없음, 시스템콜 없음.**
경계값(`bin_edges`)은 `tap_init` 때 한 번만 읽어 프로세스 메모리에 캐싱해 두므로(공유메모리가
아니라), hot-path는 자기 캐시 + 카운터 증가만 만진다.

> **이걸 어떻게 "증명"했나?** hot-path 함수만 별도 컴파일 단위(`tap_hot.cpp`)에 두고, 빌드된
> 오브젝트 파일의 미정의 심볼을 `nm`으로 검사하는 테스트(`tap_symbol_audit`)를 넣었다.
> malloc/operator new/__cxa_throw/pthread_mutex/mmap 같은 게 **하나도 없어야** 통과한다.
> 실제로 유일한 미정의 심볼은 데이터 포인터 `g_tap` 하나뿐이었다. 벤치(`bench_tap_latency`)
> 측정값: **p50 ≈ 77ns, p99 ≈ 99ns**(측정용 시계 호출 2회 포함 값), **hot-path 힙 할당 0회.**

**서빙은 절대 안 죽는다(degrade-to-no-op):** worker가 아직 안 떴거나, 번들이 깨졌거나,
메모리 모양이 안 맞으면 `tap_init`이 false를 돌려주고 이후 `tap_update_*`는 **조용한 no-op**이
된다. 감시가 안 될 뿐 서비스는 정상.

---

## 5. 동작 흐름 ② — worker가 읽고 판정한다 (느려도 됨)

worker는 주기적으로 한 슬롯에 대해 **교체(swap) → 비우기(drain) → 읽기 → 초기화**를 한다
(`cpp/src/worker.cpp`의 `slot_swap_read`):

```
1. seq++ (홀수: "교체 중")
2. active_idx 를 반대 버퍼로 바꿈   ← 이제부터 writer는 새 버퍼에 씀
3. drain: 옛 버퍼에 "쓰는 중"인 writer(inflight)가 0이 될 때까지 잠깐 기다림
4. 옛(frozen) 버퍼의 카운터를 전부 읽음
5. 옛 버퍼를 0으로 초기화 (다음 교체 때 깨끗하게 재사용 → uint64 오버플로 불가능)
6. seq++ (짝수: "안정")
```

읽은 히스토그램을 **윈도우에 누적**하고, 윈도우가 닫히면 detector로 판정한다.

**윈도우와 warm-up 가드 (헛알람 방지):** 윈도우는 "표본 N개가 모이거나(min_samples) T초가
지나면(max_seconds) 둘 중 먼저" 닫힌다. 단, **표본이 min_samples 미만이면 판정을 보류**한다
(데이터가 적을 때 우연한 분포로 헛알람 내는 걸 막는다). 시간이 윈도우를 닫아도 표본이 모자라면
"warming up"으로 보고 판정하지 않는다.

---

## 6. 왜 동시성이 안전한가 (제일 까다로운 부분, 쉽게)

여러 서빙 스레드가 같은 카운터에 동시에 +1 하고, worker가 그 사이 버퍼를 바꿔치기한다. 잘못하면
숫자가 깨지거나(corruption), 두 번 세거나(double-count), 빠뜨릴(lost) 수 있다.

**위험한 순간:** writer가 "버퍼 0이 active구나" 읽고 잠깐 멈춘 사이, worker가 버퍼를 1로 바꾸고
버퍼 0을 읽고 0으로 비워버린다. 그 다음 writer가 깨어나 버퍼 0에 +1 하면 → 그 숫자는 사라진다.

**우리의 차단법 (2단계):**
- writer는 `inflight++` 후 **active_idx를 한 번 더 확인**한다. 그 사이 바뀌었으면 되돌리고
  새 버퍼로 다시 시도한다. → "내가 쓰는 버퍼는 내가 inflight 표시한, 확인된 active다"가 보장됨.
- worker는 버퍼를 바꾼 **뒤에** inflight가 0이 될 때까지 기다린다(drain). 그래서 옛 버퍼에
  쓰던 writer가 끝나기 전에는 읽거나 비우지 않는다.

결과: **숫자 손상·중복 없음. 손실도 없음**(drain을 끝까지 기다리면). 병적으로 멈춘 스레드 때문에
worker가 영원히 못 기다리는 일이 없도록 상한(grace)을 두되, 그 경우에도 잃는 건 "그 순간 쓰던
극소수 카운트"뿐 — 수천 표본 히스토그램에는 통계적으로 무의미하다.

> **검증:** `test_arena_concurrency` — writer 스레드 4개가 각각 5만 번(총 20만) +1 하는 동안
> worker가 계속 교체·읽기를 반복. 읽은 합 + 마지막 버퍼 = **정확히 20만** (손실 0, 중복 0).
>
> **참고 — seqlock에 대하여:** SPEC은 슬롯에 `seq` 필드를 두지만, 우리 분석 결론은 **카운트
> 정합성은 seq가 아니라 위의 더블버퍼+drain이 책임진다**는 것이다. `seq`는 향후 외부
> 스냅샷 리더용 일관성 표시로만 유지한다(hot-path 영향 0). 이 점을 설계 로그에 못 박았다.

---

## 7. 드리프트 판정 알고리즘 4종 (직관)

모든 detector는 같은 인터페이스(`Detector`: `configure` + `eval(히스토그램)`)를 따른다. 번들의
`tests: [...]`로 피처별로 켠다.

| 이름 | 무엇을 보나 | 한 줄 직관 |
|---|---|---|
| **PSI** | 분포 전체의 모양 차이 | 칸별 (실제비율−기대비율)·ln(실제/기대) 합. 0.1 주의/0.2 위험. |
| **KS** | 누적분포의 최대 격차 | 기대 CDF와 실제 CDF가 가장 크게 벌어진 지점. 국소 이동에 민감. |
| **CUSUM** | 평균의 지속적 이동 | 평균이 기준에서 한쪽으로 계속 치우치면 누적합이 커져 알람(스트리밍). |
| **ADWIN** | 최근 흐름의 급변 | 최근 신호를 두 토막 내 평균이 크게 다르면 "체제 변화"로 보고 옛 토막 버림(스트리밍). |

- **PSI/KS는 배치형**(한 윈도우를 통째로 본다). **CUSUM/ADWIN은 스트리밍형**(윈도우들 사이에
  상태를 이어가며 누적 변화를 본다). 스트리밍 신호로는 "히스토그램의 평균 칸 인덱스"를 쓴다.
- PSI 공식·임계(ε=1e-4, 0.1/0.2)는 기존 코어와 동일하게 맞췄다(검증으로 패리티 확인).
- ADWIN은 정식 지수-히스토그램 알고리즘이 아니라 테스트 가능한 단순화 변형이다(tier-1 조기경보
  목적엔 충분, 정식화는 ROADMAP에 후속으로 기록).

> **검증:** `test_psi_detector`/`test_ks_detector`/`test_streaming_detectors` — 동일 분포면
> 점수 0, 분포를 인위로 흔들면 알람, 안정 구간 무알람·급변 시 검출.

---

## 8. 운영에 필요한 나머지 조각들

- **다모델 (1 worker = 여러 모델):** `WorkerSet`이 번들 여러 개를 읽어 모델별 감시기를
  띄운다. **한 모델 번들이 깨져도 그 모델만 비활성**되고(명확한 에러 기록) 나머지·서빙은 무영향.
  모델 추가는 **번들 파일 추가 + worker 재기동**이면 끝(코드 변경 0).
- **장애 격리/복구:** worker는 별도 프로세스라 죽어도 서빙에 전파 안 됨. 재기동 시
  `arena_open_or_create`가 **레이아웃이 맞는 기존 공유메모리를 비우지 않고 그대로 인수** →
  죽은 동안 서빙이 쌓던 데이터까지 보존돼 감시가 끊김 없이 복구된다.
- **export (off-box 전송):** 평가 때마다 모델별 {점수·알람·severity·히스토그램·세대번호·시각}을
  내보낸다. 두 방식 선택: **Prometheus 텍스트 파일**(임시파일+rename으로 원자적 교체 →
  스크레이퍼가 반쪽짜리 안 봄) 또는 **JSON 아티팩트 파일**(MinIO 적재용). **best-effort**:
  쓰기 실패해도 로그만 남기고 worker 루프는 계속(throw 없음).
- **코어 핀:** `--cpu 2,3`으로 worker를 housekeeping 코어에 묶어(`sched_setaffinity`) 서빙
  코어를 선점하지 못하게 한다(best-effort).

---

## 9. 운영↔개발 계약 — reference 번들 (예시)

개발팀이 학습 끝에 모델당 JSON 한 개를 준다. 운영은 이 스키마만 알면 된다(숫자는 몰라도 됨).

```json
{
  "schema_version": "1.0",
  "model_id": "beam_predictor_v3",
  "window": { "min_samples": 1000, "max_seconds": 60 },
  "tests": ["psi", "ks"],
  "features": [
    { "name": "sinr_db", "index": 0,
      "bin_edges": [-10, -6, -2, 2, 6],     // 칸 경계 (정상 구간을 나눔)
      "ref_hist":  [12, 45, 130, 30],        // 학습 분포 (같은 경계 기준 개수)
      "psi_threshold": 0.2 }
  ],
  "outputs": [ /* 동일 구조: 출력 분포 baseline */ ]
}
```

worker는 부팅 때 이걸 **검증**한다(필수 필드 존재, `bin_edges` 길이 == `ref_hist` 길이 + 1,
경계 단조 증가, 음수 없음, index 중복 없음 …). 위반하면 **그 모델만 감시 비활성 + 명확한 에러**.
"조용히 잘못된 값으로 도는" 일은 금지한다. 입력(`features`)과 출력(`outputs`)은 내부적으로 입력
먼저인 하나의 평탄한 피처 목록으로 합쳐 동일하게 다룬다.

---

## 10. 오늘 만든 파일 (역할별)

```
cpp/
  include/driftmon/
    shm_abi.h        공유메모리 모양(ABI). 양쪽이 공유하는 단일 계약. 버전·골든 크기 고정.
    tap.h            서빙이 쓰는 공개 API (tap_init / tap_update_input/output / tap_shutdown).
  src/
    arena_rt.h       writer 임계구역(드레인 가드) 인라인 — 탭과 테스트가 같은 코드로 검증.
    shm_arena.*      공유메모리 생성/연결/재사용(open_or_create).
    tap_state.h      탭 내부 상태 (핫/콜드 TU가 공유).
    tap_init.cpp     탭 셋업(콜드): 번들 로드·연결. 여기서만 할당/IO.
    tap_hot.cpp      탭 핫 패스(노알록/노락/노스로): 빈 계산 + 카운터 +1. 심볼 감사 대상.
    bundle.*         번들 파서/검증(게이트). 코어의 dm::JsonParser 재사용.
    detector.h       Detector 인터페이스 + 평균-칸 신호 헬퍼.
    psi_/ks_/cusum_/adwin_detector.*   드리프트 판정 4종.
    worker.*         ModelMonitor(슬롯 1개 감시) + WorkerSet(다모델). 스왑·윈도우·warm-up.
    export.*         Prometheus/JSON 내보내기 (best-effort).
    affinity.*       코어 핀(sched_setaffinity).
    worker_main.cpp  worker 실행 파일: 인자 파싱 → 번들 로드 → 루프.
  tests/             13개 테스트 + 심볼 감사 + 지연 벤치 (아래 검증 참고).
SPEC-cpp.md          형식 규격 + 설계 결정 로그(§11~13).
cpp/ROADMAP.md       이어받기용 남은 작업·진입점.
cpp/DESIGN.md        (이 문서) 해설서.
```

기존 코어에 손댄 곳은 **딱 하나**: `src/json_min.{h,cpp}`에서 검증된 JSON 프리미티브를
`dm::JsonParser`로 끌어올려 새 번들 파서가 재사용하게 한 것. **동결 헤더
`include/driftmon.h`는 불변**, 코어 동작 회귀 없음.

---

## 11. 빌드·실행·검증

```sh
# 빌드 (기본 OFF — 켜야 cpp 프레임워크가 빌드됨)
cmake -S . -B build -DDRIFTMON_ENABLE_CPP=ON && cmake --build build

# 테스트 (코어 + cpp 전부)
ctest --test-dir build --output-on-failure

# worker 실행 예시
./build/driftmon_worker --bundle ref.json --export prometheus \
    --export-target /var/lib/node_exporter/driftmon.prom --cpu 2,3
```

**검증 결과(오늘 기준):**
- 전 옵션 ON에서 **19/19 테스트 통과** (코어 6 + cpp 12 + 심볼 감사 1).
- `DRIFTMON_ENABLE_CPP=OFF` 빌드는 기존과 **byte-for-byte 동일**(회귀 0).
- 탭 지연 p50 ≈ 77ns, 힙 할당 0. 동시성 20만 카운트 정확 일치.
- worker 바이너리 end-to-end 기동 → clean SIGTERM 종료 시 공유메모리 정리 확인.
- **SPEC-cpp 수용 기준 AC1~AC10 전부 충족.**

---

## 12. 설계 결정 요약 (왜 그렇게 했나)

| 결정 | 이유 |
|---|---|
| 핫/콜드 TU 분리 | 핫 패스 오브젝트에 malloc/throw가 "없음"을 `nm`으로 증명하기 위해. |
| 경계 밖을 별도 칸(no-clamp) | 범위 이탈 자체가 드리프트 신호라서. |
| 경계값을 공유메모리 아닌 프로세스 메모리에 | 핫 패스 캐시 지역성 + 공유메모리 단순화. |
| 더블버퍼 + drain (seqlock 비의존) | writer를 절대 안 막으면서 손상/중복/손실 없이 읽기 위해. |
| 모델별 arena × N (단일 거대 arena 아님) | 장애 격리·모델 추가가 단순. 단일 arena 옵션은 ROADMAP에 후속. |
| arena 재사용(open_or_create) | worker 재기동 시 서빙이 쌓던 데이터 보존(무중단 복구). |
| export best-effort | 내보내기 실패가 감시 루프를 멈추면 안 되므로. |
| 새 `cpp/` 트리 + 기본 OFF | 동결 코어·기존 빌드에 영향 0, 에어갭 빌드 유지. |

---

## 13. 다음 단계

[ROADMAP.md](ROADMAP.md) 참조. 요약: **Phase 4**(모델 의존성 메타데이터 슬롯 *예약*, 귀속
로직은 미구현) + 운영 연계(단일 arena 옵션, 모델 무중단 추가, 실제 HTTP/MinIO 전송, 정식
ADWIN, KS 통계적 임계, RT 커널 실측). 각 항목에 진입점 파일이 적혀 있어 바로 이어갈 수 있다.
