# Architecture

**English** · [简体中文](architecture.zh.md) · [繁體中文](architecture.zh.hant.md)

## Current status

CMP is a C++23 module project with a small coroutine execution core. The root module exports a
lazy, single-consumer `mcpplibs::cmp::Task<T>`, structured variadic/vector `when_all()`, eager
`TaskGroup`, allocation-free `OneShotEvent`, reusable `AsyncManualResetEvent`, RAII `AsyncMutex`,
caller-thread `RunLoop`, fixed-size CPU `ThreadPool`, and their copyable Scheduler handles. Join
primitives own every child until completion, while the events publish external signals.
`RunLoop::run()` is the public root execution boundary, while `Scheduler::schedule()` explicitly
returns a suspended coroutine to that loop. `schedule()`, `schedule_after()`, and `schedule_at()`
have `std::stop_token` overloads for cooperative cancellation; timed scheduling uses relative and
absolute `steady_clock` deadlines without a timer thread. `ThreadPool::Scheduler::schedule()`
explicitly transfers a continuation to any fixed worker and has the same cancellation-winner rule.
`run_blocking()` uses a caller-selected ThreadPool instance for synchronous work and publishes the
outcome only after an explicit return Scheduler is reached. Phase 6B/6C add an immovable
`IoContext` with one private driver, a move-only `TcpStream`, and a move-only `TcpListener` for
native async numeric-address TCP clients and servers; all public outcomes still pass through an
explicit caller-selected Scheduler.

The repository contains:

- one mcpp package manifest;
- the root module `mcpplibs.cmp` with Task, cancellation, executor, blocking, TCP, join, event, and
  mutex partitions;
- gtest contract, lifetime, exception, scheduling, and threading tests;
- one standalone path-dependency example;
- local v1-readiness and cross-platform ThreadPool benchmark consumers;
- Linux, macOS, and Windows CI workflows.

## Package and module identity

```toml
[package]
namespace   = "mcpplibs"
name        = "cmp"
version     = "0.1.0"
standard    = "c++23"
description = "A C++23 coroutine runtime project built with mcpp and C++ Modules"
license     = "Apache-2.0"
repo        = "https://github.com/mcpplibs/cmp"
```

The mcpp package identity is the pair `mcpplibs` and `cmp`. A consumer declares `cmp` under
`[dependencies.mcpplibs]` and imports the C++ module `mcpplibs.cmp`. Public C++ declarations use
the namespace `mcpplibs::cmp`.

The root interface is `src/cmp.cppm`, which matches mcpp's default library-root naming rule.
There is no `src/main.cpp`, so mcpp infers a library target named `cmp`; the manifest does not
need a `[lib]` or `[targets.cmp]` override. The C++23 baseline is written explicitly even though
it is also mcpp's default.

`chriskohlhoff.asio = "1.38.1"` is the exact runtime dependency privately used by the TCP partition;
no Asio type appears in CMP's public API. `compat.gtest = "1.15.2"` is an explicitly namespaced
development dependency used by the test targets. CMP does not track an `mcpp.lock` file; it is
excluded by `.gitignore`.

## Repository layout

```text
.
├── .github/workflows/
│   ├── ci-linux.yml
│   ├── ci-macos.yml
│   └── ci-windows.yml
├── .xlings.json
├── docs/
│   ├── architecture.md
│   ├── architecture.zh.md
│   └── architecture.zh.hant.md
├── examples/basic/
│   ├── mcpp.toml
│   └── src/main.cpp
├── src/
│   ├── cmp.cppm
│   ├── task.cppm
│   ├── cancellation.cppm
│   ├── run_loop.cppm
│   ├── thread_pool.cppm
│   ├── blocking.cppm
│   ├── tcp.cppm
│   ├── when_all.cppm
│   ├── task_group.cppm
│   ├── one_shot_event.cppm
│   ├── async_manual_reset_event.cppm
│   └── async_mutex.cppm
├── tests/
│   ├── cmp_test.cpp
│   ├── run_loop_test.cpp
│   ├── thread_pool_test.cpp
│   ├── blocking_test.cpp
│   ├── tcp_test.cpp
│   ├── when_all_test.cpp
│   ├── task_group_test.cpp
│   ├── one_shot_event_test.cpp
│   ├── async_manual_reset_event_test.cpp
│   └── async_mutex_test.cpp
├── benchmarks/v1-readiness/
│   ├── mcpp.toml
│   └── src/main.cpp
├── benchmarks/thread-pool/
│   ├── mcpp.toml
│   └── src/main.cpp
└── mcpp.toml
```

