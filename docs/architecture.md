# Architecture

**English** · [简体中文](architecture.zh.md) · [繁體中文](architecture.zh.hant.md)

## Current status

CMP currently consists of an initial C++23 module project. The package can be built and imported,
but the root module has no public declarations and no coroutine runtime has been implemented.

The repository contains:

- one mcpp package manifest;
- the import-only root module `mcpplibs.cmp`;
- one gtest import test;
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
`[dependencies.mcpplibs]` and imports the C++ module `mcpplibs.cmp`. The namespace
`mcpplibs::cmp` is reserved for future public C++ declarations.

The root interface is `src/cmp.cppm`, which matches mcpp's default library-root naming rule.
There is no `src/main.cpp`, so mcpp infers a library target named `cmp`; the manifest does not
need a `[lib]` or `[targets.cmp]` override. The C++23 baseline is written explicitly even though
it is also mcpp's default.

`gtest = "1.15.2"` is a development dependency used by the test target. CMP does not track an
`mcpp.lock` file; it is excluded by `.gitignore`.

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
├── src/cmp.cppm
├── tests/cmp_test.cpp
└── mcpp.toml
```

## Build and tests

`.xlings.json` pins the mcpp version used by the project. `mcpp build` builds the inferred library
target. `mcpp test` discovers `tests/cmp_test.cpp`, links the gtest entry point, and verifies that a
separate translation unit can import `mcpplibs.cmp`.

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
import mcpplibs.cmp;

int main() {
    return 0;
}
```

This example checks path dependency resolution and module consumption independently of the root
test target.

## Current constraints

The module currently provides no `task`, promise type, scheduler, timer, cancellation mechanism,
asynchronous I/O backend, or blocking-work pool. It also provides no compatibility alias for the
old scaffold module.

The `C` in CMP echoes the naming role of Go runtime's `G`; it does not imply equivalent semantics.
Standard C++ coroutines provide suspension and resumption mechanics, but they do not supply a
scheduler and do not make a blocking operation asynchronous.

## Possible extensions

The following areas may be considered in separate designs. They are not part of the current
package contract:

1. coroutine task ownership, completion, and lifetime rules;
2. a single-thread scheduler and explicit scheduling awaiters;
3. timers, wake-up paths, and cancellation;
4. multi-worker scheduling and work stealing;
5. asynchronous I/O integrations;
6. a dedicated pool for unavoidable blocking work.

Module partitions or implementation units can be added when an implemented API needs those
boundaries.

## Verification

Run from the repository root:

```text
mcpp build --cache=off
mcpp test --cache=off
cd examples/basic
mcpp run
```

The expected result is a successful library build, one passing import test, and an example that
exits with status 0. The current Windows LLVM toolchain does not emit GNU depfiles. If a file
included by a module interface changes, an incremental build can reuse an older BMI or object;
`--cache=off` is used for a full local verification.
