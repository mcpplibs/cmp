# CMP

> **Coroutine Machine Process** — a C++23 coroutine runtime project.

[![C++23](https://img.shields.io/badge/C%2B%2B-23-blue.svg)](https://en.cppreference.com/w/cpp/23)
[![Module](https://img.shields.io/badge/module-mcpplibs.cmp-green.svg)](https://en.cppreference.com/w/cpp/language/modules)
[![License](https://img.shields.io/badge/license-Apache_2.0-blue.svg)](LICENSE)

**English** · [简体中文](README.zh.md) · [繁體中文](README.zh.hant.md)

[mcpp](https://github.com/mcpp-community/mcpp) · [Architecture](docs/architecture.md) ·
[v1 readiness benchmark](docs/benchmarks/2026-08-29-cmp-v1-readiness.md) ·
[thread-pool benchmark](docs/benchmarks/2026-08-29-cmp-thread-pool.md) ·
[Issues](https://github.com/mcpplibs/cmp/issues)

[![ci-linux](https://github.com/mcpplibs/cmp/actions/workflows/ci-linux.yml/badge.svg?branch=main)](https://github.com/mcpplibs/cmp/actions/workflows/ci-linux.yml)
[![ci-macos](https://github.com/mcpplibs/cmp/actions/workflows/ci-macos.yml/badge.svg?branch=main)](https://github.com/mcpplibs/cmp/actions/workflows/ci-macos.yml)
[![ci-windows](https://github.com/mcpplibs/cmp/actions/workflows/ci-windows.yml/badge.svg?branch=main)](https://github.com/mcpplibs/cmp/actions/workflows/ci-windows.yml)

> [!IMPORTANT]
> CMP provides a lazy, single-consumer `Task<T>` / `Task<void>`, structured variadic and vector
> `when_all()`, an eager structured `TaskGroup`, one-shot and reusable events, an RAII `AsyncMutex`,
> a caller-thread `RunLoop` with explicit and monotonic timed scheduling, and a fixed-size CPU
> `ThreadPool`. `run_blocking()` executes an owned synchronous callable on a dedicated pool instance
> and delivers its outcome through an explicit return Scheduler. Ready scheduling on either
> executor, timed waits, reusable-event waits, TaskGroup children, and queued blocking offloads can
> use explicit cooperative cancellation with `std::stop_token`; native asynchronous I/O and
> detached execution are not implemented.

CMP is being built as a modern coroutine runtime and library on standard stackless C++
coroutines. Its explicit `co_await` model now covers fixed and incremental structured concurrency,
one-time event notification, caller-thread and multi-worker scheduling, monotonic timers, and
cancellable waits, plus structured isolation of blocking work. Native asynchronous I/O remains a
separate, incremental design step.

## Why CMP?

The name stands for **Coroutine Machine Process**. CMP uses **C** as the intended name for a
lightweight coroutine execution unit. This is analogous to Go runtime's **G** as a naming and
mental-model inspiration only; it is not a claim that a future CMP task is already equivalent to
a goroutine.

The project is guided by a few principles:

- use C++23 standard stackless coroutines and C++ Modules;
- keep suspension explicit through `co_await` and purpose-built awaiters;
- develop runtime pieces incrementally, with tests and small reviewable changes;
- support more than server workloads;
- keep mcpp as the single source of build and package truth.

## Runtime Boundaries

C++ standard coroutines are a language mechanism, not a complete runtime. CMP therefore does not
promise that:

- a task is automatically equivalent to a Go goroutine;
- an arbitrary blocking call becomes non-blocking;
- coroutine switching is safe directly inside a signal handler;
- task migration is implicit, work stealing is already enabled, or arbitrary async I/O exists.

Those capabilities must be designed and verified individually. CMP now uses an explicitly
dedicated `ThreadPool` instance with `run_blocking()` for synchronous work; native async I/O awaiters
and cooperative safe points remain separate work.

## Quick Start

Install [xlings](https://github.com/openxlings/xlings), then install the mcpp version pinned by
[`.xlings.json`](.xlings.json):

```bash
xlings install
mcpp --version
mcpp build
mcpp test
```

Run the standalone consumer:

```bash
cd examples/basic
mcpp run
```

The example prints `Coroutine result: 42`, `Concurrent result: 42`, `Worker pool result: 42`,
`Blocking result: 42`, `Task group result: 42`, and `Recursive group result: 3`; it then demonstrates
notifications with `Event signalled` and `Reusable event cycles: 2`, prints `Mutex result: 42`, and
finishes with `Coroutine cancelled`. All messages come from `Task<void>` coroutines.

## Current API

```cpp
import std;
import mcpplibs.cmp;

using mcpplibs::cmp::Task;
using mcpplibs::cmp::RunLoop;
using mcpplibs::cmp::AsyncMutex;
using mcpplibs::cmp::OperationCancelled;
using mcpplibs::cmp::OneShotEvent;
using mcpplibs::cmp::TaskGroup;
using mcpplibs::cmp::ThreadPool;
using mcpplibs::cmp::run_blocking;
using mcpplibs::cmp::when_all;

using namespace std::chrono_literals;

Task<int> answer() {
    co_return 42;
}

Task<void> print_answer(RunLoop::Scheduler scheduler) {
    co_await scheduler.schedule_after(10ms);
    auto value = co_await answer();
    std::println("Coroutine result: {}", value);
    co_return;
}

Task<int> delayed_value(
    RunLoop::Scheduler scheduler,
    std::chrono::milliseconds delay,
    int value) {
    co_await scheduler.schedule_after(delay);
    co_return value;
}

Task<void> print_concurrent_results(RunLoop::Scheduler scheduler) {
    std::vector<Task<int>> tasks {};
    tasks.emplace_back(delayed_value(scheduler, 10ms, 20));
    tasks.emplace_back(delayed_value(scheduler, 1ms, 22));
    auto values = co_await when_all(std::move(tasks));
    std::println("Concurrent result: {}", values[0] + values[1]);
    co_return;
}

Task<int> calculate_on_workers(
    ThreadPool::Scheduler workers,
    RunLoop::Scheduler caller) {
    co_await workers.schedule();
    const int result = 21 * 2;
    co_await caller.schedule();
    co_return result;
}

Task<void> print_worker_result(
    ThreadPool::Scheduler workers,
    RunLoop::Scheduler caller) {
    const auto result = co_await calculate_on_workers(workers, caller);
    std::println("Worker pool result: {}", result);
    co_return;
}

Task<void> print_blocking_result(
    ThreadPool::Scheduler blockingWorkers,
    RunLoop::Scheduler caller) {
    const auto result = co_await run_blocking(
        blockingWorkers,
        caller,
        [] {
            std::this_thread::sleep_for(1ms);
            return 42;
        });
    std::println("Blocking result: {}", result);
    co_return;
}

Task<void> add_delayed(
    RunLoop::Scheduler scheduler,
    std::chrono::milliseconds delay,
    int value,
    int& total) {
    co_await scheduler.schedule_after(delay);
    total += value;
    co_return;
}

Task<void> print_task_group(RunLoop::Scheduler scheduler) {
    int total { 0 };
    TaskGroup group {};
    group.spawn(add_delayed(scheduler, 10ms, 20, total));
    group.spawn(add_delayed(scheduler, 1ms, 22, total));
    co_await group.join();
    std::println("Task group result: {}", total);
    co_return;
}

Task<void> set_event(RunLoop::Scheduler scheduler, OneShotEvent& event) {
    co_await scheduler.schedule();
    event.set();
    co_return;
}

Task<void> print_event(RunLoop::Scheduler scheduler) {
    OneShotEvent event {};
    TaskGroup group {};
    group.spawn(set_event(scheduler, event));
    co_await event;
    co_await group.join();
    std::println("Event signalled");
    co_return;
}

Task<void> add_locked(
    RunLoop::Scheduler scheduler,
    AsyncMutex& mutex,
    int value,
    int& total) {
    co_await scheduler.schedule();
    auto guard = co_await mutex.lock_async();
    total += value;
    co_return;
}

Task<void> print_mutex(RunLoop::Scheduler scheduler) {
    int total { 0 };
    AsyncMutex mutex {};
    TaskGroup group {};
    group.spawn(add_locked(scheduler, mutex, 20, total));
    group.spawn(add_locked(scheduler, mutex, 22, total));
    co_await group.join();
    std::println("Mutex result: {}", total);
    co_return;
}

Task<void> print_cancellation(RunLoop::Scheduler scheduler, std::stop_token token) {
    try {
        co_await scheduler.schedule_after(1s, token);
    } catch (const OperationCancelled&) {
        std::println("Coroutine cancelled");
    }
    co_return;
}

int main() {
    ThreadPool workers { 2 };
    ThreadPool blockingWorkers { 2 };
    RunLoop loop {};
    loop.run(print_answer(loop.get_scheduler()));
    loop.run(print_concurrent_results(loop.get_scheduler()));
    loop.run(print_worker_result(
        workers.get_scheduler(),
        loop.get_scheduler()));
    loop.run(print_blocking_result(
        blockingWorkers.get_scheduler(),
        loop.get_scheduler()));
    loop.run(print_task_group(loop.get_scheduler()));
    loop.run(print_event(loop.get_scheduler()));
    loop.run(print_mutex(loop.get_scheduler()));

    std::stop_source source {};
    source.request_stop();
    loop.run(print_cancellation(loop.get_scheduler(), source.get_token()));
}
```

`Task` is lazy: calling `answer()` creates a suspended coroutine. It starts when consumed by
`co_await`. A Task is move-only, has one consumer, and can only be awaited as an rvalue. It stores
either a value or an exception, transfers directly between child and continuation, and destroys
an unconsumed frame through RAII. `Task<T&>`, copying, move assignment, and detached execution are
deliberately unsupported.

`when_all()` is also lazy. Awaiting it starts every input Task from left to right without waiting
for earlier suspended inputs, then keeps all child frames alive until every input completes.
Results are returned in parameter order as a `std::tuple`; `Task<void>` contributes
`std::monostate`. If children fail, all children still finish before the first exception in
parameter order is rethrown. Named Tasks must be moved into `when_all()`. A runtime-sized,
homogeneous collection can be passed as `std::vector<Task<T>>`; it returns a result vector in the
same index order, and a named input vector must also be moved.

`TaskGroup::spawn()` takes ownership of a `Task<void>` and starts it immediately. The single-use
`join()` waits until the group reaches quiescence; an active child may recursively add work while
join is waiting. Admission closes permanently when the active count reaches zero. The group must
be joined before destruction. `get_stop_token()` and `request_stop()` provide one explicit
standard cancellation channel, while `cancel_and_join()` requests that channel before joining.
The token is not injected automatically. Use `when_all()` when child results must be returned.

`co_await event` suspends on an unset `OneShotEvent` without allocating. The first thread-safe
`set()` permanently sets it and resumes every registered waiter exactly once on the setter thread;
later waits continue inline and later sets do nothing. The event is immovable and must outlive its
waiters. It deliberately has no reset or implicit Scheduler transfer.

`AsyncManualResetEvent` adds reusable `set()` / `reset()` cycles. `wait(stop_token)` supports
cooperative cancellation, pending waiters are resumed in FIFO order, and set-versus-cancel has one
stable winner. Resumption occurs on the thread calling `set()` or requesting cancellation; await a
Scheduler explicitly when RunLoop affinity is required. The event must outlive every waiter.

`auto guard = co_await mutex.lock_async()` acquires `AsyncMutex` without allocating a waiter and
releases it through RAII. Contended waiters acquire in FIFO order and resume on the releasing
thread; ownership is not tied to a thread. The mutex must outlive every guard and queued waiter.
v1 exposes no manual unlock, try-lock, cancellation, or implicit Scheduler transfer.

A translation unit that defines a coroutine must import `std` so the compiler can see the standard
coroutine protocol types. CMP imports `std` privately and does not re-export the whole standard
library.

`RunLoop::run()` consumes one root Task, executes ready coroutines on the calling thread, returns
its value, and rethrows its exception. `Scheduler::schedule()` always suspends and queues the
continuation; its `std::stop_token` overload can cancel that queued wait. `schedule_after()` waits
for a relative `steady_clock` duration, while
`schedule_at()` waits for an absolute steady-clock time point; expiry makes work eligible and
never resumes it inline. Scheduler handles are copyable, but remain tied to their originating
RunLoop. Sequential `run()` calls are supported; nested or concurrent calls are rejected. A
moved-from Task must not be awaited.

All Scheduler overloads accepting `std::stop_token` still queue, including pre-cancelled waits. If
cancellation wins before consumption, awaiting throws `OperationCancelled`; a late stop request
cannot replace a completion that already won. The stop callback only wakes the RunLoop, so user
coroutine code is resumed by the thread driving `run()`. Cancellation currently performs an O(n)
queue lookup.

`ThreadPool` starts a fixed number of CPU workers. Its copyable Scheduler always suspends and may
resume a continuation on any worker; explicitly await a `RunLoop::Scheduler` to return to the
caller thread. `schedule(stop_token)` has the same completion-versus-cancellation winner rule as
RunLoop ready scheduling. The v1 implementation uses one work-conserving shared FIFO and sleeping
workers. Destruction closes admission, drains every accepted entry, and joins the workers. It does
not own higher-level Tasks, and blocking calls still block their current worker.

`run_blocking(blockingWorkers, returnTo, operation, stopToken)` owns a synchronous callable in its
lazy Task frame, schedules it once to the selected ThreadPool, and publishes its value, `void`,
exception, or queued cancellation only after scheduling back to `returnTo`. Use a separate
ThreadPool instance for blocking work so it cannot occupy latency-sensitive CPU workers. A stop
request can skip work that has not been claimed; it cannot preempt a running synchronous call, and
the return schedule is intentionally not cancellable. This is thread-based isolation, not native
non-blocking I/O.

RunLoop is not a background thread and does not make blocking code asynchronous. A Task that
suspends without arranging a future resume can leave `run()` waiting indefinitely. CMP does not
provide automatic thread affinity: after an external awaiter resumes on another thread, explicitly
await the desired Scheduler to return to its RunLoop.

## Repository Layout

```text
.
├── .xlings.json              # pinned project tool environment
├── mcpp.toml                 # package identity and test dependency
├── src/cmp.cppm              # root module interface
├── src/task.cppm             # Task module partition
├── src/cancellation.cppm     # shared cooperative-cancellation exception
├── src/run_loop.cppm         # RunLoop and Scheduler partition
├── src/thread_pool.cppm      # fixed-size CPU worker scheduler
├── src/blocking.cppm         # structured blocking-call offload
├── src/when_all.cppm         # structured concurrent Task join
├── src/task_group.cppm       # eager mutable structured Task scope
├── src/one_shot_event.cppm   # allocation-free one-time notification
├── src/async_manual_reset_event.cppm # reusable cancellable notification
├── src/async_mutex.cppm      # FIFO coroutine-aware RAII mutex
├── tests/cmp_test.cpp        # Task contract and lifetime tests
├── tests/run_loop_test.cpp   # scheduler, boundary, and threading tests
├── tests/thread_pool_test.cpp # worker, cancellation, and shutdown tests
├── tests/blocking_test.cpp   # blocking offload, affinity, and cancellation tests
├── tests/when_all_test.cpp   # join ownership, result, and race tests
├── tests/task_group_test.cpp # mutable scope lifetime and race tests
├── tests/one_shot_event_test.cpp # event publication and race tests
├── tests/async_manual_reset_event_test.cpp # reusable event race tests
├── tests/async_mutex_test.cpp # mutex ownership and hand-off tests
├── examples/basic/           # standalone path-dependency consumer
├── benchmarks/v1-readiness/  # local compute, file, and loopback baseline
├── benchmarks/thread-pool/   # local multi-worker scheduling baseline
├── docs/architecture.md      # current structure, boundaries, and evolution
└── .github/workflows/        # Linux, macOS, and Windows CI
```

The repository does not ship `mcpp new` templates yet. Purpose-built templates can be added
after CMP has a stable runtime API worth demonstrating.

## Development

The local verification path is:

```bash
mcpp build --profile dev --strict --cache=off
mcpp test --profile dev --strict --cache=off
mcpp build --profile release --strict --cache=off
mcpp test --profile release --strict --cache=off
cd examples/basic && mcpp run
```

CI runs the equivalent build, test, and standalone example flow on Linux, macOS, and Windows.
The mcpp version is pinned by `.xlings.json`; contributors should not rely on an unrelated
global mcpp installation.

CMP does not track `mcpp.lock`; `.gitignore` enforces that repository policy. Runtime dependencies
belong in `[dependencies]`; gtest is declared explicitly under `[dev-dependencies.compat]`.
The current local suite contains 116 tests across nine binaries. The POSIX-only Release pressure
consumer and its recorded success/failure data are documented in the
[v1 readiness benchmark](docs/benchmarks/2026-08-29-cmp-v1-readiness.md).
Multi-worker correctness and performance data are documented in the
[thread-pool benchmark](docs/benchmarks/2026-08-29-cmp-thread-pool.md).

## Roadmap

Runtime work is split into independently reviewable phases:

1. package identity and importable-module bootstrap — implemented;
2. coroutine task and lifetime semantics — initial `Task` implemented;
3. a root runner and minimal single-thread scheduler — initially implemented;
4. monotonic Timer v1, cancellable ready/timed waits, variadic/vector joins, quiescent TaskGroup,
   OneShotEvent, AsyncManualResetEvent, and AsyncMutex — implemented and pressure-tested;
5. fixed-size multi-worker scheduling — implemented and benchmarked; work stealing remains gated
   by profiling evidence;
6. structured blocking offload — implemented; native asynchronous I/O remains separately gated.

The remaining order is directional, not a promise that a listed feature is already implemented.

## Contributing

Read [the architecture notes](docs/architecture.md) before changing module boundaries. Keep
changes small, use C++23 module conventions, and treat `mcpp build`, `mcpp test`, the standalone
example, and CI as the implementation facts.

## License

[Apache License 2.0](LICENSE)
