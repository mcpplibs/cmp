# CMP Phase 6A Blocking Offload v1 Plan

**Date:** 2026-08-29
**Design:** `docs/superpowers/specs/2026-08-29-cmp-phase6a-blocking-offload-v1-design.md`
**Status:** Complete

## 1. Preserve the Phase 5 baseline

Keep the implemented `ThreadPool` contract unchanged. Phase 6A adds no worker, queue, shutdown,
executor base, or dependency; it only composes existing scheduling and Task behavior.

## 2. Add the single public helper

Create `src/blocking.cppm`, export it from `src/cmp.cppm`, and implement
`run_blocking(blockingWorkers, returnTo, operation, stopToken)` as one lazy function template.

Take both schedulers and the callable by value. Schedule to the blocking workers, invoke the owned
callable once, capture its value or exception, schedule back without cancellation, and only then
publish the outcome. Keep direct `void` and value branches; do not add futures or type erasure.

## 3. Lock the contract with deterministic tests

Add `tests/blocking_test.cpp` for laziness, worker/return affinity, value and void, move-only state,
exception transport, queued and late cancellation, scheduler lifetime failures, exactly-once race
behavior, structured concurrency, and stack safety.

Use latches, semaphores, atomics, and thread IDs. Do not use timing ratios as correctness checks.

## 4. Update the developer path

Extend `examples/basic` with one deterministic `run_blocking()` call and print its result after
returning to RunLoop. Keep the existing explicit ThreadPool calculation example so the difference
between manual migration and structured blocking offload remains visible.

Refactor only the file and loopback sections of `benchmarks/v1-readiness` from hand-written adapter
threads to one dedicated CMP ThreadPool plus `run_blocking()`. Preserve workloads and hard success /
failure counts so old and new reports remain comparable.

## 5. Synchronize public documentation

Update the three README and three architecture variants with the implemented API, explicit return
Scheduler, queued-only cancellation, dedicated ThreadPool-instance guidance, and the distinction
from kernel-native asynchronous I/O. Do not describe phase 6B as implemented.

## 6. Verify and record

Run focused tests first, then:

```text
mcpp build --profile dev --strict --cache=off
mcpp test --profile dev --strict --cache=off
mcpp build --profile release --strict --cache=off
mcpp test --profile release --strict --cache=off
cd examples/basic && mcpp run
cd benchmarks/v1-readiness && mcpp build --profile release --strict --cache=off
```

Run the focused cancellation race repeatedly and execute the readiness benchmark for measured
rounds. Update its report with new raw data rather than retaining adapter-thread measurements.
Finally update the Design status, this Plan, and `.agent/HANDOFF.md` with executed facts only.

## Completion record

Implemented the single `run_blocking()` API without a new pool type or dependency. Dev and Release
strict builds pass with 116/116 tests; the focused cancellation race passes 100 repeated Release
runs; the standalone example prints the blocking result; and five migrated readiness-benchmark
rounds pass with zero unexpected failures.
