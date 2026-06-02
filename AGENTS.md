# AGENTS.md — Codex (및 기타 에이전트) 작업 규칙

**Read SPEC.md first.** SPEC.md is the single source of truth — algorithm (PSI),
data format (`reference.json`), the contract (`include/driftmon.h`), build/test
commands, conventions, and the decision log all live there. This file and
`CLAUDE.md` point to the same SPEC, so any tool can pick up the work with the
same assumptions.

## Core rules
1. **Frozen header.** Do not modify `include/driftmon.h` without agreement.
   Record the proposal/agreement in SPEC.md §7 (Decision Log) first, then change
   the header. Internal implementation is free to change.
2. **Inference-engine agnostic.** The core (`include/`, `src/`) must not import
   any inference-engine types (ONNX Runtime / LibTorch / OpenVINO / ...).
   Standard library only.
3. **Test gate.** Must pass before any commit:
   ```sh
   cmake -S . -B build && cmake --build build
   ctest --test-dir build --output-on-failure
   ```
4. **Small units.** One item in `TASKS.md` = one (or a few) commits. Check it
   off when done.
5. **Cross-implementation.** The other tool (Claude Code, `CLAUDE.md`) may take
   over. SPEC + frozen header + tests are the shared rails. When one tool writes
   the implementation, the other fills in tests and edge cases.

## Workflow
- Before starting, read `SPEC.md` then `TASKS.md`; pick the next item.
- Implement → add/pass tests → small commit → check off in `TASKS.md`.
- If SPEC and code conflict, fix SPEC first and record it in the decision log.
