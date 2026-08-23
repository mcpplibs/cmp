# CMP OneShotEvent v1 Design

**Date:** 2026-08-24
**Status:** Implemented and merged via PR #8
**Base:** `main` at `a9183fc`
**Branch:** `feature/one-shot-event-v1`

## Purpose

Add the smallest built-in bridge from a one-time external event to one or more CMP coroutines.
`OneShotEvent` lets developers suspend with `co_await event` and publish completion with one
thread-safe `event.set()` call. It does not own a thread, scheduler, Task, or result.

## Public API

```cpp
class OneShotEvent final {
public:
    class Awaiter final;

    OneShotEvent() noexcept = default;
    OneShotEvent(const OneShotEvent&) = delete;
    OneShotEvent& operator=(const OneShotEvent&) = delete;
    OneShotEvent(OneShotEvent&&) = delete;
    OneShotEvent& operator=(OneShotEvent&&) = delete;
    ~OneShotEvent();

    [[nodiscard]] bool is_set() const noexcept;
    void set() noexcept;
    [[nodiscard]] Awaiter operator co_await() noexcept;
};
```

Typical structured use keeps the event and all waiting Tasks in one scope:

```cpp
OneShotEvent ready {};
TaskGroup group {};

group.spawn(consume_after(ready));
produce_value();
ready.set();

co_await group.join();
```

## Observable Semantics

- A default event is unset.
- Awaiting an unset event suspends and registers the coroutine without allocating storage.
- The first `set()` atomically makes the event permanently set and resumes every registered
  waiter exactly once before `set()` returns.
- Awaiting an already-set event does not suspend.
- Later `set()` calls are no-ops.
- Concurrent wait registration and `set()` cannot lose a wake-up: a waiter is either registered
  and resumed by the winning setter, or observes the set state and continues inline.
- Writes sequenced before the winning `set()` are visible after a successful await.
- Registered waiters resume on the thread calling `set()`. Waiter order is unspecified. CMP adds
  no implicit Scheduler affinity.

`set()` can therefore execute user coroutine work inline. A waiter that requires RunLoop affinity
must explicitly `co_await scheduler.schedule()` after the event.

## State and Synchronization

One atomic pointer has three monotonic states:

- `nullptr`: unset with no waiters;
- an intrusive singly linked list of awaiter nodes: unset with waiters;
- the event object's address: permanently set.

Each awaiter node lives inside its awaiting coroutine frame and stores only the next pointer and
continuation handle. Registration uses an acquire load plus a release compare-exchange. `set()`
uses an acquire-release exchange, takes ownership of the whole waiter list, and copies each next
pointer and continuation before resuming that waiter. This is the same no-allocation publication
shape used by mature coroutine event primitives.

The list is LIFO only as an implementation detail; public ordering is deliberately unspecified.
With no reset transition, the pointer state cannot return to an earlier generation, avoiding the
ABA and waiter-generation rules required by a reusable manual-reset event.

## Lifetime Contract

`OneShotEvent` is immovable because pending awaiters refer to its stable address and its address is
also the set-state sentinel. Destruction is valid only when the event is unset with no waiters or
after it is set. Destroying an event with registered waiters invokes `std::terminate()`.

The owner must also ensure that `set()` and all resumed waiter work have returned before the event
object's lifetime ends. Destroying an awaiting coroutine frame before the event is set remains an
invalid use; v1 has no cancellation-aware waiter removal. `TaskGroup` is the intended ownership
tool when several Tasks wait on the event.

## Error and Race Boundaries

- Waiting and setting do not allocate and do not throw.
- Exactly one racing setter receives and resumes the waiter list; all others observe the set
  sentinel and return.
- A waiter that races the exchange either appears in the exchanged list or observes the sentinel.
- A resumed waiter may complete immediately; `set()` copies all node fields it needs before each
  resume and never accesses that node afterward.
- Blocking work resumed by `set()` blocks the setter thread. OneShotEvent is a notification
  primitive, not a scheduler or automatic deblocking mechanism.

## Test Boundaries

- initial state, immovability, and set-before-wait;
- suspended waiter and inline setter-thread resumption;
- multiple waiters and idempotent repeated set;
- write publication across threads;
- repeated registration-versus-set races without lost wake-ups;
- explicit return to a RunLoop Scheduler;
- many immediate waiter completions without native-stack growth.

## Deliberately Excluded

- `reset()`, auto-reset semantics, generations, or reusable pulses;
- cancellation tokens or removal of a registered waiter;
- values, exceptions, channels, queues, semaphores, or result futures;
- timeout, `when_any`, race, detached execution, or scheduler selection;
- fairness or FIFO waiter order.

These require different state machines. The one-way event already covers the common callback or
initialization-complete signal without exposing those unrelated policies.

## Reference Direction

- [cppcoro `async_manual_reset_event`](https://github.com/lewissbaker/cppcoro/blob/master/include/cppcoro/async_manual_reset_event.hpp)
  uses an atomic sentinel plus intrusive waiter list, resumes all waiters inside `set()`, and
  requires no waiter to remain at destruction.
- [Folly coroutine `Baton`](https://github.com/facebook/folly/blob/main/folly/coro/Baton.h) uses the
  same publication shape, supports multiple waiters, resumes on the posting thread, and leaves
  cancellation to a larger abstraction.

CMP keeps those proven registration and publication rules but removes reset and optional initial
state from v1.

## Acceptance Criteria

- Developers can directly `co_await` one event from one or more Tasks.
- A set racing registration never loses or duplicates a continuation.
- Cross-thread writes are published and resumption-thread behavior is documented.
- No event wait allocates, blocks a thread, or creates detached work.
- Existing 62 tests, new event tests, standalone example, strict Dev/Release builds, and
  Linux/macOS/Windows CI pass before merge.
