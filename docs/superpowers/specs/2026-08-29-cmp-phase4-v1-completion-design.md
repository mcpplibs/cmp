# CMP Phase 4 v1 Completion Design

**Date:** 2026-08-29
**Status:** Implemented and locally verified
**Baseline:** 74/74 tests pass with mcpp 2026.8.11.2 and LLVM 22.1.8

## Purpose

Finish the three capabilities that the public roadmap leaves in phase 4:

1. recursive TaskGroup admission while a live child keeps the scope open;
2. explicit cancellation beyond timed waits;
3. one reusable, structured wake-up primitive.

This phase does not implement a multi-worker scheduler, an operating-system I/O backend, detached
ownership, or a blocking pool. Network and file readiness are measured through external adapters,
which is the interoperability boundary supported by v1.

## TaskGroup Quiescent Join

`TaskGroup::join()` still starts only when awaited and still succeeds once. Starting join no longer
closes admission immediately. A child already owned by the group may call `spawn()` while join is
waiting. The new child increments the same active counter before it starts, so join observes the
transitive closure of recursively spawned work.

The group becomes permanently joined when the active count reaches zero under the state mutex.
Admission after that point fails with `std::logic_error`. External concurrent admission racing the
last completion is deliberately not promised: callers may only rely on recursive admission from a
currently active child, matching the lifetime rule used by established async scopes.

`cancel_and_join()` is a lazy convenience Task. When awaited, it requests the existing standard
stop source and then performs the same single join. Cancellation remains cooperative and children
receive the token explicitly through `get_stop_token()`.

## General Scheduler Cancellation

Add `Scheduler::schedule(std::stop_token)`. Like every Scheduler operation it always queues before
resuming, including a pre-cancelled token. If cancellation wins before the ready entry is consumed,
`await_resume()` throws `OperationCancelled`; once consumption wins, a later stop request cannot
replace successful scheduling.

The RunLoop ready queue stores a continuation plus an optional cancellation-state pointer. Timer
and ordinary scheduling cancellation share the existing stop-callback protocol and one state lock.
Cancellation lookup remains O(n); a different data structure requires benchmark evidence.

Move `OperationCancelled` into a small `:cancellation` partition so reusable primitives can depend
on the exception without depending on RunLoop.

## AsyncManualResetEvent

Add an immovable `AsyncManualResetEvent` with this public surface:

```cpp
class AsyncManualResetEvent final {
public:
    explicit AsyncManualResetEvent(bool initiallySet = false) noexcept;

    [[nodiscard]] bool is_set() const noexcept;
    void set() noexcept;
    void reset() noexcept;

    [[nodiscard]] WaitOperation wait(std::stop_token token = {}) noexcept;
    [[nodiscard]] WaitOperation operator co_await() noexcept;
};
```

An unset wait registers an intrusive coroutine-frame node in FIFO order. `set()` publishes prior
writes, marks the event set, and resumes every registered waiter exactly once on the setter thread.
Later waits continue without suspension until `reset()`. Reset affects only future waits; waiters
selected by an earlier set still complete.

`wait(token)` uses a doubly linked queue so cancellation removes one pending waiter in O(1).
Registration, set, reset, and cancellation serialize under one short standard mutex. Set versus
cancellation has exactly one winner. A no-allocation thread-local dispatch trampoline flattens
nested wake-up chains.

Destroying the event with pending waiters terminates. Awaiting frames must remain alive until set or
cancellation completes. CMP does not add scheduler affinity: set and cancellation resume inline,
and a coroutine explicitly awaits its Scheduler to return to a RunLoop.

## Validation Contract

Functional and race tests cover:

- recursive admission before and during join, quiescent closure, exception order, and cancellation;
- pre-cancelled, queued-cancelled, and late-cancelled ordinary scheduling;
- reusable set/reset cycles, multiple waiters, publication, cancellation races, cross-thread wake,
  FIFO dispatch, and native-stack safety;
- all pre-existing public contracts.

A standalone Release benchmark records operation count, elapsed time, throughput, successes, and
failures for computation, temporary-file I/O, and loopback network I/O. I/O work runs in adapter
threads and resumes structured CMP tasks; results therefore validate integration and lifetime
safety, not native non-blocking I/O performance.

## Deliberately Excluded

- implicit cancellation hidden inside Task promises;
- cancellable AsyncMutex acquisition or mutation of OneShotEvent semantics;
- result handles, detached work, multi-worker scheduling, work stealing, I/O polling, or a pool;
- third-party dependencies or a benchmark framework.

## Reference Direction

- Folly `AsyncScope` permits adding work during join only when the caller is known to be inside
  already-added work and uses a barrier sentinel for quiescence.
- Folly `CancellableAsyncScope` documents cooperative token propagation and cancel-then-join.
- cppcoro `async_manual_reset_event` and Folly `Baton` use a set sentinel plus waiter collection;
  CMP keeps the observable reusable-event contract but chooses a standard mutex for cancellable
  O(1) removal and direct race auditing.

## Implementation Result

The three contracts are implemented without new dependencies. Dev and Release strict builds pass,
the complete suite contains 90 passing tests across seven binaries, and focused cancellation,
recursive-admission, and reusable-event races pass repeated Release runs. The standalone example
exercises every new public path. Compute, file-adapter, and loopback-adapter counts are recorded in
`docs/benchmarks/2026-08-29-cmp-v1-readiness.md`; every measured run had zero unexpected failures.
