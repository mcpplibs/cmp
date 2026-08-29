# CMP Cancellation v1 Design

**Date:** 2026-08-23
**Status:** Implemented, Linux/macOS/Windows CI verified, and merged via PR #4
**Base:** `main` at `c06705c`
**Branch:** `feature/cancellation-v1`

## Purpose

Add cooperative cancellation to CMP's timed Scheduler waits while preserving the existing
single-consumer RunLoop model. Developers should use the standard C++ stop source and token,
explicitly pass the token to a wait, and receive ordinary Task exception propagation when
cancellation wins.

Cancellation v1 is the second independently reviewable part of roadmap phase 4. It does not add
detached ownership or full structured concurrency. Its job is narrower: make a suspended timed
wait wake safely, exactly once, and only through its RunLoop.

## Public API

CMP exports one cancellation exception and adds token-taking overloads to the existing timed
Scheduler API:

```cpp
class OperationCancelled final : public std::exception {
public:
    [[nodiscard]] const char* what() const noexcept override;
};

class RunLoop::Scheduler {
public:
    [[nodiscard]] auto schedule_after(
        Duration delay,
        std::stop_token stopToken) const noexcept;

    [[nodiscard]] auto schedule_at(
        TimePoint deadline,
        std::stop_token stopToken) const noexcept;
};
```

Typical use remains explicit and uses only the standard library:

```cpp
using namespace std::chrono_literals;

Task<void> wait_for_work(
    RunLoop::Scheduler scheduler,
    std::stop_token stopToken) {
    co_await scheduler.schedule_after(30s, stopToken);
    // Reached only if the deadline wins.
}
```

When cancellation wins, `co_await` throws `OperationCancelled`. Existing Task exception
propagation carries it through nested Tasks and `RunLoop::run()` unless application code catches
it. CMP does not wrap or alias `std::stop_source` and `std::stop_token`.

The existing one-argument timed overloads stay unchanged and do not create cancellation state or
register callbacks. `schedule()` does not gain a token overload in v1 because it is already
immediately eligible; callers can poll a token before an ordinary scheduling yield.

## Observable Semantics

- Both token-taking awaiters always suspend, including for an already-requested token.
- Scheduler lifetime and active-RunLoop validation remain part of `await_suspend()`.
- An already-requested token wins registration and is queued as cancellation on the RunLoop.
- Otherwise, deadline observation and cancellation race through one RunLoop state transition;
  exactly one wins.
- Once a wait has been placed in the ready queue for deadline completion, a later stop request
  cannot change it to cancellation.
- Cancellation only makes the continuation ready. It never resumes user coroutine code inline on
  the thread calling `std::stop_source::request_stop()`.
- `OperationCancelled` is thrown on the thread driving the associated RunLoop.
- A disengaged or never-requested token behaves like the existing timed overload.
- Cancellation is cooperative. It does not destroy a coroutine frame, stop the RunLoop, interrupt
  blocking code, or roll back work already performed.

Equal-time deadline and cancellation races intentionally have no ordering promise beyond
exactly-once completion. The winner is the first transition from `pending` while holding the
RunLoop state mutex.

## Wait State

Each cancellable timed awaiter owns a small state object inside its coroutine frame:

```text
pending -- deadline observed --> deadline won
pending -- stop callback -----> cancellation won
```

Both terminal states are immutable. `await_resume()` deregisters the stop callback before reading
the terminal state; it returns normally for `deadline won` and throws `OperationCancelled` for
`cancellation won`.

The state contains only:

- an atomic pre-registration stop flag;
- the mutex-protected outcome;
- the `std::stop_callback` registration;
- the existing weak RunLoop state reference and await parameters.

The Timer entry continues to hold a non-owning coroutine handle and, only for the cancellable
overloads, a non-owning pointer to the awaiter's cancellation state. This does not widen CMP's
existing lifetime contract: externally destroying a coroutine frame after publishing its
continuation remains invalid.

## Registration Order

`await_suspend()` performs these steps before publishing the continuation:

