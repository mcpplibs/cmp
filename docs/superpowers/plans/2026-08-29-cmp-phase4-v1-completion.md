# CMP Phase 4 v1 Completion Plan

**Date:** 2026-08-29
**Design:** `docs/superpowers/specs/2026-08-29-cmp-phase4-v1-completion-design.md`
**Status:** Complete

## 1. Preserve the verified baseline

**Complete.** mcpp 2026.8.11.2 resolved LLVM 22.1.8 and all existing 74 tests passed with cache
disabled before implementation.

## 2. Complete phase 4 contracts

**Complete.** RunLoop now shares cancellation state between ready and timed waits and exposes
`schedule(stop_token)`. TaskGroup admits recursive work until quiescence and provides lazy
`cancel_and_join()`. The root exports the focused cancellation and reusable-event partitions.

## 3. Add contract and failure tests

**Complete.** TaskGroup and RunLoop success, rejection, cancellation, race, lifecycle, and stack
boundaries are covered. The manual-reset-event suite covers reuse, FIFO, publication, cancellation,
thread affinity, 1,000-way races, 50,000 waiters, and 20,000 nested signals. The complete Dev and
Release strict suites contain 90 passing tests across seven binaries.

## 4. Add representative v1 readiness pressure tests

**Complete.** The standalone Release consumer uses no new package dependency and fails its process
on any count mismatch. Five measured runs of compute scheduling, temporary-file adapter work, and
loopback TCP adapter work all passed with zero unexpected failures. Exact data and scope are in
`docs/benchmarks/2026-08-29-cmp-v1-readiness.md`.

## 5. Synchronize consumer facts

**Complete.** The basic consumer, three README variants, three architecture variants, benchmark
report, and HANDOFF describe the implemented API, reproducible validation, I/O-adapter boundary,
and remaining phase-5/6 work. No Git or GitHub operation was performed without separate authority.
