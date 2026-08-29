# CMP Phase 6A Blocking Offload v1 Design

**Date:** 2026-08-29
**Status:** Implemented and locally verified
**Baseline:** Phase 5 is locally verified with 105/105 tests
**Verification:** Phase 6A passes 116/116 tests in Dev and Release

## Purpose

Provide one structured helper for running an unavoidable synchronous operation away from its
current executor and delivering its value, exception, or queued-cancellation result back through an
explicit return Scheduler:

```cpp
auto text = co_await run_blocking(
    blockingWorkers.get_scheduler(),
    caller,
    [path = std::move(path)] {
        return read_file(path);
    });
```

This is the first independently reviewable slice of roadmap phase 6. It makes blocking file,
database, resolver, and legacy-library calls safe to integrate without occupying a RunLoop or CPU
worker. It does not claim kernel-native asynchronous I/O.

Phase 6 is split because portable blocking isolation and native I/O have different contracts. The
first can reuse the implemented ThreadPool exactly. The second requires platform handles, buffer
ownership, completion dispatch, concurrent close, and operating-system cancellation semantics, and
will receive a separate phase-6B design before code is added.

## Decisions

1. A dedicated `ThreadPool` instance is the blocking pool; CMP does not add a second pool type with
   identical worker, queue, cancellation, and shutdown behavior.
2. `run_blocking()` owns the callable in its coroutine frame, invokes it once on the blocking pool,
   and then explicitly schedules back to a caller-selected Scheduler.
3. Values, `void`, exceptions, and queued cancellation are delivered only after the return schedule
   succeeds.
4. Cancellation may prevent a queued callable from starting, but cannot preempt a callable that a
   worker has already claimed.
5. No native socket or file type, global executor, detached ownership, or third-party dependency is
   introduced in phase 6A.

## Public API

Add one exported function in a `mcpplibs.cmp:blocking` module partition:

```cpp
template<typename ReturnScheduler, typename Function>
requires (
    std::move_constructible<Function> &&
    std::invocable<Function> &&
    (
        std::same_as<std::invoke_result_t<Function>, void> ||
        (
            std::is_object_v<std::invoke_result_t<Function>> &&
            !std::is_array_v<std::invoke_result_t<Function>> &&
            std::move_constructible<std::invoke_result_t<Function>>
        )
    ) &&
    std::move_constructible<ReturnScheduler> &&
    requires(const ReturnScheduler& scheduler) {
        scheduler.schedule();
    }
)
[[nodiscard]] Task<std::invoke_result_t<Function>> run_blocking(
    ThreadPool::Scheduler blockingWorkers,
    ReturnScheduler returnTo,
    Function operation,
    std::stop_token stopToken = {});
```

`Function` is taken by value deliberately. A coroutine initially suspends before its body runs, so
storing a forwarding reference could leave a temporary callable dangling. The by-value parameter is
copied or moved into the coroutine frame before the returned lazy Task is observed.

The callable is invoked as `std::invoke(std::move(operation))`. It may therefore own move-only state
and is consumed exactly once. Its result must be `void` or a move-constructible, non-array object
accepted by `Task<T>`; reference and array results are rejected rather than given surprising
lifetime semantics. Arguments belong in the callable capture, keeping the public overload set to
one function.

`ReturnScheduler` is a constrained template rather than an executor base class. Both existing CMP
Scheduler types already provide `schedule()`, so callers may explicitly return to a `RunLoop` or a
different `ThreadPool` without adding type erasure or virtual dispatch.

## Intended Use

The pool is an ordinary `ThreadPool` with a dedicated role and an application-chosen size:

```cpp
Task<std::string> load_text(
    ThreadPool::Scheduler blockingWorkers,
    RunLoop::Scheduler caller,
    std::filesystem::path path) {
    co_return co_await run_blocking(
        blockingWorkers,
        caller,
        [path = std::move(path)] {
            return read_file(path);
        });
}

int main() {
    ThreadPool blockingWorkers { 4 };
    RunLoop loop {};
    const auto text = loop.run(load_text(
        blockingWorkers.get_scheduler(),
        loop.get_scheduler(),
        "config.json"));
    std::println("Read {} bytes", text.size());
}
```

Applications should not use the same pool for latency-sensitive CPU continuations and operations
that may block for an unbounded time. Separate instances provide isolation without a duplicate CMP
type. There is no universal blocking-worker count, so phase 6A does not guess one or create a hidden
process-wide pool.

## Execution Protocol

`run_blocking()` is a normal lazy `Task` and performs these steps only when awaited:

