# CLAUDE.md — Claude Code 작업 규칙

**Read SPEC.md first.** SPEC.md가 정본이다 — 알고리즘(PSI), 데이터 포맷
(`reference.json`), 계약(`include/driftmon.h`), 빌드/테스트 명령, 컨벤션, 결정 로그가
모두 거기 있다. 이 파일과 `AGENTS.md`는 같은 SPEC을 가리킨다.

## 핵심 규칙
1. **헤더 동결.** `include/driftmon.h`는 합의 없이 수정하지 않는다. 변경이 필요하면
   먼저 SPEC.md §7 결정 로그에 제안·합의를 기록한 뒤에만 고친다. 내부 구현은 자유.
2. **추론엔진 비의존.** 코어(`include/`, `src/`)는 ONNX Runtime/LibTorch/OpenVINO 등
   어떤 추론엔진 타입도 import하지 않는다. 표준 라이브러리만.
3. **테스트 게이트.** 커밋 전 반드시 통과:
   ```sh
   cmake -S . -B build && cmake --build build
   ctest --test-dir build --output-on-failure
   ```
4. **작은 단위.** `TASKS.md`의 한 항목 = 한 커밋(또는 소수 커밋). 완료 시 체크.
5. **교차 작업.** 다른 툴(Codex, `AGENTS.md`)이 이어받을 수 있다. SPEC·동결 헤더·
   테스트가 공유 레일이다. 한 툴이 구현하면 다른 툴이 테스트·엣지케이스를 채운다.

## 작업 흐름
- 시작 전 `SPEC.md` → `TASKS.md`를 읽고 다음 항목을 고른다.
- 구현 → 테스트 추가/통과 → 작은 커밋 → `TASKS.md` 체크.
- SPEC과 코드가 충돌하면 SPEC을 먼저 고치고 결정 로그에 남긴다.