1. lock the Scheduler's weak RunLoop state;
2. calculate the relative deadline with the existing saturation rule, when applicable;
3. construct the `std::stop_callback` in the awaiter;
4. register the continuation and cancellation-state pointer with the RunLoop;
5. do not access awaiter members after registration, because another thread may immediately drive
   the coroutine.

A callback registered on an already-requested token can run synchronously in the
`std::stop_callback` constructor. It sets the atomic stop flag before looking for a Timer entry.
If registration has not published that entry yet, the later registration step observes the flag
and queues cancellation directly. If registration won the race, the callback finds the entry
under the RunLoop mutex and transitions it there.

The active-RunLoop check still has final authority. An invalid Scheduler reports its existing
`std::logic_error`; cancellation is not allowed to make an invalid Scheduler appear usable.

## Timer Storage and Wake-up

The current `std::priority_queue` uses a `std::vector` internally but does not expose entries for
safe arbitrary adjustment. Cancellation v1 replaces it with an explicit standard `std::vector`
heap using `std::ranges::push_heap()`, `pop_heap()`, and `make_heap()`.

Normal insertion and expiry remain O(log n). A unified Timer entry gains one nullable cancellation
state pointer, but the non-cancellable path adds no allocation or callback registration.

When a stop callback finds its still-pending Timer entry, it:

1. changes the outcome to `cancellation won` under the existing RunLoop mutex;
2. changes that entry's effective deadline to `TimePoint::min()`;
3. rebuilds the heap with `make_heap()`;
4. releases the mutex and notifies the RunLoop.

The RunLoop then promotes the entry through the existing FIFO ready queue and resumes it outside
the mutex. The callback itself never pushes into `ready_`: `std::deque::push_back()` may allocate,
and an exception escaping a standard stop callback terminates the process. Heap reordering moves
only no-throw Timer entries and performs no allocation.

Cancellation is O(n) because it scans for the operation and rebuilds the heap. This is the
intentional v1 ceiling. It keeps ordinary Timer performance and the implementation small. An
indexed or intrusive erase-capable heap is justified only if cancellation throughput is later
measured as a bottleneck.

## Lifetime and Concurrency Boundaries

`std::stop_callback` deregistration blocks when the callback is concurrently executing on another
thread. Cancellation v1 relies on that guarantee: `await_resume()` resets the registration before
examining or destroying callback-visible state, and it does so without holding the RunLoop mutex.

The callback is `noexcept` and performs only:

- one atomic flag store;
- a weak-pointer lock;
- mutex-protected state transition and no-allocation heap reordering;
- condition-variable notification.

The deadline path and cancellation callback use the same RunLoop mutex, so no extra lock-order or
atomic reference-count protocol is required. Callback execution may briefly wait for that mutex,
but it never waits for user coroutine code.

RunLoop cleanup keeps its existing behavior:

- successful completion requires both queues to be empty;
- a cancelled Timer is promoted and removed before its coroutine resumes;
- root failure clears ready work and the entire Timer heap;
- callback deregistration completes before the owning coroutine frame is destroyed;
- sequential RunLoop reuse remains valid after caught cancellation or root failure.

## Error and Allocation Boundaries

- `OperationCancelled` is the only new public cancellation outcome.
- Destroyed or inactive Scheduler errors remain `std::logic_error`.
- Empty continuation validation remains `std::invalid_argument` at the state boundary.
- Existing ready-queue and Timer-vector allocation failures may propagate from `await_suspend()` or
  `RunLoop::run()` and are cleaned by `RunGuard`.
- The stop callback itself performs no allocation and cannot propagate an exception.
- No allocator injection or synthetic allocation-failure hook is added.

## Test Boundaries

Focused tests must cover:

- an already-requested token that still suspends, resumes on the RunLoop thread, and throws;
- cancellation of a long Timer from another thread, with prompt RunLoop wake-up;
- cancellation requested from the RunLoop thread without deadlock or inline resumption;
- deadline completion followed by a late stop request, which remains successful;
- repeated deadline-versus-cancellation races with exactly one resume and one terminal outcome;
- cancelling a non-earliest Timer without disturbing other different or equal deadlines;
- disengaged and never-requested tokens behaving like ordinary timers;
- invalid and expired Scheduler handles with token-taking overloads;
- cleanup and sequential RunLoop reuse after caught and uncaught `OperationCancelled`;
- callback deregistration before referenced awaiter state is destroyed;
- repeated pre-cancelled waits without native-stack growth.

