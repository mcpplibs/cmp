# CMP `when_all` Range Design

**Date:** 2026-08-24
**Status:** Implemented; local validation passed
**Base:** `main` at `724050c`
**Branch:** `feature/when-all-range`

## Purpose

Extend structured `when_all()` joins from compile-time parameter packs to runtime-sized,
homogeneous Task collections. Developers can build a vector in a loop, move it into one await,
and receive results in the same order without managing a mutable TaskGroup lifecycle.

## Public API

```cpp
template<typename Result>
[[nodiscard]] auto when_all(std::vector<Task<Result>> tasks)
    -> Task<std::vector<WhenAllValue<Result>>>;
```

As in the variadic overload, `WhenAllValue<T>` means `T` and `WhenAllValue<void>` means
`std::monostate`. A named vector must be moved because it contains uniquely owned Tasks:

```cpp
std::vector<Task<int>> tasks {};
tasks.emplace_back(load(1));
tasks.emplace_back(load(2));

auto values = co_await when_all(std::move(tasks));
```

## Observable Semantics

- The returned aggregate Task is lazy and owns the input vector.
- Awaiting starts every input in ascending index order without waiting for earlier suspended Tasks.
- The parent resumes only after all inputs reach a terminal state.
- Results retain input index order, independent of completion order.
- An empty input returns an empty vector without suspending.
- `Task<void>` produces one `std::monostate` per input.
- Move-only results are supported.
- After all children finish, the first exception by input index is rethrown.
- The parent resumes on the thread that completes the last child; no affinity is implicit.

## Implementation

The existing private `JoinCounter`, `JoinTask<T>`, and `make_join_task()` remain the only completion
machinery. A new range awaiter owns `std::vector<JoinTask<T>>` and uses the same count-plus-sentinel
protocol as the variadic awaiter.

Construction reserves all join-task storage before any child starts. If allocation or wrapper
creation fails, every original Task is still owned by either the input vector or an unstarted join
coroutine and is destroyed through RAII.

`await_resume()` first checks all stored exceptions in index order. Only after that pass succeeds
does it reserve and move results into the output vector. This avoids partially moving successful
child results before discovering a child failure.

The algorithm is O(n), uses one wrapper coroutine frame per child, and performs no lock allocation
or scheduler operation. Cross-thread result publication reuses the existing atomic counter's
release/acquire ordering.

## Test Boundaries

- lazy ownership and destruction of an unawaited input vector;
- empty input;
- start order and result order for scheduled Tasks;
- move-only and void results;
- completion-order-independent exception selection and RunLoop reuse;
- cross-thread completions;
- repeated immediate dynamic joins without native-stack growth;
- unchanged variadic behavior.

## Deliberately Excluded

- heterogeneous runtime collections;
- accepting arbitrary ranges or iterators;
- adding Tasks after the await starts;
- TaskGroup, spawn, detached execution, or recursive admission;
- implicit cancellation propagation, fail-fast destruction, `when_any`, or race adapters.

`std::vector` is the one runtime collection already needed by developers and the standard library.
General range customization is deferred until a real consumer requires it.

## Reference Direction

- [cppcoro range `when_all()`](https://github.com/lewissbaker/cppcoro/tree/391215262bd40d68ac6534810164131f5f9eb148/include/cppcoro)
  accepts a vector of homogeneous awaitables and preserves input order.
- [libcoro `when_all`](https://github.com/jbaldwin/libcoro#when_all) supports both range and variadic
  forms while retaining per-input outcomes.

## Acceptance Criteria

- Runtime-sized vectors join with the same ownership, ordering, and exception semantics as the
  variadic overload.
- Empty, void, move-only, immediate, scheduled, and cross-thread inputs behave as documented.
- Existing 46 tests remain green and focused range tests pass repeatedly in Dev and Release.
- The standalone consumer, three public documentation languages, and three-platform CI pass before
  merge.