## Build and tests

`.xlings.json` pins the mcpp version used by the project. `mcpp build` builds the inferred library
target. `mcpp test` discovers ten test files and links a gtest entry point for each. The 161 tests
verify Task ownership and symmetric transfer together with structured joins, root execution,
scheduling, exception propagation, timed and cross-thread wake-up, cancellation races, invalid
scheduler use, loop reuse, stack-safe repeated completion, and TCP lifecycle/load behavior.

Each CI workflow installs the project tools, builds the library, runs the test suite, and runs
`examples/basic`. The workflows are separate because tool installation and runner details differ
by operating system.

CMP does not currently ship scaffolds for `mcpp new`. The inherited `basic` and `lib` publishing
templates were generic rather than CMP-specific. After those templates were removed,
`tools/template_smoke.sh` had no templates to render or compile and was removed with its CI steps.
The standalone example remains because it tests package consumption directly.

`.gitignore` covers mcpp outputs, the compile database, the project tool environment, compiler and
editor caches, and repository-local project state. These paths are generated or local and are not
part of the package.

## Standalone consumer

`examples/basic` has its own manifest and depends on the repository root:

```toml
[package]
name     = "cmp-basic"
standard = "c++23"

[dependencies.mcpplibs]
cmp = { path = "../.." }
```

This abridged API sample uses the same import path as an external package:

