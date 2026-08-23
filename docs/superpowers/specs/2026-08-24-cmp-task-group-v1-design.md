# CMP TaskGroup v1 Design

**Date:** 2026-08-24
**Status:** Implemented; local verification complete, delivery pending
**Base:** `main` at `4467ab0`
**Branch:** `feature/task-group-v1`

## Purpose

Add the smallest mutable structured-concurrency scope for work that must start before its complete
set is known. `when_all()` remains the preferred one-shot join; `TaskGroup` covers callbacks,
loops, and incremental producers that need eager admission followed by one explicit join.

## Public API

```cpp
class TaskGroup final {
public:
    TaskGroup() = default;
    TaskGroup(const TaskGroup&) = delete;
    TaskGroup& operator=(const TaskGroup&) = delete;
    TaskGroup(TaskGroup&&) = delete;
    TaskGroup& operator=(TaskGroup&&) = delete;
    ~TaskGroup();

    void spawn(Task<void> task);
    [[nodiscard]] Task<void> join();

    [[nodiscard]] std::stop_token get_stop_token() const noexcept;
    bool request_stop() noexcept;
};
```

Only `Task<void>` is admitted. Discarding a result silently is error-prone; callers that need
values should use `when_all()` or store values in state whose lifetime extends through `join()`.
A named Task must be moved into `spawn()`.

```cpp
TaskGroup group {};
auto token = group.get_stop_token();

group.spawn(read_loop(scheduler, token));
group.spawn(write_loop(scheduler, token));

co_await group.join();
```

## Observable Semantics

- `spawn()` accepts ownership and starts the task inline before returning, running until its first
  suspension or completion.
- Successful concurrent `spawn()` calls are serialized into one admission order. Their inline
  execution order across caller threads is unspecified.
- Awaiting `join()` atomically closes admission. It waits for every accepted task, including tasks
  that completed before the join started.
- `join()` is single-use. A later join or spawn is rejected with `std::logic_error`.
- An empty group joins without suspension.
- Child exceptions are retained until all accepted children finish. The first exception in
  admission order is then rethrown.
- The waiting coroutine resumes on the thread that completes the final child. CMP adds no implicit
  affinity; callers can explicitly await a Scheduler.
- `request_stop()` forwards to a group-owned `std::stop_source`. The token is already stopped for
  both current and future tasks after a successful request.
- Cancellation is explicit: `spawn()` does not inject a token into an already-created Task, and a
  child only observes the request if the developer passed `get_stop_token()` to it.

Calling `join()` only creates a lazy CMP Task. Admission closes when that Task is actually awaited,
consistent with every other CMP Task operation.

## Lifetime Contract

`TaskGroup` is immovable and uncopyable because admitted child frames refer to its stable address.
Its states are:

- unused: no task was accepted and join has not started;
- open: at least one task was accepted;
- joining: join has started and at least one child remains;
- joined: join started and every accepted child reached a terminal state.

Destruction is valid only in `unused` or `joined`. Destroying an open or joining group invokes
`std::terminate()` because C++ destructors cannot asynchronously wait and destroying published
child frames would be unsafe. This deliberately turns a lost join into an immediate contract
failure rather than a later use-after-free.

If admission itself throws after earlier tasks were accepted, the caller must request stop if
appropriate and still await `join()` before allowing the group to leave scope.

## Ownership and Synchronization

Each accepted Task is moved into a private, lazy child-wrapper coroutine. The group retains every
wrapper frame in admission order until destruction, which also retains each result exception.
Completed wrappers are not removed individually in v1.

One mutex protects admission state, the active count, wrapper storage, and join-continuation
registration. A wrapper is fully allocated before locking. Admission stores it and increments the
count before starting it outside the lock, so immediate completion cannot deadlock and a racing
join cannot miss the child.

Child completion stores its exception before taking the mutex and decrementing the active count.
The final child moves the group to `joined`, releases the mutex, and returns the registered join
continuation for symmetric transfer. The mutex provides publication for earlier completions; the
final callback does not access the group after selecting the continuation because the parent may
immediately destroy it.

The algorithm is O(n) in retained child frames and exception inspection. `spawn()` may reallocate
the handle vector but never moves coroutine frames; `join()` performs no allocation beyond its own
normal CMP Task frame.

## Error and Race Boundaries

- Child errors never destroy siblings early and never trigger implicit sibling cancellation.
- Allocation failure before admission destroys the candidate Task through RAII without changing
  group state.
- A spawn racing with join either completes admission first and is joined, or observes the closed
  state and throws; it cannot be lost between the count and storage.
- Multiple join attempts have a single winner and all later attempts throw.
- A stop request may race with admission or completion because the standard stop source/token
  operations are thread-safe.
- A child that never arranges a future resume causes join to wait indefinitely, matching Task and
  `when_all()` semantics.

## Test Boundaries

- eager start, lazy join, scheduled overlap, and empty join;
- concurrent admission and deterministic accepted count;
- rejection and RAII cleanup after join starts;
- all-child completion and first exception by admission order;
- explicit cancellation for existing and future children;
- cross-thread result publication and exactly-once join resume;
- second-join rejection and sequential RunLoop reuse;
- repeated immediate admission without native-stack growth;
- compile-time immovability.

## Deliberately Excluded

- `Task<T>` result handles, futures, or per-child result inspection;
- admission after join starts, recursive spawn during shutdown, or reopening;
- a separate token/association type or arbitrary awaitables;
- implicit cancellation injection, automatic cancel-on-error, or `cancel_and_join()`;
- scheduler/executor selection, background workers, detached execution, or frame allocators;
- removing completed wrappers before the group is joined.

These can be added only when a real use case requires their extra lifecycle states. The existing
`request_stop(); co_await join();` sequence already expresses cancellation and cleanup without a
second join primitive.

## Reference Direction

- [P3149R11 `async_scope`](https://www.open-std.org/jtc1/sc22/wg21/docs/papers/2025/p3149r11.html)
  requires tracked work to be joined before scope destruction and separates counting, admission,
  cancellation, and scheduling responsibilities.
- [Folly `AsyncScope`](https://github.com/facebook/folly/blob/main/folly/coro/AsyncScope.h) eagerly
  starts void work, retains a barrier, and requires `joinAsync()` or cleanup before destruction.
- [stdexec `async_scope`](https://github.com/NVIDIA/stdexec/blob/main/include/exec/async_scope.hpp)
  serializes active-count changes with waiter registration and never notifies a waiter while still
  holding the scope lock.

CMP adopts these lifetime rules but exposes only Task-native operations needed by the current
runtime.

## Acceptance Criteria

- Developers can incrementally spawn void Tasks and join them with one `co_await`.
- Every accepted child is owned until completion; no work becomes detached.
- Empty, immediate, scheduled, failing, cancelled, concurrent-admission, and cross-thread children
  obey the documented contract.
- Lost joins and repeated lifecycle operations fail deterministically.
- Existing 53 tests, standalone example, strict Dev/Release builds, and Linux/macOS/Windows CI pass
  before merge.
