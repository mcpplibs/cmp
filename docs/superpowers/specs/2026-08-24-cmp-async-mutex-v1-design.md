# CMP AsyncMutex v1 Design

**Date:** 2026-08-24
**Status:** Implemented; local verification complete, delivery pending
**Base:** `main` at `f8ae3e5`
**Branch:** `feature/async-mutex-v1`

## Purpose

Add a coroutine-aware mutex whose ownership may cross suspension and thread migration. A native
`std::mutex` can block the caller and ties unlock to the locking thread; `AsyncMutex` instead
suspends contending coroutines and transfers ownership with an RAII guard.

## Public API

```cpp
class AsyncMutex final {
public:
    class [[nodiscard]] Guard final;
    class LockOperation final;

    AsyncMutex() = default;
    AsyncMutex(const AsyncMutex&) = delete;
    AsyncMutex& operator=(const AsyncMutex&) = delete;
    AsyncMutex(AsyncMutex&&) = delete;
    AsyncMutex& operator=(AsyncMutex&&) = delete;
    ~AsyncMutex();

    [[nodiscard]] LockOperation lock_async() noexcept;
};
```

Usage deliberately binds the returned guard:

```cpp
auto guard = co_await mutex.lock_async();
update_shared_state();
```

`Guard` is move-constructible, not copyable or assignable, and releases ownership in its
destructor. There is no public manual `unlock()` or `try_lock()` in v1. This keeps every successful
acquisition paired with RAII and avoids accidentally treating `AsyncMutex` as a native BasicLockable.

## Observable Semantics

- Awaiting `lock_async()` acquires immediately when unlocked; otherwise it suspends without
  allocating a waiter node.
- Contending waiters acquire in FIFO registration order.
- Destroying or moving the guard releases ownership exactly once.
- If a waiter exists, release transfers ownership directly and resumes that waiter on the releasing
  thread after the internal state lock is released.
- If no waiter exists, release makes the mutex available to the next acquirer.
- Lock ownership has no thread affinity. A coroutine may suspend, resume on another thread, and
  destroy its guard there.
- CMP adds no Scheduler affinity; a resumed waiter can explicitly await its desired Scheduler.

`lock_async()` itself is lazy only in the ordinary await-expression sense: constructing the
operation changes no state, and acquisition happens in `await_suspend()`.

## Ownership and Synchronization

One private `std::mutex` protects three fields: a locked flag and the head/tail of an intrusive FIFO
waiter queue. Each `LockOperation` is the queue node and lives inside the awaiting coroutine frame,
so contention performs no heap allocation.

Registration takes the state lock. It either marks an unlocked mutex as locked and returns without
suspending, or appends the operation to the FIFO queue. Guard destruction takes the same lock. It
either clears the locked flag or removes the head waiter while keeping the ownership state locked.
The continuation is copied only after releasing the state lock. A no-allocation thread-local
trampoline drains nested hand-offs iteratively, so an immediately releasing waiter cannot grow the
native stack.

The single state lock is intentionally the v1 throughput ceiling. It makes FIFO order, queue
integrity, and destruction checks direct to audit. An atomic dual-list algorithm is justified only
if benchmarks show this short critical section is a bottleneck.

## Lifetime Contract

`AsyncMutex` is immovable because queued operations refer to its stable address. Destruction is
valid only while unlocked with no waiters. Destroying a locked mutex or one with queued waiters
invokes `std::terminate()`.

A suspended waiter frame must remain alive until it acquires the mutex. Cancellation-aware removal
is not part of v1. Structured Task/TaskGroup ownership must therefore outlive every pending lock
operation.

## Error and Race Boundaries

- Lock registration and guard release do not allocate and do not throw.
- The state lock is never held while a user coroutine resumes.
- A new acquirer racing the last release either queues before that release chooses a waiter or sees
  the unlocked state afterward; ownership cannot be lost.
- Guard move construction nulls the source so exactly one destructor releases.
- Recursive acquisition by the same coroutine is unsupported and waits indefinitely like a
  non-recursive mutex.
- Blocking code inside the guarded region still blocks its current thread.

## Test Boundaries

- immediate acquisition, immovability, and guard move ownership;
- RAII release on normal return and exception unwinding;
- a contended call suspends without blocking its caller;
- deterministic FIFO hand-off for several waiters;
- mutual exclusion under cross-thread resumptions;
- ownership released on a different thread;
- explicit Scheduler return after acquisition;
- repeated immediate and contended hand-offs without native-stack growth.

## Deliberately Excluded

- public manual `unlock()`, synchronous `try_lock()`, recursive ownership, or owner identity checks;
- cancellation tokens or removal of a queued waiter;
- timed lock, shared/read locks, condition variables, semaphores, or channels;
- priority scheduling, executor selection, detached ownership, or lock-free queue machinery.

## Reference Direction

- [cppcoro `async_mutex`](https://github.com/lewissbaker/cppcoro/blob/master/include/cppcoro/async_mutex.hpp)
  documents thread-independent ownership, inline hand-off, and RAII scoped acquisition.
- [Folly coroutine `Mutex`](https://github.com/facebook/folly/blob/main/folly/coro/Mutex.h)
  guarantees FIFO acquisition and separates asynchronous locking from scheduler/executor transfer.

CMP keeps those user-visible guarantees but uses one standard state lock and exposes only the RAII
path required by the current runtime.

## Acceptance Criteria

- Developers can protect state across `co_await` with one bound guard.
- Contention suspends coroutines rather than blocking on ownership.
- FIFO hand-off, cross-thread release, and exception safety are deterministic.
- No acquisition allocates or creates detached work.
- Existing 69 tests, new mutex tests, standalone example, strict Dev/Release builds, and
  Linux/macOS/Windows CI pass before merge.