```cpp
import std;
import mcpplibs.cmp;

using mcpplibs::cmp::Task;
using mcpplibs::cmp::RunLoop;
using mcpplibs::cmp::AsyncMutex;
using mcpplibs::cmp::OperationCancelled;
using mcpplibs::cmp::OneShotEvent;
using mcpplibs::cmp::TaskGroup;
using mcpplibs::cmp::ThreadPool;
using mcpplibs::cmp::run_blocking;
using mcpplibs::cmp::when_all;

using namespace std::chrono_literals;

Task<int> answer() {
    co_return 42;
}

Task<void> print_answer(RunLoop::Scheduler scheduler) {
    co_await scheduler.schedule_after(10ms);
    auto value = co_await answer();
    std::println("Coroutine result: {}", value);
    co_return;
}

Task<int> delayed_value(
    RunLoop::Scheduler scheduler,
    std::chrono::milliseconds delay,
    int value) {
    co_await scheduler.schedule_after(delay);
    co_return value;
}

Task<void> print_concurrent_results(RunLoop::Scheduler scheduler) {
    std::vector<Task<int>> tasks {};
    tasks.emplace_back(delayed_value(scheduler, 10ms, 20));
    tasks.emplace_back(delayed_value(scheduler, 1ms, 22));
    auto values = co_await when_all(std::move(tasks));
    std::println("Concurrent result: {}", values[0] + values[1]);
    co_return;
}

Task<void> print_worker_result(
    ThreadPool::Scheduler workers,
    RunLoop::Scheduler caller) {
    co_await workers.schedule();
    const int result = 21 * 2;
    co_await caller.schedule();
    std::println("Worker pool result: {}", result);
    co_return;
}

Task<void> print_blocking_result(
    ThreadPool::Scheduler blockingWorkers,
    RunLoop::Scheduler caller) {
    const auto result = co_await run_blocking(
        blockingWorkers,
        caller,
        [] {
            std::this_thread::sleep_for(1ms);
            return 42;
        });
    std::println("Blocking result: {}", result);
    co_return;
}

Task<void> add_delayed(
    RunLoop::Scheduler scheduler,
    std::chrono::milliseconds delay,
    int value,
    int& total) {
    co_await scheduler.schedule_after(delay);
    total += value;
    co_return;
}

Task<void> print_task_group(RunLoop::Scheduler scheduler) {
    int total { 0 };
    TaskGroup group {};
    group.spawn(add_delayed(scheduler, 10ms, 20, total));
    group.spawn(add_delayed(scheduler, 1ms, 22, total));
    co_await group.join();
    std::println("Task group result: {}", total);
    co_return;
}

Task<void> set_event(RunLoop::Scheduler scheduler, OneShotEvent& event) {
    co_await scheduler.schedule();
    event.set();
    co_return;
}

Task<void> print_event(RunLoop::Scheduler scheduler) {
    OneShotEvent event {};
    TaskGroup group {};
    group.spawn(set_event(scheduler, event));
    co_await event;
    co_await group.join();
    std::println("Event signalled");
    co_return;
}

Task<void> add_locked(
    RunLoop::Scheduler scheduler,
    AsyncMutex& mutex,
    int value,
    int& total) {
    co_await scheduler.schedule();
    auto guard = co_await mutex.lock_async();
    total += value;
    co_return;
}

Task<void> print_mutex(RunLoop::Scheduler scheduler) {
    int total { 0 };
    AsyncMutex mutex {};
    TaskGroup group {};
    group.spawn(add_locked(scheduler, mutex, 20, total));
    group.spawn(add_locked(scheduler, mutex, 22, total));
    co_await group.join();
    std::println("Mutex result: {}", total);
    co_return;
}

Task<void> print_cancellation(RunLoop::Scheduler scheduler, std::stop_token token) {
    try {
        co_await scheduler.schedule_after(1s, token);
    } catch (const OperationCancelled&) {
        std::println("Coroutine cancelled");
    }
    co_return;
}

int main() {
    ThreadPool workers { 2 };
    ThreadPool blockingWorkers { 2 };
    RunLoop loop {};
    loop.run(print_answer(loop.get_scheduler()));
    loop.run(print_concurrent_results(loop.get_scheduler()));
    loop.run(print_worker_result(
        workers.get_scheduler(),
        loop.get_scheduler()));
    loop.run(print_blocking_result(
        blockingWorkers.get_scheduler(),
        loop.get_scheduler()));
    loop.run(print_task_group(loop.get_scheduler()));
    loop.run(print_event(loop.get_scheduler()));
    loop.run(print_mutex(loop.get_scheduler()));

    std::stop_source source {};
    source.request_stop();
    loop.run(print_cancellation(loop.get_scheduler(), source.get_token()));
}
```

This example checks path dependency resolution, module consumption, external coroutine
compilation, and the public root runner independently of the root test targets. The RunLoop drives
`print_answer()` on the main thread; a short monotonic timer expires before the coroutine prints
`Coroutine result: 42`. The next root Task concurrently joins two timed values and prints
`Concurrent result: 42`. A worker-pool coroutine computes away from the caller, explicitly returns
to the RunLoop, and prints `Worker pool result: 42`. A blocking callable runs on a separate pool,
returns to the RunLoop, and prints `Blocking result: 42`. The next coroutine eagerly spawns and joins
two void Tasks before printing
`Task group result: 42`; a recursively growing group then prints `Recursive group result: 3`.
Other coroutines print `Event signalled`, exercise two reusable-event cycles, and print
`Reusable event cycles: 2`. Two guarded Tasks produce `Mutex result: 42`. A final structured group
uses `cancel_and_join()` and catches `OperationCancelled` from cancellable ready scheduling before
printing `Coroutine cancelled`.

