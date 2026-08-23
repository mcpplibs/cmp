# CMP

> **Coroutine Machine Process** — a C++23 coroutine runtime project.

[![C++23](https://img.shields.io/badge/C%2B%2B-23-blue.svg)](https://en.cppreference.com/w/cpp/23)
[![Module](https://img.shields.io/badge/module-mcpplibs.cmp-green.svg)](https://en.cppreference.com/w/cpp/language/modules)
[![License](https://img.shields.io/badge/license-Apache_2.0-blue.svg)](LICENSE)

**English** · [简体中文](README.zh.md) · [繁體中文](README.zh.hant.md)

[mcpp](https://github.com/mcpp-community/mcpp) · [Architecture](docs/architecture.md) ·
[Issues](https://github.com/mcpplibs/cmp/issues)

[![ci-linux](https://github.com/mcpplibs/cmp/actions/workflows/ci-linux.yml/badge.svg?branch=main)](https://github.com/mcpplibs/cmp/actions/workflows/ci-linux.yml)
[![ci-macos](https://github.com/mcpplibs/cmp/actions/workflows/ci-macos.yml/badge.svg?branch=main)](https://github.com/mcpplibs/cmp/actions/workflows/ci-macos.yml)
[![ci-windows](https://github.com/mcpplibs/cmp/actions/workflows/ci-windows.yml/badge.svg?branch=main)](https://github.com/mcpplibs/cmp/actions/workflows/ci-windows.yml)

> [!IMPORTANT]
> CMP provides a lazy, single-consumer `Task<T>` / `Task<void>`, structured variadic `when_all()`,
> and a caller-thread `RunLoop` with explicit and monotonic timed scheduling. Timed waits support
> cooperative cancellation with `std::stop_token`; asynchronous I/O and detached execution are
> not implemented.

CMP is being built as a modern coroutine runtime and library on standard stackless C++
coroutines. Its explicit `co_await` model now covers structured concurrent joins, scheduling,
monotonic timers, and cancellable timed waits and can grow, in small verified steps, toward
asynchronous I/O and safe handling of blocking work.

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
- M:N scheduling, work stealing, general cancellation propagation, or async I/O already exist.

Those capabilities must be designed and verified individually. The expected direction is
explicit async I/O awaiters, a dedicated blocking pool, and cooperative safe points.

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

The example prints `Coroutine result: 42`, joins two timed Tasks and prints
`Concurrent result: 42`, then prints `Coroutine cancelled` from a pre-cancelled timed wait. All
messages come from `Task<void>` coroutines. It proves that an independent mcpp package can import
`mcpplibs.cmp`, compose and join Tasks, and use timed scheduling and cancellation through the
public RunLoop.

## Current API

```cpp
import std;
import mcpplibs.cmp;

using mcpplibs::cmp::Task;
using mcpplibs::cmp::RunLoop;
using mcpplibs::cmp::OperationCancelled;
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
    auto [first, second] = co_await when_all(
        delayed_value(scheduler, 10ms, 20),
        delayed_value(scheduler, 1ms, 22));
    std::println("Concurrent result: {}", first + second);
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
    RunLoop loop {};
    loop.run(print_answer(loop.get_scheduler()));
    loop.run(print_concurrent_results(loop.get_scheduler()));

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
parameter order is rethrown. Named Tasks must be moved into `when_all()`.

A translation unit that defines a coroutine must import `std` so the compiler can see the standard
coroutine protocol types. CMP imports `std` privately and does not re-export the whole standard
library.

`RunLoop::run()` consumes one root Task, executes ready coroutines on the calling thread, returns
its value, and rethrows its exception. `Scheduler::schedule()` always suspends and queues the
continuation. `schedule_after()` waits for a relative `steady_clock` duration, while
`schedule_at()` waits for an absolute steady-clock time point; expiry makes work eligible and
never resumes it inline. Scheduler handles are copyable, but remain tied to their originating
RunLoop. Sequential `run()` calls are supported; nested or concurrent calls are rejected. A
moved-from Task must not be awaited.

The timed overloads accepting `std::stop_token` also always suspend. If cancellation wins before
the deadline, awaiting throws `OperationCancelled`; a late stop request cannot replace an already
queued deadline completion. The stop callback only wakes the RunLoop, so user coroutine code is
still resumed by the thread driving `run()`. Cancellation currently performs an O(n) timer lookup.

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
├── src/run_loop.cppm         # RunLoop and Scheduler partition
├── src/when_all.cppm         # structured concurrent Task join
├── tests/cmp_test.cpp        # Task contract and lifetime tests
├── tests/run_loop_test.cpp   # scheduler, boundary, and threading tests
├── tests/when_all_test.cpp   # join ownership, result, and race tests
├── examples/basic/           # standalone path-dependency consumer
├── docs/architecture.md      # current structure, boundaries, and evolution
└── .github/workflows/        # Linux, macOS, and Windows CI
```

The repository does not ship `mcpp new` templates yet. Purpose-built templates can be added
after CMP has a stable runtime API worth demonstrating.

## Development

The local verification path is:

```bash
mcpp build --cache=off
mcpp test --cache=off
cd examples/basic && mcpp run
```

CI runs the equivalent build, test, and standalone example flow on Linux, macOS, and Windows.
The mcpp version is pinned by `.xlings.json`; contributors should not rely on an unrelated
global mcpp installation.

CMP does not track `mcpp.lock`; `.gitignore` enforces that repository policy. Runtime dependencies
belong in `[dependencies]`; gtest is declared explicitly under `[dev-dependencies.compat]`.

## Roadmap

Runtime work is split into independently reviewable phases:

1. package identity and importable-module bootstrap — implemented;
2. coroutine task and lifetime semantics — initial `Task` implemented;
3. a root runner and minimal single-thread scheduler — initially implemented;
4. monotonic Timer v1, cancellable timed waits, and variadic structured joins — implemented;
   dynamic task scopes, cancellation propagation, and more wake-up paths remain;
5. multi-worker scheduling and work stealing;
6. asynchronous I/O integration and a blocking pool.

The remaining order is directional, not a promise that a listed feature is already implemented.

## Contributing

Read [the architecture notes](docs/architecture.md) before changing module boundaries. Keep
changes small, use C++23 module conventions, and treat `mcpp build`, `mcpp test`, the standalone
example, and CI as the implementation facts.

## License

[Apache License 2.0](LICENSE)
