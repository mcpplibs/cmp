# Architecture

**English** · [简体中文](architecture.zh.md) · [繁體中文](architecture.zh.hant.md)

## Current status

CMP is a C++23 module project with a small coroutine execution core. The root module exports a
lazy, single-consumer `mcpplibs::cmp::Task<T>`, `RunLoop`, and its copyable `Scheduler` handle.
`RunLoop::run()` is the public root execution boundary, while `Scheduler::schedule()` explicitly
returns a suspended coroutine to that loop.

The repository contains:

- one mcpp package manifest;
- the root module `mcpplibs.cmp` with Task and RunLoop partitions;
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
│   └── run_loop.cppm
├── tests/
│   ├── cmp_test.cpp
│   └── run_loop_test.cpp
└── mcpp.toml
```

## Build and tests

`.xlings.json` pins the mcpp version used by the project. `mcpp build` builds the inferred library
target. `mcpp test` discovers both test files and links a gtest entry point for each. The tests
verify Task ownership and symmetric transfer together with root execution, scheduling, exception
propagation, cross-thread wake-up, invalid scheduler use, loop reuse, and stack-safe repeated
scheduling.

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

Task<int> answer() {
    co_return 42;
}

Task<void> print_answer(RunLoop::Scheduler scheduler) {
    co_await scheduler.schedule();
    auto value = co_await answer();
    std::println("Coroutine result: {}", value);
    co_return;
}

int main() {
    RunLoop loop {};
    loop.run(print_answer(loop.get_scheduler()));
}
```

This example checks path dependency resolution, module consumption, external coroutine
compilation, and the public root runner independently of the root test targets. The RunLoop drives
`print_answer()` on the main thread; the explicit scheduling point is reached before the coroutine
prints `Coroutine result: 42`.

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

`RunLoop` and `Scheduler` have the following contract:

- RunLoop is neither copyable nor movable; its identity anchors every Scheduler it creates;
- `run(Task<T>)` consumes one root Task and runs ready continuations on the calling thread;
- a root value, including a move-only value, is returned; a root exception is rethrown;
- a RunLoop can be reused sequentially, but nested and concurrent `run()` calls throw
  `std::logic_error`;
- `schedule()` always suspends and appends its continuation to a thread-safe FIFO ready queue;
- producers may enqueue from other threads, but only the thread inside `run()` consumes the queue;
- using a Scheduler after its RunLoop is destroyed, or while its own RunLoop is not active, throws
  `std::logic_error` from the await expression;
- root completion with separately queued work is rejected because detached ownership is not part
  of this phase.

RunLoop does not own a worker thread and supplies no automatic thread affinity. An external
awaiter may resume a Task on another thread; awaiting the original Scheduler explicitly returns
the continuation to its RunLoop. A Task that suspends without arranging another thread or event
source to resume it can leave `run()` blocked indefinitely. Blocking functions still block the
thread on which the coroutine currently executes.

There is no public free-standing `sync_wait`, detached execution, timer, cancellation mechanism,
asynchronous I/O backend, custom frame allocator, or blocking-work pool. The module also provides
no compatibility alias for the old scaffold module.

Capturing coroutine lambdas require particular care: invoking a temporary capturing lambda can
leave the lazy coroutine referring to a destroyed closure. CMP does not yet provide a helper that
extends that closure's lifetime.

The `C` in CMP echoes the naming role of Go runtime's `G`; it does not imply equivalent semantics.
Standard C++ coroutines provide suspension and resumption mechanics, but they do not supply a
scheduler and do not make a blocking operation asynchronous.

## Possible extensions

The following areas may be considered in separate designs. They are not part of the current
package contract:

1. structured task scopes and concurrent joins;
2. timers, wake-up paths, and cancellation;
3. multi-worker scheduling and work stealing;
4. asynchronous I/O integrations;
5. a dedicated pool for unavoidable blocking work;
6. result adapters and optional coroutine-frame allocation strategies.

Task and RunLoop now occupy separate module partitions because they are implemented public
boundaries. Further partitions or implementation units are added only when another implemented API
needs them.

## Verification

Run from the repository root:

```text
mcpp build --cache=off
mcpp test --cache=off
cd examples/basic
mcpp run
```

The expected result is a successful library build, 22 passing tests across two binaries, and an
example that prints `Coroutine result: 42` and exits with status 0. One test performs one million
immediate Task completions; another performs 100,000 explicit scheduling operations. These check
that neither symmetric transfer nor queued scheduling grows the native call stack. The current
Windows LLVM toolchain does not emit GNU depfiles. If a file
included by a module interface changes, an incremental build can reuse an older BMI or object;
`--cache=off` is used for a full local verification.