Any translation unit that defines a coroutine imports `std` itself so `std::coroutine_traits` and
the standard coroutine protocol types participate in compilation. The CMP module imports `std`
privately rather than re-exporting the entire standard library.

## Current constraints

`Task<T>` and `Task<void>` have the following contract:

- construction is lazy; the coroutine body starts when the Task is awaited;
- ownership is unique: Task is movable but not copyable or move-assignable;
- `operator co_await()` is rvalue-only and consumes the coroutine handle;
- one value or `std::exception_ptr` is stored in the coroutine frame;
- child completion transfers directly to its continuation, avoiding recursive `resume()` chains;
- an unconsumed Task destroys its frame, and the consuming awaiter destroys a completed frame;
- reference and array result types are rejected.

A moved-from Task is empty and must not be awaited. The current implementation terminates on that
contract violation.

`when_all()` has the following contract:

- the aggregate Task is lazy and owns every input Task;
- awaiting it starts inputs from left to right without waiting for earlier suspended inputs;
- every child reaches a terminal state before the parent resumes;
- results retain parameter order in a `std::tuple`, with `Task<void>` mapped to `std::monostate`;
- a `std::vector<Task<T>>` input produces a same-sized result vector in index order;
- move-only results are supported;
- after all children finish, the first child exception in parameter order is rethrown;
- an atomic count-plus-sentinel protocol covers immediate and cross-thread completion exactly once;
- the parent resumes on the thread that completes the last child; no affinity is added implicitly.

`TaskGroup` has the following contract:

- `spawn(Task<void>)` accepts ownership and starts the child inline before returning;
- concurrent admission is serialized; an active child may recursively admit work while the
  single-use `join()` is waiting;
- admission closes permanently when the active count reaches zero; external admission racing the
  last completion has no guaranteed winner;
- every accepted child reaches a terminal state before join resumes;
- the first exception in admission order is rethrown only after all children finish;
- the group is immovable and must be unused or joined when destroyed; otherwise it terminates;
- `get_stop_token()` and `request_stop()` expose an explicit standard cancellation channel but do
  not inject it into Tasks;
- `cancel_and_join()` lazily requests that channel and then performs the same join;
- the final child resumes join on its completion thread; no scheduler affinity is implicit.

`OneShotEvent` has the following contract:

- a default event is unset and can be awaited by multiple coroutines without allocation;
- the first `set()` permanently sets it and resumes all registered waiters exactly once;
- registration racing set either joins the resumed list or observes the set state;
- writes before set are published to waiters through acquire-release ordering;
- waiters resume inline on the setter thread in unspecified order;
- the event is immovable and must outlive every waiter; pending destruction terminates;
- reset, cancellation-aware removal, values, and implicit Scheduler transfer are not provided.

`AsyncManualResetEvent` has the following contract:

- a default event is unset; `set()` resumes pending waiters in FIFO order and keeps later waits ready;
- `reset()` changes only future waits, while waiters already selected by set still complete;
- `wait(stop_token)` removes one cancelled pending waiter in O(1), and a pre-cancelled wait wins;
- set versus cancellation has exactly one winner and resumes the waiter exactly once;
- waiters resume on the setter or cancellation-request thread, with no implicit Scheduler transfer;
- a thread-local dispatch trampoline keeps nested signal chains stack-safe;
- the event is immovable and must outlive every waiter; pending destruction terminates.

`AsyncMutex` has the following contract:

- `lock_async()` returns an allocation-free operation whose awaited result is a move-only RAII Guard;
- an immediate acquirer continues inline, while contended waiters suspend in FIFO order;
- Guard destruction transfers ownership exactly once and resumes the next waiter on that thread;
- ownership is not thread-affine and the internal state lock is never held during user resumption;
- the mutex must outlive every Guard and waiter; destroying it while owned or queued terminates;
- manual unlock, try-lock, cancellation, recursion, and implicit Scheduler transfer are excluded.

