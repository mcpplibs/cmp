# CMP RunLoop v1 Implementation Plan

**Status:** Complete; merged via PR #2

**Goal:** Add the smallest scheduler-aware root execution API that makes Task convenient to use
while keeping lifetime, synchronization, and failure handling inside CMP.

**Toolchain:** C++23 Modules, mcpp/xlings 2026.8.11.2, LLVM 22.1.8, compat.gtest 1.15.2.

**Git constraint:** Work on `feature/run-loop-v1`; do not stage, commit, or push. The user performs
the commit.

## Tasks

- [x] Clean the inherited project state, ignore `.idea/`, pin mcpp 2026.8.11.2, and make the gtest
  namespace explicit for current mcpp dependency rules.
- [x] Move the unchanged Task implementation into `mcpplibs.cmp:task` and keep the root module as
  the public re-export point.
- [x] Implement `RunLoop`, `RunLoop::Scheduler`, the root coroutine bridge, FIFO ready queue,
  condition-variable wake-up, exception propagation, and RAII run-state recovery.
- [x] Add focused tests for values, void, move-only results, exceptions, scheduling, cross-thread
  wake-up, direct external-thread completion, sequential reuse, nested/concurrent runs, invalid
  Scheduler lifetime, outstanding work, and repeated scheduling.
- [x] Replace the example-private root coroutine with the public RunLoop and print from inside the
  scheduled Task.
- [x] Update English, Simplified Chinese, and Traditional Chinese README/architecture documents
  and repository-local development skills.
- [x] Run strict clean build, all tests, release build, standalone consumer, diff validation, and
  final repository status review.

## Required Verification

```bash
mcpp --version
mcpp build --strict --cache=off
mcpp test --strict --cache=off
mcpp build --release --strict --cache=off
cd examples/basic
mcpp run
git diff --check
```

The RunLoop test binary is also repeated 100 times to exercise concurrency and wake-up paths.
