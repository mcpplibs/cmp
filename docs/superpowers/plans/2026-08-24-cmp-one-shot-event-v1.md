# CMP OneShotEvent v1 Implementation Plan

**Date:** 2026-08-24
**Design:** `docs/superpowers/specs/2026-08-24-cmp-one-shot-event-v1-design.md`
**Branch:** `feature/one-shot-event-v1`
**Status:** Complete; merged via PR #8

## 1. Add failing public-contract tests

**Complete.** The focused target first failed because `OneShotEvent` was absent, then passed all
seven public-contract tests after implementation.

- Add `tests/one_shot_event_test.cpp` through the exported root module only.
- Cover pre-set, suspended, multiple, cross-thread, racing, Scheduler-return, and stack-safe paths.
- Confirm the focused target fails because `OneShotEvent` is not exported.

## 2. Implement the one-way event

**Complete.** One atomic sentinel/list state registers awaiter-frame nodes without allocation and
publishes every winning set exactly once.

- Add one `src/one_shot_event.cppm` partition and export it from `src/cmp.cppm`.
- Use one atomic sentinel/list state and awaiter-frame nodes; add no allocation or dependency.
- Copy waiter linkage before inline resume and terminate on destruction with pending waiters.

## 3. Update the public consumer and facts

**Complete.** The independent consumer prints one event notification, and all three README and
architecture languages describe the same API and boundaries.

- Add one coroutine event notification to `examples/basic`.
- Synchronize English, Simplified Chinese, and Traditional Chinese README and architecture facts.
- Keep `.agent/HANDOFF.md` current through implementation and delivery.

## 4. Verify and deliver

**Complete.** Strict cache-off Dev and Release builds and 69/69 tests pass. The Release event suite
passes 50/50 repeats, covering 50,000 registration races, and cross-thread resumption passes
5,000/5,000. PR #8 passed Linux, macOS, and Windows CI and was squash-merged as `26ae032`; local
`main` is synchronized.

- Run strict cache-off Dev and Release builds and all tests.
- Repeat focused Release registration/set races and self-review memory ordering and lifetime edges.
- Commit, push, create a PR, require all three platform checks, squash merge, and synchronize local
  `main` under the existing user authorization.
