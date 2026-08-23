# CMP `when_all` v1 Design

**Date:** 2026-08-24
**Status:** Implemented; local and Linux/macOS/Windows CI verified; merged by PR #5
**Base:** `main` at `f3d199c`
**Branch:** `feature/when-all-v1`

## Purpose

Add the first structured-concurrency primitive to CMP: a variadic `when_all()` that starts every
input `Task`, owns every child until completion, and resumes its parent only after all children
finish.

Today, awaiting lazy Tasks one by one is sequential. `when_all()` gives developers one small API
for concurrent composition without exposing detached work, a mutable task group, or scheduler
internals.

## Public API

```cpp
template<typename... Results>
[[nodiscard]] auto when_all(Task<Results>... tasks)
    -> Task<std::tuple<WhenAllValue<Results>...>>;
```

`WhenAllValue<T>` is `T`; `WhenAllValue<void>` is `std::monostate`. The concrete return type is
therefore composed only from standard-library types:

```cpp
auto [number, text] = co_await when_all(load_number(), load_text());
co_await when_all(write_first(), write_second());
```

Tasks are accepted by value. Temporary Tasks are natural to use, while a named Task must be moved,
preserving the existing unique-consumer contract.

The zero-input identity is supported and returns an empty tuple without suspending. A single input
uses the same path as every other arity.

## Observable Semantics

- Calling `when_all()` is lazy because it returns a normal CMP `Task`.
- When that Task is awaited, inputs are started from left to right without waiting for earlier
  inputs to finish.
- Synchronous portions necessarily run on the starting thread. Inputs that suspend can overlap.
- The parent resumes only after every input reaches a terminal state.
- Results keep parameter order, not completion order.
- Move-only results are moved once into the returned tuple.
- `Task<void>` contributes `std::monostate` at its tuple position.
- Child exceptions are retained until all children finish. Then the first exception in parameter
  order is rethrown.
- The parent resumes on the thread that completes the last child, matching CMP's existing lack of
  implicit affinity. A caller can explicitly await a Scheduler to return to its RunLoop.

Waiting for every child before throwing is required for structured lifetime: no child coroutine
frame is destroyed while its continuation may still be published to a timer, scheduler, thread,
or other event source.

## Ownership and Completion State

`when_all()` creates one private join coroutine per input Task. Each join coroutine owns and awaits
one input, storing either its result or its exception in its own frame. The parent awaiter owns all
join coroutines in a tuple until `await_resume()` has collected their results.

A shared counter begins at `child count + 1`. The extra count is a registration sentinel:

1. start every join coroutine;
2. register the awaiting parent and decrement the sentinel;
3. if all children already completed, do not suspend;
4. otherwise the last completing child resumes the parent exactly once.

The counter is atomic because children may complete on different threads. Release/acquire ordering
publishes every child result and exception before the parent reads them. A completion occurring
while `await_suspend()` is still returning is valid; after the final atomic operation,
`await_suspend()` must not access the awaiter again.

Immediate child completion cannot recursively resume the parent while later children are still
being started because the sentinel prevents the counter from reaching zero early.

## Error and Allocation Boundaries

- User exceptions are captured by the corresponding join coroutine.
- All children still complete before one captured exception is rethrown.
- If multiple children fail, parameter order selects the observable exception deterministically.
- Coroutine-frame allocation or result-tuple construction failures propagate through the normal
  `Task` exception path; already-created objects remain RAII-owned.
- `when_all()` adds no dependency, background thread, lock, or scheduler requirement.

Destroying a pending `when_all()` frame after a child has published its continuation remains
invalid for the same reason that externally destroying any published CMP Task frame is invalid.
The structured path is to await `when_all()` to completion.

## Module and File Scope

```text
src/when_all.cppm       private join state and exported variadic function
src/cmp.cppm            export the new partition
tests/when_all_test.cpp public contract, failures, threads, and stack safety
examples/basic/         one concise concurrent print demonstration
README* / architecture current API, boundaries, and roadmap status
```

The implementation uses `import std` and existing `Task`; no manifest or dependency change is
needed.

## Test Boundaries

Focused tests cover:

- laziness and destruction of an unawaited aggregate;
- all inputs starting before a scheduled input completes;
- heterogeneous and move-only results in parameter order;
- `void` mapping and zero-input completion;
- all children finishing before failure propagation;
- deterministic exception selection independent of completion order;
- completions from different threads and result visibility;
- repeated immediate joins without native-stack growth;
- sequential RunLoop reuse after success and failure.

## Deliberately Excluded

- dynamic ranges of Tasks;
- mutable `TaskGroup`, `spawn`, detached execution, or recursive task admission;
- fail-fast destruction of still-running children;
- implicit stop-token propagation or sibling cancellation;
- `when_any`, race, timeout adapters, or result-inspection wrappers;
- executor selection, automatic thread affinity, or multi-worker scheduling.

Dynamic scopes and cancellation propagation need their own lifecycle and close/join contract. This
phase first establishes the smaller completion counter and structured child ownership they can
reuse.

## Reference Direction

- [cppcoro `when_all()` and `when_all_ready()`](https://github.com/lewissbaker/cppcoro/tree/391215262bd40d68ac6534810164131f5f9eb148/include/cppcoro)
  start lazy awaitables in turn, retain each outcome, and use a count-plus-sentinel protocol.
- [libunifex `when_all()`](https://github.com/facebookexperimental/libunifex/blob/effb7527401b32b5a2d82fdf6d1a8e8810cbdb07/doc/api_reference.md#when_allsenders---sender)
  requests sibling cancellation on failure; CMP intentionally postpones that behavior until Tasks
  have an explicit propagation contract.
- [Boost.Cobalt `join` and `gather`](https://www.boost.org/doc/libs/latest/libs/cobalt/doc/html/index.html)
  separate aggregate exception propagation from per-child result inspection. CMP v1 implements
  only the aggregate form.
- [P3149R11 `async_scope`](https://www.open-std.org/jtc1/sc22/wg21/docs/papers/2025/p3149r11.html)
  records production experience that scope, cancellation, and scheduler transfer are distinct
  responsibilities. CMP therefore does not combine them into one first API.

## Acceptance Criteria

- One `co_await when_all(...)` starts all input Tasks and joins them structurally.
- Results and the selected exception follow parameter order.
- Every child finishes before the parent resumes or throws.
- Immediate and cross-thread completion resume the parent exactly once.
- Move-only values, `void`, zero inputs, and existing Task ownership rules work as documented.
- Existing Task, RunLoop, Timer, and cancellation behavior remains unchanged.
- Dev, Release, standalone example, and Linux/macOS/Windows CI pass before merge.
