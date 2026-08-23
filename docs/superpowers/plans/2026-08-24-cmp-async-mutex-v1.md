# CMP AsyncMutex v1 Implementation Plan

**Date:** 2026-08-24
**Design:** `docs/superpowers/specs/2026-08-24-cmp-async-mutex-v1-design.md`
**Branch:** `feature/async-mutex-v1`
**Status:** Complete; merged in PR #9

## 1. Add failing public-contract tests

**Complete.** The focused target first failed because `AsyncMutex` was absent, then passed all five
public-contract tests after implementation.

- Add `tests/async_mutex_test.cpp` through the exported root module only.
- Cover RAII, FIFO, contention, exception, cross-thread ownership, Scheduler return, and stress.
- Confirm the focused target fails because `AsyncMutex` is not exported.

## 2. Implement the minimal RAII mutex

**Complete.** One standard state lock protects an intrusive FIFO, while a no-allocation thread-local
trampoline flattens immediately cascading hand-offs.

- Add one `src/async_mutex.cppm` partition and export it from `src/cmp.cppm`.
- Use one standard state mutex plus an intrusive FIFO of operation-frame nodes.
- Resume a copied continuation after releasing internal state and expose no manual unlock API.

## 3. Update the public consumer and facts

**Complete.** The independent consumer prints one guarded result, and all three README and
architecture languages describe the same API and boundaries.

- Add one coroutine-protected counter result to `examples/basic`.
- Synchronize English, Simplified Chinese, and Traditional Chinese README and architecture facts.
- Keep `.agent/HANDOFF.md` current through implementation and delivery.

## 4. Verify and deliver

**Complete.** Strict cache-off Dev and Release builds and 74/74 tests pass. The Release mutex suite
passes 50/50 repeats and cross-thread ownership passes 5,000/5,000. PR #9 and the merged `main`
commit passed Linux, macOS, and Windows CI before local `main` synchronization.

- Run strict cache-off Dev and Release builds and all tests.
- Repeat focused Release hand-off suites and self-review guard ownership and queue races.
- Commit, push, create a PR, require all three platform checks, squash merge, and synchronize local
  `main` under the existing user authorization.
