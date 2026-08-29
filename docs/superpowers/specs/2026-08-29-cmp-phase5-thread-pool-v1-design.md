# CMP Phase 5 Thread Pool v1 Design

**Date:** 2026-08-29
**Status:** Implemented; local and Linux/macOS/Windows CI verified
**Baseline:** Phase 4 is implemented and locally verified with 90/90 tests

## Purpose

Add one fixed-size CPU worker pool that lets a coroutine explicitly move execution away from a
caller-thread `RunLoop`. Keep the developer-facing operation to one familiar expression:

```cpp
co_await workers.schedule();
```

Phase 5 does not make `Task` carry an implicit executor. A coroutine runs on the thread that resumes
it until it suspends or completes. It explicitly awaits another scheduler when it needs different
thread affinity.

The existing roadmap combines multi-worker scheduling and work stealing. This design separates
those decisions. A shared FIFO is the phase-5 v1 implementation because it is work-conserving,
auditable, and sufficient to measure the real contention of CMP workloads. Work stealing remains
an internal optimization gate, not a public promise: it receives a separate design only if the v1
benchmark identifies the shared queue as the limiting bottleneck.

## Public API

Add a `:thread_pool` module partition and export it from `mcpplibs.cmp`. The public surface is below;
private state constructors and storage are omitted:

```cpp
namespace mcpplibs::cmp {

class ThreadPool final {
public:
    class Scheduler final;

    ThreadPool();
    explicit ThreadPool(std::size_t workerCount);

    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;
    ThreadPool(ThreadPool&&) = delete;
    ThreadPool& operator=(ThreadPool&&) = delete;

    ~ThreadPool();

    [[nodiscard]] std::size_t thread_count() const noexcept;
    [[nodiscard]] Scheduler get_scheduler() const noexcept;
};

class ThreadPool::Scheduler final {
public:
    Scheduler() = delete;
    Scheduler(const Scheduler&) = default;
    Scheduler& operator=(const Scheduler&) = default;
    Scheduler(Scheduler&&) noexcept = default;
    Scheduler& operator=(Scheduler&&) noexcept = default;
    ~Scheduler() = default;

    [[nodiscard]] auto schedule() const noexcept;
    [[nodiscard]] auto schedule(std::stop_token token) const noexcept;

    friend bool operator==(
        const Scheduler& left,
        const Scheduler& right) noexcept;
};

}  // namespace mcpplibs::cmp
```

The default constructor uses `std::thread::hardware_concurrency()`, normalizing an unknown result
of zero to one worker. An explicitly requested count of zero is a caller error and throws
`std::invalid_argument`. Other thread-creation failures cleanly stop and join already-created
workers, then propagate the original exception.

`ThreadPool` is immovable because it is the visible lifetime anchor for its threads and Scheduler
identity. Scheduler is a cheap copyable handle backed by a weak reference to the pool state, like
`RunLoop::Scheduler`. Two handles compare equal exactly when they refer to the same pool state,
including after that state has expired.

No public `submit`, `spawn`, `detach`, `resize`, `close`, `join`, priority, affinity, or metrics API is
added. `Task`, `when_all()`, and `TaskGroup` remain the structured ownership layer.

## Intended Use

```cpp
Task<int> calculate(
    ThreadPool::Scheduler workers,
    RunLoop::Scheduler caller) {
    co_await workers.schedule();
    const auto result = expensive_calculation();

    // Only needed when the following code requires caller-thread affinity.
    co_await caller.schedule();
    co_return result;
}

int main() {
    ThreadPool workers {};
    RunLoop loop {};
    return loop.run(calculate(
        workers.get_scheduler(),
        loop.get_scheduler()));
}
```

Waiting on the pool always suspends, even when called by one of the pool's own workers. The resumed
coroutine may run on any worker, including the worker that submitted it. No stable-worker or
completion-order guarantee is exposed.

CPU work should be split into independently schedulable, finite pieces. A coroutine is
non-preemptive between suspension points: a long computation monopolizes one worker, and a
blocking system call blocks that worker. Native asynchronous I/O and the separate pool for
unavoidable blocking work remain phase 6.

## Queue and Worker Protocol

The private shared state contains only:

- one `std::mutex`;
- one `std::condition_variable`;
- one unbounded `std::deque` of ready entries;
- one `accepting` flag.

Each entry contains a coroutine handle and, for cancellable waits, a pointer to the outcome stored
inside the awaiting coroutine frame. The queue does not allocate a separate node owned by CMP;
normal `std::deque` storage growth can still allocate and can propagate `std::bad_alloc` before the
operation is published.

Enqueue is linearized under the state mutex:

1. reject if the state is no longer accepting work;
2. append the entry to the FIFO;
3. unlock;
4. call `notify_one()` for every accepted entry.

Every worker repeats:

