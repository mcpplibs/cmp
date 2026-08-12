# CMP RunLoop v1 Design

**Date:** 2026-08-12
**Status:** Implemented and verified, pending user commit
**Base:** `feature/task-v1` at `7e24ee0`

## Purpose

Turn `Task<T>` from a composition-only primitive into a usable application root without adding a
thread pool, cancellation model, or detached ownership. Developers should only need to create a
RunLoop, pass its Scheduler where an explicit scheduling point is needed, and call `run()`.

## Public API

```cpp
RunLoop loop {};
auto scheduler = loop.get_scheduler();
auto result = loop.run(operation(scheduler));
```

The root module exports:

- non-copyable, non-movable `RunLoop`;
- copyable `RunLoop::Scheduler` handles with identity comparison;
- `Scheduler::schedule()`, an awaitable that always suspends and enqueues the continuation;
- `RunLoop::run(Task<T>)`, returning `T`, supporting `void` and move-only values, and rethrowing
  the root exception.

There is no free-standing `sync_wait()` in this phase. A RunLoop member keeps the blocking root
boundary and the Scheduler that can make progress explicitly connected.

## Execution Model

RunLoop owns a shared internal state with a mutex, condition variable, and FIFO deque of coroutine
handles. Scheduler contains only a weak reference to that state. This gives invalid lifetime use a
defined exception instead of leaving a dangling RunLoop pointer.

`run()` performs these steps:

1. atomically claims the RunLoop for one active root;
2. creates an internal lazy root coroutine that awaits the supplied Task;
3. enqueues that root and consumes ready handles on the calling thread;
4. waits on the condition variable when no handle is ready;
5. publishes the root value or exception under the same synchronization boundary;
6. releases the RunLoop for sequential reuse and destroys the root frame through RAII.

Task-to-Task completion continues to use the existing symmetric-transfer contract. Explicit
`schedule()` always goes through the queue, including on the RunLoop thread, so it is a real yield
point and does not grow the native stack.

## Threading and Lifetime Boundaries

- One thread at a time may execute `run()` for a RunLoop.
- Other threads may enqueue a suspended continuation through its Scheduler.
- Only the thread executing `run()` consumes the ready queue.
- A root may complete on an external thread; completion publication wakes the RunLoop thread.
- RunLoop supplies no hidden thread affinity. Code explicitly awaits a Scheduler to return to it.
- A Scheduler is valid only while its RunLoop exists and while that RunLoop has an active root.
- Destroying or externally resuming a coroutine after publishing its handle remains the owner's
  responsibility; RunLoop stores scheduling references, not frame ownership.

## Error Policy

The root Task's own exception is rethrown by `run()`. Detectable contract violations throw
`std::logic_error`:

- nested or concurrent `run()`;
- scheduling on a RunLoop that is not active;
- scheduling after the RunLoop was destroyed;
- completing the root while separately queued work remains;
- encountering an empty, completed, or otherwise non-runnable queued handle.

Queue allocation failure propagates normally. RAII resets the active-run flag and drops queued
references before the exception leaves `run()`.

A Task that suspends without arranging a future resume can keep `run()` blocked indefinitely.
This is expected event-loop behavior; without timers, cancellation, or a work registry, CMP cannot
distinguish a valid external wait from a deadlock.

## Module Structure

```text
src/cmp.cppm       root module, re-exports public partitions
src/task.cppm      existing Task implementation
src/run_loop.cppm  RunLoop, Scheduler, root bridge, and queue state
```

The implementation imports `std` privately. Consumers defining coroutine functions still import
`std` themselves for the standard coroutine protocol.

## Deliberately Excluded

- detached tasks, spawn, and public queue submission;
- `finish()`, `poll()`, `run_one()`, or early stop;
- a dedicated background thread or thread pool;
- timers, cancellation, I/O, and blocking-work isolation;
- automatic Scheduler propagation or thread-local current Scheduler;
- custom allocators and lock-free queues.

These require ownership or shutdown semantics that do not exist yet.

## Reference Direction

The separation between a run loop and a cheap Scheduler follows the proven shape used by
[stdexec](https://github.com/NVIDIA/stdexec/blob/main/include/stdexec/__detail/__run_loop.hpp) and
[libunifex](https://github.com/facebookexperimental/libunifex/blob/main/include/unifex/manual_event_loop.hpp).
The root coroutine/result bridge follows the core technique used by
[cppcoro](https://github.com/lewissbaker/cppcoro/blob/master/include/cppcoro/detail/sync_wait_task.hpp),
while driving the loop during the blocking boundary follows
[Folly](https://github.com/facebook/folly/blob/main/folly/coro/BlockingWait.h). CMP keeps only the
parts required by its current Task contract.

## Acceptance Criteria

- Task value, void, move-only result, and exception roots work.
- Scheduling on and back to the calling thread works.
- Cross-thread enqueue and external-thread root completion wake the loop safely.
- Nested, concurrent, expired, wrong-loop, and outstanding-work cases are rejected.
- A RunLoop remains reusable after successful runs and handled failures.
- Repeated scheduling does not grow the stack.
- The standalone consumer prints from inside a scheduled coroutine using only public CMP APIs.
