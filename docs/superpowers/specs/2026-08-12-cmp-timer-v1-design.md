# CMP Timer v1 Design

**Date:** 2026-08-12
**Reviewed:** 2026-08-23
**Status:** Implemented and merged via PR #3
**Base:** `main` at `e729782`
**Branch:** `feature/timer-v1`

## Purpose

Add CMP's first built-in asynchronous event source without adding a worker thread, platform timer
API, cancellation model, or detached ownership. A coroutine should be able to suspend until a
monotonic deadline and resume on the RunLoop by awaiting the Scheduler it already uses.

Timer v1 is the first independently reviewable part of roadmap phase 4. Cancellation remains a
separate design because it adds exactly-once resumption and coroutine-frame lifetime races that a
plain timer does not need.

## Public API

`RunLoop::Scheduler` adds a monotonic clock vocabulary and two awaitable factories:

```cpp
class RunLoop::Scheduler {
public:
    using Clock = std::chrono::steady_clock;
    using Duration = Clock::duration;
    using TimePoint = Clock::time_point;

    [[nodiscard]] auto schedule_after(Duration delay) const noexcept;
    [[nodiscard]] auto schedule_at(TimePoint deadline) const noexcept;
};
```

Typical use remains direct:

```cpp
using namespace std::chrono_literals;

Task<void> delayed_work(RunLoop::Scheduler scheduler) {
    co_await scheduler.schedule_after(10ms);
    // Continues on scheduler's RunLoop thread.
}
```

The names follow the existing `schedule()` API and timed-scheduler vocabulary. The relative API
accepts the Scheduler's native duration rather than adding a generic conversion layer with new
rounding and overflow rules. Standard duration literals used by the configured platforms convert
exactly; callers with another representation can convert explicitly.

Timer v1 does not add `sleep_for()` or `sleep_until()` aliases.

## Await Semantics

- `schedule_after()` measures its delay in `await_suspend()`, not when the awaiter is created.
- `schedule_at()` uses an absolute `std::chrono::steady_clock` time point.
- Both awaiters always suspend. An elapsed deadline enters the ready queue rather than continuing
  inline, preserving Scheduler validation, RunLoop affinity, and stack safety.
- A non-positive relative delay is immediately eligible.
- A positive relative delay that cannot be added safely saturates at `TimePoint::max()`.
- Expiry only makes the continuation eligible. Existing ready work may delay its resumption; no
  exact wake-up-time guarantee is made.
- Resumption happens only through the associated RunLoop and therefore on the thread currently
  executing `run()`.

For a positive delay, overflow is checked before addition:

```cpp
const auto now = Clock::now();
const auto deadline = now > TimePoint::max() - delay
    ? TimePoint::max()
    : now + delay;
```

The non-positive case is handled before this expression. The implementation must not calculate
`now + delay` first and then attempt to detect wraparound.

## Execution Model

The shared `RunLoopState` gains a min-oriented timer heap implemented with
`std::priority_queue`. Each timer entry contains only:

- a steady-clock deadline;
- a non-owning `std::coroutine_handle<>` continuation.

The heap gives O(log n) insertion and removal and O(1) access to the earliest deadline. Equal
deadlines have no public ordering guarantee. The existing FIFO ready queue remains the dispatch
queue, so ordinary `schedule()` keeps its current behavior and never enters the timer heap.

Timer registration reuses the existing mutex and active-run boundary:

1. validate that the continuation is non-empty;
2. lock the Scheduler's weak state and reject a destroyed RunLoop;
3. reject a Scheduler whose own RunLoop has no active `run()`;
4. if the deadline is already eligible, append directly to the FIFO ready queue;
5. otherwise insert it into the timer heap;
6. after releasing the mutex, notify for ready work or when a timer became the new earliest
   deadline.

Directly queueing an elapsed deadline avoids needless heap traffic but does not resume inline.

`drive()` remains the only consumer. Each dispatch pass:

1. checks root completion; completion succeeds only when both queues are empty;
2. moves at most one expired timer to the back of the FIFO ready queue;
3. removes one ready continuation, unlocks, and resumes it;
4. waits normally when both queues are empty, or calls
   `condition_variable::wait_until()` with the earliest timer deadline.

Moving one expired timer per pass bounds time spent under the state mutex. Appending before
selecting the next FIFO item also prevents continuously requeued ready work from starving expired
timers without giving a large timer backlog exclusive priority.

The timed wait is deliberately non-predicate-based. A notification or spurious wake-up returns to
the outer state check, so a timer inserted with an earlier deadline replaces the old wait target.
Using a predicate that only observes completion or ready work would incorrectly continue waiting
for the old deadline after an earlier timer notification.

No timer thread is created. The thread inside `RunLoop::run()` waits for deadlines and resumes
eligible continuations.

## Lifetime and Failure Boundaries