Concurrency tests use latches or semaphores for ordering and finite fallback deadlines for liveness.
They must join every created thread on both success and failure paths. Timing assertions do not
claim precision beyond the existing lower-bound Timer contract.

## Module and File Scope

Expected implementation scope:

```text
src/run_loop.cppm       exception, token overloads, wait state, vector heap, wake-up path
tests/run_loop_test.cpp cancellation behavior, races, cleanup, and lifetime tests
examples/basic/         one small standard stop_source demonstration after behavior passes
```

The root module already exports the RunLoop partition, so no new public module or dependency is
needed. English, Simplified Chinese, and Traditional Chinese public documents are updated only
after the implementation and tests pass.

## Deliberately Excluded

- custom CancellationToken or CancellationSource wrappers;
- implicit token propagation through Task promises;
- cancellation of plain `schedule()`;
- a public Timer handle, changing deadlines, or manual Timer removal;
- Task groups, async scopes, concurrent joins, spawn, or detached execution;
- periodic timers;
- cancellation of arbitrary external awaiters or blocking functions;
- background threads, platform cancellation APIs, or a new dependency;
- lock-free cancellation queues and custom intrusive heaps.

Structured task ownership can later create and propagate stop sources. Cancellation v1 provides
the safe Scheduler wake-up primitive that such a scope would reuse, without speculating on that
scope's API now.

## Reference Direction

- The C++ working draft specifies synchronous callback invocation for an already-requested token
  and blocking deregistration when a callback is running on another thread:
  [stoppable callback registration](https://eel.is/c++draft/stoptoken.concepts) and
  [`std::stop_callback`](https://eel.is/c++draft/stopcallback).
- [cppcoro cancellation and `io_service::schedule_after`](https://github.com/lewissbaker/cppcoro/tree/391215262bd40d68ac6534810164131f5f9eb148)
  demonstrate an explicit token parameter and exception propagation from a coroutine timer.
- [libunifex `timed_single_thread_context`](https://github.com/facebookexperimental/libunifex/blob/effb7527401b32b5a2d82fdf6d1a8e8810cbdb07/source/timed_single_thread_context.cpp)
  moves a cancelled timed operation to immediate eligibility under the context mutex so completion
  remains scheduler-affine.
- [stdexec `timed_thread_scheduler`](https://github.com/NVIDIA/stdexec/blob/c2d745a7dd74d84d64c7abc9a98fd6a0572b454c/include/exec/timed_thread_scheduler.hpp)
  uses an erase-capable intrusive heap and atomic lifetime coordination. CMP adopts its
  exactly-once requirement but not machinery needed for a multi-threaded sender runtime.
- [Boost.Asio timer cancellation](https://www.boost.org/doc/libs/latest/doc/html/boost_asio/reference/basic_waitable_timer/cancel.html)
  distinguishes cancellation that wins before completion from a timer already queued to complete.

## Acceptance Criteria

- Developers can cancel relative and absolute Scheduler waits with `std::stop_token`.
- Cancellation propagates as `OperationCancelled` through existing Task exception handling.
- Pre-requested, cross-thread, and RunLoop-thread cancellation never resumes inline.
- Deadline and cancellation races resume the continuation exactly once.
- Callback deregistration and RunLoop cleanup prevent callback access after awaiter destruction.
- Cancellation callback execution is `noexcept` and performs no allocation.
- Ordinary timed overload behavior and asymptotic performance remain unchanged.
- Cancellable Timer insertion/expiry remain O(log n); cancellation is explicitly O(n).
- Existing Task, Scheduler, Timer v1, and RunLoop reuse behavior remains intact.
- Dev, Release, standalone example, and Linux/macOS/Windows CI pass before merge.
