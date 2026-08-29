# CMP Phase 5 Thread Pool v1 Plan

**Date:** 2026-08-29
**Design:** `docs/superpowers/specs/2026-08-29-cmp-phase5-thread-pool-v1-design.md`
**Status:** Complete; local and Linux/macOS/Windows CI verified

## Completion record

The shared-FIFO `ThreadPool`, cancellation and shutdown protocol, 15 deterministic tests, basic
example, and standard-library-only benchmark are implemented. Dev and Release strict builds and
the complete 105-test suite pass; the focused Release suite passed 30 consecutive runs. Five
benchmark rounds completed 8,410,240 operations with zero unexpected failures. Coarse CPU work
scaled by about 7.56x from one to eight workers, while tiny-task contention confirmed the documented
v1 ceiling. Work stealing was therefore not added without a separate representative profiling case.
PR #10 later passed the current build, 140-test suite, and standalone example on all three CI
platforms.

## 1. Preserve and re-check the phase-4 baseline

Before implementation, work from an integrated phase-4 baseline and run the complete Dev suite
with cache disabled. Do not mix phase-4 delivery changes or phase-6 I/O work into the thread-pool
change.

## 2. Lock the public contract with focused tests

Add `tests/thread_pool_test.cpp` with the smallest deterministic tests for construction,
thread-count/Scheduler identity, always-suspending scheduling, worker identity, one-worker FIFO
order, explicit return to RunLoop, parallel occupancy, values, exceptions, and expired Scheduler
behavior.

Use standard latches/barriers and counts for concurrency properties. Do not use elapsed-time
speedups as correctness checks.

## 3. Implement the shared-FIFO worker core

Add `src/thread_pool.cppm` and export it from `src/cmp.cppm`. Use a fixed set of `std::jthread`
workers, one mutex/condition-variable/deque state, weak Scheduler handles, `notify_one()` for every
accepted entry, and resume only after releasing the state lock.

Implement exception-safe partial construction, explicit-zero rejection, default-count
normalization, queue-allocation failure propagation, graceful accepted-entry draining, close/enqueue
linearization, and self-destruction detection. Do not introduce a generic executor or queue layer.

## 4. Add cancellation and race coverage

Implement `schedule(std::stop_token)` with one atomic outcome in the awaiter. Cancellation and
worker claim compete with compare-exchange; the worker always consumes and resumes the entry.

Cover pre-cancel, queued cancel, late cancel, repeated claim races, concurrent producers, nested
worker submissions, graceful close, rejection, large queues, and stack safety. Re-run focused
Release tests enough times to expose missed wake-ups or double resumes.

## 5. Add one developer-facing example

Extend `examples/basic` with a short CPU calculation that schedules onto `ThreadPool`, explicitly
returns to the original `RunLoop`, and prints a deterministic result. Keep the example structured;
do not add detached work or blocking-I/O claims.

## 6. Measure before considering work stealing

Add a standalone standard-library-only Release benchmark for coarse CPU chunks, tiny schedule hops,
nested fan-out, and concurrent rescheduling at one and multiple worker counts. Validate counts and
unexpected failures, record environment and median results, and keep timing thresholds out of CI.

If profiling does not identify the shared queue as the dominant scaling bottleneck, stop here and
record that work stealing is not justified. If it does, write and review a separate internal
work-stealing design before implementation; preserve the public API and all wake-up, fairness,
cancellation, and shutdown contracts.

## 7. Synchronize documentation and verify delivery

Update all three README and architecture variants, the benchmark report, and `.agent/HANDOFF.md`
with implemented facts only. Run:

```text
mcpp build --profile dev --strict --cache=off
mcpp test --profile dev --strict --cache=off
mcpp build --profile release --strict --cache=off
mcpp test --profile release --strict --cache=off
cd examples/basic && mcpp run
```

Then run the standalone Release pressure consumer for multiple measured rounds. Record any check
that cannot run; do not describe an unexecuted check as passing.
