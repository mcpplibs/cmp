# Architecture

**English** · [简体中文](architecture.zh.md) · [繁體中文](architecture.zh.hant.md)

## Current status

CMP is a C++23 module project with a small coroutine execution core. The root module exports a
lazy, single-consumer `mcpplibs::cmp::Task<T>`, structured variadic `when_all()`, `RunLoop`, and its
copyable `Scheduler` handle. `when_all()` starts and owns multiple Tasks until all complete.
`RunLoop::run()` is the public root execution boundary, while `Scheduler::schedule()` explicitly
returns a suspended coroutine to that loop. `schedule_after()` and `schedule_at()` add relative
and absolute `steady_clock` deadlines without a timer thread; overloads accepting
`std::stop_token` make those waits cooperatively cancellable.

The repository contains:

- one mcpp package manifest;
- the root module `mcpplibs.cmp` with Task, RunLoop, and `when_all` partitions;
- gtest contract, lifetime, exception, scheduling, and threading tests;
- one standalone path-dependency example;
- Linux, macOS, and Windows CI workflows.

## Package and module identity

```toml
[package]
namespace   = "mcpplibs"
name        = "cmp"
version     = "0.1.0"
standard    = "c++23"
description = "A C++23 coroutine runtime project built with mcpp and C++ Modules"
license     = "Apache-2.0"
repo        = "https://github.com/mcpplibs/cmp"
```

The mcpp package identity is the pair `mcpplibs` and `cmp`. A consumer declares `cmp` under
`[dependencies.mcpplibs]` and imports the C++ module `mcpplibs.cmp`. Public C++ declarations use
the namespace `mcpplibs::cmp`.

The root interface is `src/cmp.cppm`, which matches mcpp's default library-root naming rule.
There is no `src/main.cpp`, so mcpp infers a library target named `cmp`; the manifest does not
need a `[lib]` or `[targets.cmp]` override. The C++23 baseline is written explicitly even though
it is also mcpp's default.

`compat.gtest = "1.15.2"` is an explicitly namespaced development dependency used by the test
targets. CMP does not track an `mcpp.lock` file; it is excluded by `.gitignore`.

## Repository layout

```text
.
├── .github/workflows/
│   ├── ci-linux.yml
│   ├── ci-macos.yml
│   └── ci-windows.yml
├── .xlings.json
├── docs/
│   ├── architecture.md
│   ├── architecture.zh.md
│   └── architecture.zh.hant.md
├── examples/basic/
│   ├── mcpp.toml
│   └── src/main.cpp
├── src/
│   ├── cmp.cppm
│   ├── task.cppm
│   ├── run_loop.cppm
│   └── when_all.cppm
├── tests/
│   ├── cmp_test.cpp
│   ├── run_loop_test.cpp
│   └── when_all_test.cpp
└── mcpp.toml
```

## Build and tests

`.xlings.json` pins the mcpp version used by the project. `mcpp build` builds the inferred library
target. `mcpp test` discovers both test files and links a gtest entry point for each. The tests
verify Task ownership and symmetric transfer together with structured joins, root execution,
scheduling, exception propagation, timed and cross-thread wake-up, cancellation races, invalid
scheduler use, loop reuse, and stack-safe repeated completion.

Each CI workflow installs the project tools, builds the library, runs the test suite, and runs
`examples/basic`. The workflows are separate because tool installation and runner details differ
by operating system.

CMP does not currently ship scaffolds for `mcpp new`. The inherited `basic` and `lib` publishing
templates were generic rather than CMP-specific. After those templates were removed,
`tools/template_smoke.sh` had no templates to render or compile and was removed with its CI steps.
The standalone example remains because it tests package consumption directly.

`.gitignore` covers mcpp outputs, the compile database, the project tool environment, compiler and
editor caches, and repository-local project state. These paths are generated or local and are not
part of the package.

## Standalone consumer

`examples/basic` has its own manifest and depends on the repository root:

```toml
[package]
name     = "cmp-basic"
standard = "c++23"

[dependencies.mcpplibs]
cmp = { path = "../.." }
```

Its program uses the same import path as an external package:

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

This example checks path dependency resolution, module consumption, external coroutine
compilation, and the public root runner independently of the root test targets. The RunLoop drives
`print_answer()` on the main thread; a short monotonic timer expires before the coroutine prints
`Coroutine result: 42`. The next root Task concurrently joins two timed values and prints
`Concurrent result: 42`. A final coroutine catches `OperationCancelled` from a pre-cancelled timed
wait and prints `Coroutine cancelled`.

Any translation unit that defines a coroutine imports `std` itself so `std::coroutine_traits` and
the standard coroutine protocol types participate in compilation. The CMP module imports `std`
privately rather than re-exporting the entire standard library.

## Current constraints

`Task<T>` and `Task<void>` have the following contract:

- construction is lazy; the coroutine body starts when the Task is awaited;
- ownership is unique: Task is movable but not copyable or move-assignable;
- `operator co_await()` is rvalue-only and consumes the coroutine handle;
- one value or `std::exception_ptr` is stored in the coroutine frame;
- child completion transfers directly to its continuation, avoiding recursive `resume()` chains;
- an unconsumed Task destroys its frame, and the consuming awaiter destroys a completed frame;
- reference and array result types are rejected.

A moved-from Task is empty and must not be awaited. The current implementation terminates on that
contract violation.

`when_all()` has the following contract:

- the aggregate Task is lazy and owns every input Task;
- awaiting it starts inputs from left to right without waiting for earlier suspended inputs;
- every child reaches a terminal state before the parent resumes;
- results retain parameter order in a `std::tuple`, with `Task<void>` mapped to `std::monostate`;
- move-only results are supported;
- after all children finish, the first child exception in parameter order is rethrown;
- an atomic count-plus-sentinel protocol covers immediate and cross-thread completion exactly once;
- the parent resumes on the thread that completes the last child; no affinity is added implicitly.

`RunLoop` and `Scheduler` have the following contract:

- RunLoop is neither copyable nor movable; its identity anchors every Scheduler it creates;
- `run(Task<T>)` consumes one root Task and runs ready continuations on the calling thread;
- a root value, including a move-only value, is returned; a root exception is rethrown;
- a RunLoop can be reused sequentially, but nested and concurrent `run()` calls throw
  `std::logic_error`;
- `schedule()` always suspends and appends its continuation to a thread-safe FIFO ready queue;
- `schedule_after()` measures a native `steady_clock` duration at suspension, while
  `schedule_at()` accepts an absolute steady-clock time point;
- token-taking timed overloads always suspend; cancellation wins by throwing
  `OperationCancelled`, while a late stop request cannot replace a deadline already queued;
- elapsed deadlines remain asynchronous; future deadlines use a minimum timer heap, and expiry
  only makes the continuation eligible for FIFO dispatch;
- stop callbacks only change timer state and wake the RunLoop; they never resume user coroutine
  code inline, and cancellation currently locates its timer with an O(n) scan;
- producers may enqueue from other threads, but only the thread inside `run()` consumes the queue;
- using a Scheduler after its RunLoop is destroyed, or while its own RunLoop is not active, throws
  `std::logic_error` from the await expression;
- root completion with separately queued work or a pending timer is rejected because detached
  ownership is not part of this phase.

RunLoop does not own a worker thread and supplies no automatic thread affinity. An external
awaiter may resume a Task on another thread; awaiting the original Scheduler explicitly returns
the continuation to its RunLoop. A Task that suspends without arranging another thread or event
source to resume it can leave `run()` blocked indefinitely. Blocking functions still block the
thread on which the coroutine currently executes.

There is no public free-standing `sync_wait`, dynamic TaskGroup, detached execution, standalone
Timer handle, asynchronous I/O backend, custom frame allocator, or blocking-work pool.
Cancellation is explicit on timed waits only; the module provides neither implicit propagation nor
a token overload for plain `schedule()`, and no compatibility alias for the old scaffold module.

Capturing coroutine lambdas require particular care: invoking a temporary capturing lambda can
leave the lazy coroutine referring to a destroyed closure. CMP does not yet provide a helper that
extends that closure's lifetime.

The `C` in CMP echoes the naming role of Go runtime's `G`; it does not imply equivalent semantics.
Standard C++ coroutines provide suspension and resumption mechanics, but they do not supply a
scheduler and do not make a blocking operation asynchronous.

## Possible extensions

The following areas may be considered in separate designs. They are not part of the current
package contract:

1. dynamic structured task scopes and cancellation propagation;
2. additional structured wake-up paths;
3. multi-worker scheduling and work stealing;
4. asynchronous I/O integrations;
5. a dedicated pool for unavoidable blocking work;
6. result adapters and optional coroutine-frame allocation strategies.

Task, RunLoop, and `when_all` occupy separate module partitions because they are implemented public
boundaries. Further partitions or implementation units are added only when another implemented
API needs them.

## Verification

Run from the repository root:

```text
mcpp build --cache=off
mcpp test --cache=off
cd examples/basic
mcpp run
```

The expected result is a successful library build, 46 passing tests across three binaries, and an
example that prints `Coroutine result: 42`, `Concurrent result: 42`, then `Coroutine cancelled`
and exits with status 0. Tests perform one million immediate Task completions, 100,000 immediate
two-Task joins, 100,000 explicit schedules, 100,000 immediate timers, and 100,000 pre-cancelled
timed waits. These check that symmetric transfer and all joined or queued paths do not grow the
native call stack. The current Windows LLVM toolchain does not emit GNU depfiles. If a file included
by a module interface changes, an incremental build can reuse an older BMI or object;
`--cache=off` is used for a full local verification.