`RunLoop` and `Scheduler` have the following contract:

- RunLoop is neither copyable nor movable; its identity anchors every Scheduler it creates;
- `run(Task<T>)` consumes one root Task and runs ready continuations on the calling thread;
- a root value, including a move-only value, is returned; a root exception is rethrown;
- a RunLoop can be reused sequentially, but nested and concurrent `run()` calls throw
  `std::logic_error`;
- `schedule()` always suspends and appends its continuation to a thread-safe FIFO ready queue; its
  token-taking overload can cancel the queued wait before consumption;
- `schedule_after()` measures a native `steady_clock` duration at suspension, while
  `schedule_at()` accepts an absolute steady-clock time point;
- all token-taking overloads still queue, including pre-cancelled waits; cancellation wins by
  throwing `OperationCancelled`, while a late stop request cannot replace a completion that won;
- elapsed deadlines remain asynchronous; future deadlines use a minimum timer heap, and expiry
  only makes the continuation eligible for FIFO dispatch;
- stop callbacks only change ready/timer state and wake the RunLoop; they never resume user
  coroutine code inline, and cancellation currently locates its queue entry with an O(n) scan;
- producers may enqueue from other threads, but only the thread inside `run()` consumes the queue;
- using a Scheduler after its RunLoop is destroyed, or while its own RunLoop is not active, throws
  `std::logic_error` from the await expression;
- root completion with separately queued work or a pending timer is rejected because detached
  ownership is not part of this phase.

RunLoop does not own a worker thread and supplies no automatic thread affinity. An external
awaiter may resume a Task on another thread; awaiting the original Scheduler explicitly returns
the continuation to its RunLoop. A Task that suspends without arranging another thread or event
source to resume it can leave `run()` blocked indefinitely. Blocking functions still block the
thread on which the coroutine currently executes.

`ThreadPool` and its Scheduler have the following contract:

- construction starts a fixed worker count; the default normalizes unknown hardware concurrency
  to one, while an explicit zero throws `std::invalid_argument`;
- the pool is immovable and its weak, copyable Scheduler identifies that pool without extending its
  lifetime;
- `schedule()` always suspends and enqueues in one shared FIFO; any worker may resume it, and idle
  workers sleep instead of spinning;
- `schedule(stop_token)` queues even when pre-cancelled; worker claim and cancellation select one
  atomic outcome, and user code is resumed only by a worker;
- user continuations are resumed outside the queue mutex, and every accepted entry wakes a worker;
- destruction closes admission, drains accepted entries, joins all workers, and rejects later
  scheduling with `std::logic_error`;
- destroying the pool on one of its own workers is an invalid lifetime arrangement and terminates;
- the pool owns threads, not Tasks, and provides no timer, blocking-I/O adaptation, detached work,
  resize, priority, or affinity API.

`run_blocking(blockingWorkers, returnTo, operation, stopToken)` has the following contract:

- the lazy Task owns a move-constructible callable and invokes it exactly once after a worker claim;
- a separate ThreadPool instance isolates blocking work from latency-sensitive CPU workers;
- value, `void`, exception, or queued cancellation is observed only after `returnTo.schedule()`;
- cancellation can skip queued work, but cannot preempt a synchronous call after it starts;
- the return schedule is intentionally uncancellable so every outcome reaches one executor;
- this is thread-based isolation and does not claim native non-blocking I/O.

`IoContext`, `TcpStream`, and `TcpListener` have the following contract:

- one immovable context owns one private I/O driver and may be shared by many streams;
- a move-only stream connects only to numeric IPv4/IPv6 text; DNS is absent;
- `connect()`, `read_some()`, and `write_all()` are lazy Tasks and publish success or failure only
  after the explicit return Scheduler hop;
- read/write spans borrow their storage until the Task completes; a stream permits one pending read
  and one pending write, while same-direction overlap throws `std::logic_error`;