1. wait with the predicate `!queue.empty() || !accepting`;
2. if the queue is empty and admission is closed, exit;
3. pop the oldest entry while holding the mutex;
4. select the entry's completion outcome, if any;
5. unlock and resume the coroutine.

User code is never resumed while the queue mutex is held. Predicate waits handle spurious wake-ups,
idle workers sleep instead of spinning, and worker-origin submissions wake sleeping siblings in
the same way as external submissions. FIFO describes dequeue order after concurrent producers are
linearized by the mutex; multiple workers can begin or finish those continuations in a different
order.

The queue is deliberately unbounded. A bounded queue can deadlock when all workers are occupied by
tasks that need to enqueue more work before they can finish. Backpressure requires a separate
asynchronous admission contract and is not smuggled into `schedule()`.

## Cancellation

`schedule(std::stop_token)` follows the existing Scheduler contract:

- it always queues while the pool is active, including for a pre-cancelled token;
- cancellation that wins before a worker claims the entry causes `await_resume()` to throw
  `OperationCancelled`;
- once the worker claims completion, a later stop request does not replace success;
- cancellation never resumes user code on the requesting thread.

The awaiter owns an atomic `pending/completed/cancelled` outcome and its `std::stop_callback`.
The callback performs only a compare-exchange from `pending` to `cancelled`. A worker performs the
competing compare-exchange from `pending` to `completed`, then resumes the continuation regardless
of which outcome won. A cancelled entry stays in the FIFO until consumed, avoiding queue scans,
iterator invalidation, and a second removal protocol.

`await_suspend()` first locks the weak pool state, then installs the callback, then attempts enqueue.
This ordering makes pre-cancellation observable without bypassing the queue and ensures an enqueue
allocation failure or concurrent close publishes no dangling entry.

`await_resume()` first destroys the stop callback, then reads the selected outcome. This prevents
the callback from accessing a destroyed awaiter. Once enqueue succeeds, another worker may resume
and destroy the awaiter before `await_suspend()` returns, so `await_suspend()` must not access any
awaiter member after publication.

An expired or closing Scheduler is a lifetime error, not a cancellation result. It throws
`std::logic_error` from the await expression even when the supplied token is already stopped,
because the pool never accepted that operation.

## Lifetime and Shutdown

Construction starts a fixed set of `std::jthread` workers. The pool owns execution threads but does
not own the Tasks whose continuations it resumes. The `jthread` stop token is not the shutdown
predicate: workers exit only after admission is closed and the accepted queue is empty, so automatic
stop requests cannot discard work.

Destruction performs graceful executor shutdown:

1. close admission under the state mutex;
2. wake every sleeping worker;
3. drain all entries accepted before the close linearization point;
4. let workers exit once the closed queue is empty;
5. join every worker before releasing the state.

A concurrent enqueue therefore has exactly two outcomes: it is accepted and drained, or it observes
closure and throws `std::logic_error`. Entries are never silently discarded. A continuation that
was accepted may complete, suspend onto another executor, or attempt another pool schedule. The
last case is rejected after closure. Pool destruction consequently guarantees that no pool worker
is running when destruction returns; it does not mean every higher-level Task has completed.
Structured owners must still await their Tasks.

Destroying a pool from one of its own workers cannot satisfy the join guarantee and would self-
deadlock. This is an invalid lifetime arrangement and terminates immediately. Destruction can also
wait indefinitely if user code running on a worker blocks indefinitely; the scheduler cannot
preempt arbitrary C++ code.

Scheduler handles may safely outlive the pool, but scheduling through them fails with
`std::logic_error`. An awaiting coroutine frame must remain alive until its accepted queue entry is
consumed. Existing CMP structured ownership satisfies this rule; phase 5 does not introduce a
detached escape hatch.

## Integration with Existing CMP Semantics

- `Task` promises do not gain scheduler storage or implicit propagation.
- A child Task and its parent continue on whichever thread completes the awaited child unless they
  explicitly schedule elsewhere.
- `RunLoop::run()` remains the caller-thread root boundary and can wait while work executes in the
  pool.
- `when_all()` and `TaskGroup` retain their cross-thread, exactly-once completion protocols and are
  the primary way to own parallel work.
- Timers stay on `RunLoop::Scheduler`; `ThreadPool::Scheduler` has no `schedule_after()` or
  `schedule_at()`.
- Events and `AsyncMutex` keep their documented resumption-thread behavior. Code that needs a
  particular thread awaits that thread's Scheduler explicitly.

The implementation is isolated in `src/thread_pool.cppm`; no generic executor base class, factory,
queue abstraction, or third-party dependency is introduced.

## Validation Contract

Cross-platform tests must cover:

