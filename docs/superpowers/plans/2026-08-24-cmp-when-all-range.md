# CMP `when_all` Range Implementation Plan

**Date:** 2026-08-24
**Design:** `docs/superpowers/specs/2026-08-24-cmp-when-all-range-design.md`
**Branch:** `feature/when-all-range`
**Status:** Complete; merged via PR #6

## 1. Add failing public-contract tests

**Complete.** The focused target failed without the vector overload, then passed all 15 tests.

- Extend `tests/when_all_test.cpp` through the root module only.
- Cover empty, ordered, move-only, void, exception, cross-thread, ownership, and stress behavior.
- Confirm the focused test fails because no vector overload exists.

## 2. Reuse the existing join protocol

**Complete.** The vector awaiter reuses `JoinTask` and `JoinCounter` without a new public lifecycle.

- Add one private vector-owned awaiter to `src/when_all.cppm`.
- Export `when_all(std::vector<Task<T>>)` without changing the variadic overload.
- Keep every child unstarted until construction has safely acquired all vector storage.

## 3. Update the public consumer and facts

**Complete.** The consumer and all three public documentation languages use the runtime vector API.

- Change the existing concurrent print example to construct a runtime Task vector.
- Synchronize English, Simplified Chinese, and Traditional Chinese README and architecture text.
- Record the current phase in `.agent/HANDOFF.md`.

## 4. Verify and deliver

**Complete.** Dev and Release strict cache-off builds and 53/53 tests pass. The Release `when_all`
suite also passes 50/50 repeated runs, and both cross-thread joins pass 5,000 repeated runs after
fixing the test awaiter's publication race. PR #6 passed Linux, macOS, and Windows CI and was
squash-merged as `5c202c8`.

- Run strict cache-off Dev and Release builds and all tests.
- Repeat the focused Release suite and self-review allocation, lifetime, and memory-order edges.
- Commit, push, create a PR, require all three platform checks, squash merge, and synchronize local
  `main` under the existing user authorization.