1. await `blockingWorkers.schedule(stopToken)`;
2. if queued cancellation won, capture `OperationCancelled` without invoking the callable;
3. otherwise invoke the owned callable once on the selected worker;
4. store its result or capture its exception in the coroutine frame;
5. unconditionally await `returnTo.schedule()` without a cancellation token;
6. on the return Scheduler, rethrow the stored exception or cancellation, or return the stored
   value.

The first schedule always suspends while its ThreadPool is active. The operation therefore never
runs inline on the caller. A valid return schedule also suspends, so the awaiting parent observes
the outcome on the requested execution context. CMP still does not infer an ambient executor or
silently migrate arbitrary Tasks.

Worker-scheduler rejection, including an expired or closing pool, is captured like any other
failure and delivered after a valid return schedule. If the return Scheduler itself is expired,
closed, or inactive, its scheduling error propagates immediately from the current execution
context: CMP cannot provide return affinity after that lifetime contract has already been broken.

## Results and Exceptions

- A non-void result is stored in the helper coroutine frame and moved through the existing Task
  result path; no copy is required.
- Move-only callables and move-only results are supported.
- `void` uses a direct `if constexpr` path and does not require synthetic result storage.
- Any exception from scheduling onto the worker or invoking the callable is preserved with
  `std::exception_ptr`, then rethrown after returning to the selected Scheduler.
- An exception from the return schedule takes precedence because the requested completion context
  can no longer be reached.
- CMP does not wrap exceptions in a new error type or translate `std::system_error`; existing Task
  propagation remains the public error channel.

The implementation uses the two direct `if constexpr` branches needed for `void` and value results.
It does not add `std::future`, `std::packaged_task`, type-erased work items, or another result-state
abstraction.

## Cancellation

The optional token controls only admission to execution and reuses the ThreadPool's existing
pending/completed/cancelled race:

- a pre-requested token still queues, then skips the callable when a worker consumes the entry;
- cancellation that wins while queued skips the callable and later throws `OperationCancelled` on
  the return Scheduler;
- once a worker claims completion, the callable runs and its value or exception wins;
- requesting stop cannot terminate a synchronous system call or arbitrary C++ function;
- returning to `returnTo` is never cancellable, because cancellation must not strand the helper on
  a blocking worker or publish its outcome on the wrong executor.

A callable that supports cooperative cancellation may capture and inspect the same token itself.
`run_blocking()` does not inspect its signature, inject a token, or reinterpret the callable's own
result. Forceful thread cancellation is out of scope because it cannot preserve C++ object and lock
invariants.

## Lifetime and Shutdown

The blocking ThreadPool and the return Scheduler's owner must remain alive until the operation has
returned. The return `RunLoop::Scheduler` must additionally have an active `run()` while the helper
attempts to schedule back.

The callable object, its returned value, and its captured exception live in the helper coroutine
frame. Captures held by value are therefore safe across both scheduler transfers. Explicit reference
or pointer captures remain the caller's responsibility; phase 6A does not turn borrowed data into
owned data.

Destroying an unstarted `run_blocking()` Task destroys the owned callable without running it. Once
the worker entry is accepted, existing structured Task, `when_all()`, and `TaskGroup` ownership rules
must keep the awaiting frames alive. No detached path is added.

ThreadPool shutdown keeps its existing contract: it closes admission, drains accepted entries, and
joins workers. A callable that never returns can therefore keep its Task and pool destruction
blocked indefinitely. This is an inherent limit of wrapping synchronous work, not a condition CMP
can safely hide.

## Queueing and Backpressure

Phase 6A inherits ThreadPool's unbounded FIFO. `run_blocking()` adds no second queue and no extra
allocation beyond its coroutine frame, callable/result storage, and the underlying deque growth.

An application that can create blocking work faster than workers finish it must limit its own
in-flight structured tasks. A bounded blocking queue is deliberately excluded: synchronous
admission can deadlock recursive worker submissions, while asynchronous admission requires a new
backpressure and cancellation contract. That contract should be designed only for a measured use
case.

## Module and Implementation Boundary

Implementation changes are limited to:

- `src/blocking.cppm`: the exported function template;
- `src/cmp.cppm`: re-export the partition;
- `tests/blocking_test.cpp`: focused contract and race tests;
- `examples/basic/src/main.cpp`: one short file-style blocking example;
- `benchmarks/v1-readiness/src/main.cpp`: replace its external adapter threads with a dedicated CMP
  ThreadPool and `run_blocking()`;
- public documentation after behavior is implemented and verified.

`src/blocking.cppm` imports `std`, `:task`, and `:thread_pool`. It owns no threads or shared state and
does not depend on `:run_loop`; the return Scheduler is compile-time constrained by the expression it
uses. No installed dependency or `mcpp.toml` change is required.

## Validation Contract

Cross-platform tests must cover:

