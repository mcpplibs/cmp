# CMP `when_all` v1 Implementation Plan

**Date:** 2026-08-24
**Design:** `docs/superpowers/specs/2026-08-24-cmp-when-all-v1-design.md`
**Branch:** `feature/when-all-v1`
**Status:** Implemented and merged by PR #5

## 1. Lock the public contract with tests

- Add `tests/when_all_test.cpp` using only the exported `mcpplibs.cmp` module.
- Cover laziness, input start order, heterogeneous/move-only results, `void`, zero inputs,
  exception ordering, cross-thread completion, RunLoop reuse, and immediate-completion stress.
- Run the focused test and confirm it fails because `when_all` is not exported yet.

## 2. Implement the minimum join machinery

- Add `src/when_all.cppm` importing `std` and `:task`.
- Add a private atomic completion counter with one registration sentinel.
- Add private join coroutines that retain a value or exception for each input Task.
- Add the tuple-owned awaiter and exported variadic `when_all()`.
- Export the new partition from `src/cmp.cppm`.

## 3. Verify behavior and edge conditions

- Run the focused test, then the complete Dev suite with cache disabled.
- Review cross-thread happens-before, synchronous completion, exception cleanup, and destruction
  order against the design.
- Run Release tests and repeat the focused suite to expose race or lifetime instability.

## 4. Update the consumer and public facts

- Add one short coroutine-print example that joins two scheduled Tasks.
- Update English, Simplified Chinese, and Traditional Chinese README and architecture documents.
- Keep cancellation explicit and mark dynamic task scopes as remaining work.

## 5. Final verification and delivery

- Run strict Dev and Release builds, all tests, and the standalone example.
- Update `.agent/HANDOFF.md` with current implementation and exact verification results.
- Self-review the diff and correct all correctness or contract issues.
- Commit with a concise Chinese message, push the feature branch, create the PR, wait for all three
  platform checks, merge only when green, and fast-forward local `main`.

## Local Result

- The focused test first failed because `when_all` was absent, then passed 8/8 after implementation.
- Strict cache-off Dev and Release builds passed.
- Dev and Release each passed all 46 tests across three binaries.
- The Release `when_all` suite passed 50 consecutive runs.
- The standalone consumer printed `Coroutine result: 42`, `Concurrent result: 42`, and
  `Coroutine cancelled`, then exited with status 0.
- PR #5 passed Linux x86_64, macOS arm64, and Windows x86_64 CI before squash merge.