Timer entries are scheduling references, not coroutine-frame owners. The existing contract still
applies: externally destroying or resuming a published coroutine is its owner's responsibility.

RunLoop state follows the existing cleanup rules:

- `begin_run()` rejects abandoned ready or timer work;
- root completion with ready work or pending timers throws the existing outstanding-work error;
- `abort_run()` clears both queues before a failing `run()` destroys its root coroutine;
- successful completion leaves both queues empty and permits sequential reuse.

`schedule_after()` and `schedule_at()` report the same lifetime errors as `schedule()`:

- destroyed RunLoop: `std::logic_error` from the await expression;
- no active run on that Scheduler's RunLoop: `std::logic_error`;
- empty continuation: `std::invalid_argument` at the state boundary.

Registration allocation failure propagates through the awaiting Task. A ready-queue allocation
failure while promoting an expired timer may escape `run()` directly; `RunGuard` must still clear
both queues and restore sequential reuse. No allocator injection or synthetic allocation-failure
test is added in v1.

## Test Boundaries

Focused tests cover:

- relative and absolute deadlines that never resume early;
- zero, negative, equal-to-now, and past deadlines that remain asynchronous;
- the maximum relative duration remaining pending instead of wrapping into immediate work;
- a new earliest timer registered from another thread while the RunLoop is waiting;
- resumption on the RunLoop thread after cross-thread registration;
- several different and equal deadlines, with exactly-once but no equal-deadline ordering claim;
- root completion with a pending timer and reuse after cleanup;
- expired and inactive Scheduler handles for both timed factories;
- repeated immediately eligible timers without native-stack growth.

Tests use the real steady clock; Timer v1 does not add clock injection. Duration-correctness checks
assert only the lower bound. The new-earliest concurrency regression may use one generous upper
completion timeout solely to detect a lost notification; it must retain a finite fallback deadline
and join all threads so a failed assertion neither hangs the test process nor leaks work.

## Module and File Scope

Timer behavior remains in the existing RunLoop partition:

```text
src/run_loop.cppm       timer heap, timed wait, and Scheduler awaiters
tests/run_loop_test.cpp timer behavior and boundary tests
examples/basic/         one short delay before the coroutine print
```

No new public module partition or dependency is added. English, Simplified Chinese, and
Traditional Chinese README and architecture documents are updated only after behavior is
implemented and verified.

## Deliberately Excluded

- cancellation tokens, callbacks, and deadline cancellation;
- periodic timers and automatic recurrence;
- changing a registered deadline;
- detached tasks, spawn, and public work submission;
- a background timer thread or platform-specific timer handles;
- clock injection, virtual time, and a public timer object;
- multi-worker dispatch and work stealing.

`std::priority_queue` cannot erase an arbitrary timer efficiently. That is an intentional v1
ceiling, not a hidden cancellation mechanism. The cancellation design must explicitly choose
lazy invalidation or replace the heap with an erase-capable/intrusive structure before exposing a
public cancellation API.

## Reference Direction

- [libunifex timed_single_thread_context](https://github.com/facebookexperimental/libunifex/blob/effb7527401b32b5a2d82fdf6d1a8e8810cbdb07/source/timed_single_thread_context.cpp)
  demonstrates deferred relative-deadline calculation, waking on a new minimum, and
  non-predicate `wait_until()` rechecks.
- [stdexec timed_thread_scheduler](https://github.com/NVIDIA/stdexec/blob/c2d745a7dd74d84d64c7abc9a98fd6a0572b454c/include/exec/timed_thread_scheduler.hpp)
  uses native scheduler time vocabulary and an erase-capable intrusive timer heap because it also
  supports cancellation. CMP v1 adopts the vocabulary, not the cancellation machinery.
- [cppcoro io_service](https://github.com/lewissbaker/cppcoro/blob/391215262bd40d68ac6534810164131f5f9eb148/lib/io_service.cpp)
  shows heap-based deadline selection and the additional ownership, cancellation, and dedicated
  timer-thread machinery that Timer v1 intentionally omits.
- [Boost.Asio steady_timer](https://github.com/boostorg/asio/blob/a7dc25b4cb6c49a6946d86ea20664f1027203225/include/boost/asio/basic_waitable_timer.hpp)
  keeps immediate asynchronous completion queued rather than invoking a handler inline.

## Acceptance Criteria

- A Task can await relative and absolute monotonic deadlines through Scheduler.
- Cross-thread ready work and a newly inserted earlier timer wake the RunLoop safely.
- Timer continuations resume on the `run()` thread and never before their deadline.
- Immediate and repeated timers remain asynchronous and stack-safe.
- Pending timers participate in the existing outstanding-work and cleanup rules.
- Invalid Scheduler lifetime and active-run cases fail deterministically.
- Existing Task, `schedule()`, and RunLoop v1 behavior remains unchanged.
- Dev and Release verification and the standalone example pass locally; Linux, macOS, and Windows
  CI must pass before merge.