- default construction, an explicit count, and rejection of zero;
- thread-count reporting and Scheduler equality for same versus different pools;
- `schedule()` always suspending and resuming on a pool worker;
- deterministic parallel occupancy of at least two workers using a test barrier rather than timing;
- one-worker FIFO dequeue order and multi-worker exactly-once execution;
- worker-origin rescheduling and nested fan-out without lost wake-ups or deadlock;
- values and exceptions through `Task`, `when_all()`, `TaskGroup`, and `RunLoop::run()`;
- explicit return from the pool to the original RunLoop thread;
- normal, pre-cancelled, queued-cancelled, and late-cancelled scheduling;
- cancellation versus worker claim selecting exactly one outcome under repeated races;
- concurrent producers, large queues, repeated scheduling, and native-stack safety;
- graceful draining of accepted entries, rejection during/after close, and expired Scheduler use.

Timing ratios are not correctness assertions. CI tests use latches, barriers, counts, and thread IDs
to establish observable contracts without depending on machine speed. Platform-specific death
testing is not required; self-destruction remains a documented hard lifetime invariant and the
implementation performs the check before joining.

## Performance Measurement and Work-Stealing Gate

Add one standalone Release benchmark consumer with no dependency beyond CMP and the standard
library. Record successful operation counts, unexpected failures, elapsed time, throughput, and
observed maximum concurrency for:

1. coarse independent CPU chunks at one and multiple worker counts;
2. repeated tiny `schedule()` hops, which stress the shared queue;
3. nested worker-origin fan-out, which exercises wake-up and utilization;
4. concurrent rescheduling producers, which stresses queue contention.

The benchmark reports raw data and does not fail CI on speedup thresholds. Correct counts and the
explicit multi-worker occupancy test are hard requirements; elapsed-time results are local
regression evidence.

Work stealing is considered only when profiling a representative workload shows that the shared
queue is the dominant scaling limit and a prototype gives a repeatable material improvement. Any
replacement must preserve the public API and additionally prove:

- local and external submissions always wake idle peers when runnable work exists;
- no task is lost during steal, cancellation, or shutdown races;
- external work cannot starve behind an endless local stream;
- idle workers do not spin indefinitely;
- accepted work is still drained on close;
- the full correctness and pressure suites remain clean.

This gate directly avoids a known failure mode in which a local-queue submission does not wake
sleeping workers, leaving a nominally multi-threaded pool non-work-conserving.

## Deliberately Excluded

- transparent migration, preemption, cooperative time budgets, and task priorities;
- per-worker public handles, pinning, NUMA policy, resizing, and custom allocators;
- detached task ownership or fire-and-forget submission;
- bounded admission and backpressure;
- timers, native asynchronous I/O, and blocking-call adaptation;
- an initial work-stealing implementation without benchmark evidence.

## Reference Rationale

- [cppcoro `static_thread_pool`](https://github.com/lewissbaker/cppcoro/blob/master/lib/static_thread_pool.cpp)
  demonstrates the small
  `co_await pool.schedule()` surface, but its local/global queues, stealing, spinning, sleeping, and
  wake-up protocol are substantially more complex than CMP needs before measurement.
- [stdexec issue #1305](https://github.com/NVIDIA/stdexec/issues/1305) documents a real
  non-work-conserving work-stealing failure and contrasts it with libunifex's simpler shared locked
  queue. Correct wake-up is therefore part of the contract, not a later tuning detail.
- [Boost.Asio `thread_pool`](https://www.boost.org/doc/libs/latest/doc/html/boost_asio/reference/thread_pool.html)
  validates the familiar fixed-thread RAII execution-context shape.
- [Folly `CPUThreadPoolExecutor`](https://github.com/facebook/folly/blob/main/folly/executors/CPUThreadPoolExecutor.h)
  uses an unbounded concurrent queue by default and warns about deadlocks caused by blocking bounded
  queues when pool work recursively submits work.
- [Tokio runtime scheduling](https://docs.rs/tokio/latest/tokio/runtime/) shows that production work
  stealing also needs local/global fairness, bounded local queues, wake-up rules, and cooperative
  budgeting; it is not merely replacing one deque with several.
- [oneTBB task scheduler](https://oneapi-spec.uxlfoundation.org/specifications/oneapi/v1.1-rev-1/elements/onetbb/source/task_scheduler.html)
  reinforces the CPU-oriented, non-preemptive boundary and the danger of blocking dependencies
  inside a finite worker set.

## Acceptance Criteria

Phase-5 v1 is complete: the module, tests, example, benchmark report, and
English/Simplified-Chinese/Traditional-Chinese public documentation agree; Dev and Release strict
builds and all 105 tests pass; the standalone example succeeds; and five local Release benchmark
rounds contain zero unexpected failures. PR #10 subsequently built the current library, ran the
current 140-test suite, and ran the standalone example successfully on Linux, macOS, and Windows.
