# CMP TaskGroup v1 Implementation Plan

**Date:** 2026-08-24
**Design:** `docs/superpowers/specs/2026-08-24-cmp-task-group-v1-design.md`
**Branch:** `feature/task-group-v1`
**Status:** Implemented; local verification complete, delivery pending

## 1. Add failing public-contract tests

**Complete.** The focused target first failed because `TaskGroup` was absent, then passed all nine
public-contract tests after implementation.

- Add `tests/task_group_test.cpp` and use only the exported root module.
- Cover eager start, empty and single join, scheduled overlap, concurrent admission, exception
  order, explicit cancellation, cross-thread completion, rejected lifecycle operations, and
  immediate stack safety.
- Confirm the focused target fails because `TaskGroup` is not exported.

## 2. Implement the minimal scope

**Complete.** One mutex-protected scope retains void child frames, starts accepted work eagerly,
uses symmetric transfer for the final completion, and exposes only the standard stop channel.

- Add one `src/task_group.cppm` partition and export it from `src/cmp.cppm`.
- Retain void child wrappers in admission order and protect state/count/waiter registration with one
  mutex.
- Start accepted children outside the mutex and resume a join continuation only after releasing it.
- Expose the existing standard stop source/token behavior without implicit Task transformation.

## 3. Update the public consumer and facts

**Complete.** The independent consumer prints a TaskGroup result, and all three README and
architecture languages describe the same API and boundaries.

- Add one coroutine print path to `examples/basic` using eager spawn plus join.
- Synchronize English, Simplified Chinese, and Traditional Chinese README and architecture facts.
- Keep `.agent/HANDOFF.md` current through implementation and delivery.

## 4. Verify and deliver

**In progress.** Strict cache-off Dev and Release builds and 62/62 tests pass. The Release
TaskGroup suite passes 50/50 repeats, cross-thread completion 5,000/5,000, and concurrent admission
1,000/1,000. Commit, PR, three-platform CI, merge, and main synchronization remain.

- Run strict cache-off Dev and Release builds and all tests.
- Repeat focused Release and cross-thread suites and self-review allocation, destruction, lock, and
  publication edges.
- Commit, push, create a PR, require all three platform checks, squash merge, and synchronize local
  `main` under the existing user authorization.