- remote EOF is sticky for reads but leaves the write direction open; `write_all()` is all-or-error;
- pre-cancellation initiates no I/O and leaves an open stream usable; cancelling an initiated write
  closes the stream;
- `close()` is thread-safe, idempotent, and non-blocking; close and context shutdown cancel accepted
  operations exactly once when they win the completion race;
- a move-only listener lazily binds numeric IPv4/IPv6 text, accepts port zero, and exposes the
  selected port through `local_port()`;
- one accept may be pending; pre/active cancellation leaves the listener usable, while listener
  close or context shutdown cancels that accept exactly once when it wins;
- a successful accept returns an ordinary `TcpStream`; closing the listener does not close streams
  already accepted;
- Phase 6A isolates arbitrary synchronous work on threads, while Phase 6B/6C are native async TCP
  only.

There is no public free-standing `sync_wait`, detached execution, standalone Timer handle,
generic asynchronous-I/O hierarchy, custom frame allocator, or distinct blocking-pool type.
Cancellation remains explicit: Scheduler/event/TCP waits accept tokens, and TaskGroup owns an
optional shared stop channel, but the module provides no implicit propagation and no compatibility
alias for the old scaffold module.

Capturing coroutine lambdas require particular care: invoking a temporary capturing lambda can
leave the lazy coroutine referring to a destroyed closure. CMP does not yet provide a helper that
extends that closure's lifetime.

The `C` in CMP echoes the naming role of Go runtime's `G`; it does not imply equivalent semantics.
Standard C++ coroutines provide suspension and resumption mechanics, but they do not supply a
scheduler and do not make a blocking operation asynchronous.

## Possible extensions

The following areas may be considered in separate designs. They are not part of the current
package contract:

1. TaskGroup result handles and additional cancellation-aware primitives;
2. channels and additional structured wake-up paths;
3. profile-guided work stealing if representative workloads justify it;
4. additional native I/O families such as DNS, TLS, or files;
5. result adapters and optional coroutine-frame allocation strategies.

Task, cancellation, RunLoop, ThreadPool, blocking offload, TCP, `when_all`, TaskGroup, OneShotEvent,
AsyncManualResetEvent, and AsyncMutex occupy separate module partitions because they are
implemented public boundaries.
Further partitions or implementation units are added only when another implemented API needs them.

## Verification

Run from the repository root:

```text
mcpp build --profile dev --strict --cache=off
mcpp test --profile dev --strict --cache=off
mcpp build --profile release --strict --cache=off
mcpp test --profile release --strict --cache=off
cd examples/basic
mcpp run
```

The expected result is a successful library build, 161 passing tests across ten binaries, and an
example that prints `Coroutine result: 42`, `Concurrent result: 42`, `Worker pool result: 42`,
`Blocking result: 42`, `Task group result: 42`, `Recursive group result: 3`, `Event signalled`,
`Reusable event cycles: 2`, `Mutex result: 42`, then
`Coroutine cancelled` and exits with status 0. Tests retain the existing high-volume stack checks
and add 20,000 recursive TaskGroup admissions, 100,000 pre-cancelled ready schedules, 50,000 manual
event waiters, 20,000 nested reusable-event signals, set/cancel races, queued blocking cancellation,
5,000 concurrent blocking offloads, 32-client stream and listener loads, repeated stream
close/stop races, and 100 close/accept plus 500 stop/accept race gates. Focused race suites pass
repeated Release runs. Compute,
temporary-file, and loopback-network counts and
throughput are recorded in the
[v1 readiness benchmark](benchmarks/2026-08-29-cmp-v1-readiness.md). ThreadPool counts, concurrency,
and five-round Release measurements are recorded in the
[ThreadPool benchmark](benchmarks/2026-08-29-cmp-thread-pool.md). The current Windows LLVM toolchain
does not emit GNU depfiles. If a file included by a module interface changes, an incremental build
can reuse an older BMI or object; `--cache=off` is used for a full local verification.
