# CMP Cancellation v1 Implementation Plan

**Date:** 2026-08-23
**Goal:** Add exactly-once cooperative cancellation to relative and absolute Scheduler waits with
standard stop tokens, no inline coroutine resumption, and no allocation from stop callbacks.
**Design:** `docs/superpowers/specs/2026-08-23-cmp-cancellation-v1-design.md`
**Base:** `main` at `c06705c`
**Branch:** `feature/cancellation-v1`
**Toolchain:** C++23 Modules, mcpp 2026.8.11.2, LLVM 22.1.8, compat.gtest 1.15.2.

No Git or GitHub operation is part of implementation. Commit, push, PR, and merge remain separate
user permissions.

## Tasks

- [x] Confirm the merged Timer v1 baseline with the pinned mcpp environment and current 29 tests.
- [x] Add focused failing tests for the exception type, relative and absolute token overloads,
  pre-requested tokens, RunLoop-thread delivery, cross-thread wake-up, late cancellation,
  deadline/cancellation exactly-once races, non-earliest cancellation, invalid Scheduler use,
  cleanup/reuse, callback lifetime, and repeated pre-cancelled waits.
- [x] Export `OperationCancelled` from the existing RunLoop partition without adding a module or
  dependency.
- [x] Replace the private `std::priority_queue` with an explicit `std::vector` heap while preserving
  Timer v1 insertion, expiry, fairness, overflow, outstanding-work, and cleanup behavior.
- [x] Add the private cancellation outcome state and no-throw stop callback. Registration handles
  an already-requested token before publication; cancellation changes the matching Timer to an
  immediate heap entry and only notifies the RunLoop.
- [x] Add `schedule_after(Duration, std::stop_token)` and
  `schedule_at(TimePoint, std::stop_token)`. Both always suspend, deregister the callback in
  `await_resume()`, and throw `OperationCancelled` only when cancellation won.
- [x] Run focused Dev tests, then the complete Dev and Release build/test matrix. Repeat the race
  tests enough to expose double resume, lost wake-up, deadlock, or stale callback access.
- [x] Update `examples/basic` with one short `std::stop_source` demonstration after behavior passes.
- [x] Synchronize English, Simplified Chinese, and Traditional Chinese README/architecture
  contracts only after code and tests pass.
- [x] Update HANDOFF with actual verification and remaining cross-platform CI work.

## Implementation Constraints

- Reuse `std::stop_source`, `std::stop_token`, and `std::stop_callback`; do not add wrappers.
- Keep the existing one-argument Timer awaiters on their non-cancellable path.
- Store cancellation state in the token-taking awaiter's coroutine frame; do not heap-allocate an
  operation state.
- Timer entries remain non-owning and gain only one nullable cancellation-state pointer.
- All terminal outcome transitions occur under the existing RunLoop mutex.
- A pre-registration atomic flag closes the synchronous callback-construction race.
- The callback must be `noexcept`, must not push to `ready_`, and must perform no allocation.
- Reorder a cancelled Timer to `TimePoint::min()` and wake the RunLoop; only `drive()` promotes it
  to FIFO and resumes it.
- Deregister the stop callback without holding the RunLoop mutex before reading the terminal state.
- Preserve the existing rule that externally destroying a published coroutine frame is invalid.
- Keep cancellation O(n); do not add indices, intrusive nodes, tombstone registries, or lock-free
  queues without measured need.

## Test Discipline

- Prove the tests are red because the public cancellation API is absent before implementation.
- Use latches, semaphores, and explicit thread joining rather than timing guesses where possible.
- A liveness check must have a finite timeout and a later deterministic cleanup path.
- Never assert an ordering for genuinely simultaneous deadline/cancellation events; assert one
  resume and one valid terminal outcome.
- Include a controlled case where deadline completion is already queued before requesting stop.
- Keep existing Timer lower-bound assertions and 100,000-iteration stack-safety checks intact.
- Do not add test-only public hooks, fake clocks, dependencies, or allocator injection.

## Required Product Verification

```bash
mcpp --version
mcpp build --profile dev --strict --cache=off
mcpp test --profile dev --strict --cache=off --timeout 180
mcpp build --profile release --strict --cache=off
mcpp test --profile release --strict --cache=off --timeout 180
cd examples/basic
mcpp run
```

After a later user-authorized push, Linux x86_64, macOS arm64, and Windows x86_64 CI must all pass
before merge.

## Local Result

- The focused tests first failed because `OperationCancelled` and the token overloads did not yet
  exist, then passed after the implementation.
- Dev and Release builds passed with all 38 tests (8 Task and 30 RunLoop).
- The complete Release RunLoop suite passed 10 additional consecutive runs.
- The standalone example exited with status 0 and printed `Coroutine result: 42` followed by
  `Coroutine cancelled` from coroutine bodies.
- Manual Linux x86_64, macOS arm64, and Windows x86_64 workflows all passed for implementation
  commit `3a997ab`, including the build, 38 tests, and standalone example.

## Completion Gate

- API and documented semantics match the approved design.
- Every continuation reaches exactly one terminal outcome and is resumed at most once.
- Requesting stop never invokes user coroutine code inline and cannot throw from the callback.
- Callback-visible state outlives callback execution and is deregistered before awaiter teardown.
- Existing non-cancellable Scheduler and Timer behavior remains green.
- Local Dev, Release, repeated race tests, and standalone example pass.