- laziness and exactly-once callable invocation;
- execution on a blocking worker and resumption on the requested RunLoop thread;
- returning to a different ThreadPool Scheduler;
- value, `void`, move-only callable, and move-only result paths;
- callable exceptions observed on the return Scheduler;
- pre-cancelled and queued-cancelled work never invoking the callable;
- late cancellation preserving the claimed callable's result;
- repeated cancellation-versus-claim races selecting one outcome;
- expired/closing blocking Scheduler and expired/inactive return Scheduler behavior;
- many concurrent offloads completing exactly once without growing the native call stack;
- one deliberately blocked callable while RunLoop continues processing another ready Task.

Correctness tests use latches, barriers, atomics, and thread IDs rather than elapsed-time ratios.
Filesystem behavior in the standalone example or tests uses temporary files and deterministic
cleanup. Cross-platform root tests do not introduce platform socket headers.

The existing POSIX-only `v1-readiness` benchmark is reused instead of adding another consumer. Its
file and loopback network workloads move from manually managed adapter `jthread`s to a dedicated
CMP ThreadPool plus `run_blocking()`. Success, expected-failure, and unexpected-failure counts remain
hard checks; timing remains local evidence and never a CI threshold.

## Native I/O Phase 6B Gate

`run_blocking()` is asynchronous relative to its caller but still consumes one operating-system
thread per running synchronous call. Documentation and examples must not describe it as non-blocking
or kernel-native asynchronous I/O.

Before phase 6B implementation, a separate Spec must choose and prove:

- the first resource family, preferably one narrow file or TCP surface rather than both at once;
- Linux, Windows, and macOS backend policy and any dependency policy;
- owned handle and buffer lifetimes across pending operations;
- completion-thread and explicit return-affinity behavior;
- immediate completion, partial transfer, EOF, retryable errors, and concurrent close semantics;
- cancellation versus completion with exactly-once resumption;
- event-loop shutdown with outstanding operations;
- deterministic platform tests and three-platform CI coverage.

Linux `io_uring`, Windows IOCP, and readiness-based event loops expose materially different
submission and completion rules. Phase 6B must not hide those differences behind an API before its
smallest portable contract is known.

## Deliberately Excluded

- a duplicate `BlockingPool` wrapper around ThreadPool;
- a hidden singleton or automatically sized blocking pool;
- native asynchronous file, DNS, socket, pipe, console, or process I/O;
- operation timeouts, forceful interruption, or thread replacement for stuck calls;
- detached submission, fire-and-forget ownership, or futures;
- implicit capture of the current Scheduler;
- callable argument forwarding overloads, priorities, bounded queues, and backpressure;
- executor type erasure or a public generic Scheduler concept.

## Reference Rationale

- [Tokio `spawn_blocking`](https://docs.rs/tokio/latest/tokio/task/fn.spawn_blocking.html) separates
  blocking work from async workers and documents the key limit that running synchronous work cannot
  be aborted; CMP keeps that boundary but uses structured Tasks and an explicit pool.
- [Boost.Asio `thread_pool`](https://www.boost.org/doc/libs/latest/doc/html/boost_asio/reference/thread_pool.html)
  demonstrates posting owned functions to a fixed worker set; CMP already has the required pool and
  therefore does not clone it.
- [cppcoro `resume_on()`](https://github.com/lewissbaker/cppcoro#resume_on) makes completion affinity
  explicit when an awaited operation finishes on another executor. `run_blocking()` adopts that
  observable rule without adding a general awaitable transform.
- [libunifex `io_uring_context`](https://github.com/facebookexperimental/libunifex/blob/main/doc/api_reference.md#linuxio_uring_context)
  combines a driven I/O context, scheduler, resource types, and asynchronous operations, showing why
  native file I/O is a separate subsystem rather than a blocking-pool alias.
- [Microsoft IOCP documentation](https://learn.microsoft.com/en-us/windows/win32/fileio/i-o-completion-ports)
  describes a completion queue tied to overlapped file and socket handles, reinforcing that a
  Windows native backend needs explicit handle and completion lifetimes.
- [liburing](https://github.com/axboe/liburing) exposes setup, submission, completion, feature, and
  kernel-version concerns for Linux `io_uring`; phase 6A deliberately takes on none of that platform
  surface.

## Acceptance Criteria

Phase 6A is complete only when the single `run_blocking()` API, focused cross-platform tests,
standalone example, updated readiness benchmark, public documentation, and HANDOFF agree on the same
contracts; Dev and Release strict builds and all tests pass; repeated cancellation races have zero
lost or duplicate invocation; file and loopback benchmark counts have zero unexpected failures; and
the change contains no native-I/O claim, duplicate pool type, hidden executor, or new dependency.
