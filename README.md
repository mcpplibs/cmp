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
> CMP now provides its first coroutine primitive: a lazy, single-consumer `Task<T>` / `Task<void>`.
> Scheduling, cancellation, timers, asynchronous I/O, and a public root runner are not implemented.

CMP is being built as a modern coroutine runtime and library on standard stackless C++
coroutines. The intended direction is an explicit `co_await` model that can grow, in small
verified steps, toward scheduling, timers, asynchronous I/O, cancellation, and safe handling of
blocking work.

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
- M:N scheduling, work stealing, timers, cancellation, or async I/O already exist.

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

The example prints `Coroutine result: 42` from inside a `Task<void>` coroutine and exits
successfully. It proves that an independent mcpp package can resolve the path dependency, import
`mcpplibs.cmp`, compose Tasks, and execute the synchronous chain.

## Current Task API

```cpp
import std;
import mcpplibs.cmp;

using mcpplibs::cmp::Task;

Task<int> answer() {
    co_return 42;
}

Task<void> print_answer() {
    auto value = co_await answer();
    std::println("Coroutine result: {}", value);
    co_return;
}
```

`Task` is lazy: calling `answer()` creates a suspended coroutine. It starts when consumed by
`co_await`. A Task is move-only, has one consumer, and can only be awaited as an rvalue. It stores
either a value or an exception, transfers directly between child and continuation, and destroys
an unconsumed frame through RAII. `Task<T&>`, copying, move assignment, and detached execution are
deliberately unsupported.

A translation unit that defines a coroutine must import `std` so the compiler can see the standard
coroutine protocol types. CMP imports `std` privately and does not re-export the whole standard
library.

CMP does not yet provide `sync_wait` or a scheduler, so the current API is a composition primitive
rather than a complete application entry point. The standalone example therefore defines a small,
private root coroutine that is suitable only for its synchronously completing chain. A moved-from
Task must not be awaited.

## Repository Layout

```text
.
├── .xlings.json              # pinned project tool environment
├── mcpp.toml                 # package identity and test dependency
├── src/cmp.cppm              # root module interface
├── tests/cmp_test.cpp        # Task contract and lifetime tests
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
belong in `[dependencies]`; test-only dependencies belong in `[dev-dependencies]`.

## Roadmap

Runtime work is split into independently reviewable phases:

1. package identity and importable-module bootstrap — implemented;
2. coroutine task and lifetime semantics — initial `Task` implemented;
3. a minimal single-thread scheduler;
4. timers, cancellation, and structured wake-up paths;
5. multi-worker scheduling and work stealing;
6. asynchronous I/O integration and a blocking pool.

The remaining order is directional, not a promise that a listed feature is already implemented.

## Contributing

Read [the architecture notes](docs/architecture.md) before changing module boundaries. Keep
changes small, use C++23 module conventions, and treat `mcpp build`, `mcpp test`, the standalone
example, and CI as the implementation facts.

## License

[Apache License 2.0](LICENSE)
