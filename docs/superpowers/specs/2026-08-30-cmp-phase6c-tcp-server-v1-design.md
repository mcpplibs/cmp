# CMP Phase 6C TCP Server v1 Design

**Date:** 2026-08-30
**Status:** Implemented and verified locally; cross-platform CI pending
**Baseline:** `main` at PR #10 squash commit `9492075`; 140/140 Dev and Release tests

## Purpose

Add the smallest portable TCP server surface that reuses Phase 6B's `IoContext`, `TcpStream`,
native Asio backend, cancellation model, and explicit return-Scheduler contract.

The normal developer path is intentionally short:

```cpp
IoContext io {};
RunLoop loop {};

auto serve_one(RunLoop::Scheduler caller) -> Task<void> {
    auto listener = co_await TcpListener::bind(
        io,
        caller,
        "127.0.0.1",
        8080);
    auto stream = co_await listener.accept(caller);

    std::array<std::byte, 256> request {};
    const auto size = co_await stream.read_some(caller, request);
    co_await stream.write_all(caller, std::span { request }.first(size));
}
```

`TcpListener` owns only the listening socket. Each successful `accept()` returns the existing
move-only `TcpStream`; accepted connections keep working after the listener is closed.

## Scope

Phase 6C v1 includes only:

- numeric IPv4 and IPv6 bind addresses;
- one move-only `TcpListener` on an explicit `IoContext`;
- bind-and-listen as one lazy operation;
- one cancellable asynchronous accept at a time;
- thread-safe, idempotent, non-blocking close;
- open-state inspection and the bound local port;
- explicit completion through a caller-selected Scheduler;
- loopback correctness, race, load, and readiness-benchmark coverage.

It excludes DNS, interface-name resolution, TLS, UDP, Unix-domain sockets, peer/local endpoint
objects, configurable backlog, arbitrary socket options, port sharing, half-close, timeouts,
connection limits, accept queues above the operating-system backlog, and detached client handlers.

The existing Asio dependency already supplies the Linux epoll, macOS kqueue, and Windows IOCP
accept backend. No dependency or toolchain change is needed.

## Public API

Add `TcpListener` to the existing `mcpplibs.cmp:tcp` partition and root module:

```cpp
namespace mcpplibs::cmp {

class TcpListener final {
public:
    TcpListener() = delete;
    TcpListener(const TcpListener&) = delete;
    TcpListener& operator=(const TcpListener&) = delete;
    TcpListener(TcpListener&&) noexcept;
    TcpListener& operator=(TcpListener&&) = delete;

    ~TcpListener();

    template<typename ReturnScheduler>
    requires (
        std::move_constructible<ReturnScheduler> &&
        requires(const ReturnScheduler& scheduler) {
            scheduler.schedule();
        }
    )
    [[nodiscard]] static Task<TcpListener> bind(
        IoContext& context,
        ReturnScheduler returnTo,
        std::string_view numericAddress,
        std::uint16_t port,
        std::stop_token stopToken = {});

    template<typename ReturnScheduler>
    requires (
        std::move_constructible<ReturnScheduler> &&
        requires(const ReturnScheduler& scheduler) {
            scheduler.schedule();
        }
    )
    [[nodiscard]] Task<TcpStream> accept(
        ReturnScheduler returnTo,
        std::stop_token stopToken = {});

    void close() noexcept;
    [[nodiscard]] bool is_open() const noexcept;
    [[nodiscard]] std::uint16_t local_port() const noexcept;
};

}  // namespace mcpplibs::cmp
```

`bind()` performs address parse, native bind, and native listen as one operation. A returned
listener is ready to accept. Port zero is passed to the operating system; `local_port()` exposes
the selected nonzero ephemeral port. It returns the last successfully bound port after close and
zero only for a moved-from listener.

The operating system's maximum listen backlog is used. v1 does not add a backlog parameter or
silently set `reuse_address`; both change deployment policy and need a concrete consumer contract.

## Ownership and Laziness

`TcpListener` is the single public owner of a private shared listener state. Shared ownership is
only an implementation device that keeps an accepted native operation alive.

- `bind()` is lazy; calling it performs no address parse, bind, listen, or driver post.
- The public wrapper copies `numericAddress`, the weak context handle, Scheduler, and stop token
  before returning, so a temporary string is safe.
- `accept()` is lazy and captures the listener state by value rather than retaining `this`.
- Moving a listener transfers close ownership. A moved-from listener is closed, has port zero,
  accepts no operations, and tolerates repeated `close()`.
- Destroying a listener performs the same close request as `close()`.
- An accepted `TcpStream` has independent ownership. Closing or destroying the listener does not
  close streams already selected by successful accept completion.

## Driver and Bind Contract

All native acceptor access is serialized on the `IoContext` driver. Bind uses one private
driver-operation bridge: it posts setup to the driver, records either success or an exception, and
resumes only the private helper coroutine. The helper then awaits `returnTo.schedule()` before
publishing the result. The provided RunLoop and ThreadPool Schedulers move public continuations
off the Asio driver; a custom Scheduler owns its own thread-transfer semantics.

On the driver, bind:

1. accepts the context post;
2. lets a pre-requested stop win before parsing or opening;
3. parses only a numeric address;
4. opens an acceptor for that address family;
5. binds the requested port;
6. listens with the native maximum backlog;
7. records the actual local port and registers the listener for context shutdown.

Invalid text, address-in-use, permission, unsupported-family, bind, and listen failures are
reported as `std::system_error` using Asio's original `std::error_code`. Port zero is valid.
Hostnames such as `localhost` are invalid because no resolver is invoked.

Bind cancellation is cooperative only until driver setup starts. A stop request observed before
setup throws `OperationCancelled` and allocates no native listener. Once setup has begun it is a
short synchronous driver action and completion wins; CMP does not add a second cancellation race
inside open/bind/listen.

## Accept Contract

`accept()` creates one candidate `TcpSocketState` on the I/O driver, registers it with the context
before native initiation, and passes its socket to `asio::ip::tcp::acceptor::async_accept()`. Early
registration makes context shutdown own the candidate even when the kernel has selected a
connection but its handler has not yet run. This keeps the existing `TcpStream` implementation and
all of its read, write, EOF, cancellation, and close behavior unchanged.

On success, the candidate socket is marked open only if context shutdown has not already closed
it, wrapped in a `TcpStream`, and published after the explicit return-Scheduler hop. If shutdown
already closed it, accept reports `OperationCancelled`. On failure the candidate is closed and no
stream escapes. A native accept error not caused by local cancellation does not itself close the
listener; the application may decide whether to retry.

A listener permits exactly one accepted-but-not-yet-published `accept()` operation. A second
pending accept throws `std::logic_error` after its own return-Scheduler hop. Admission remains held
through result publication, making overlap independent of driver timing. Applications that need
continuous service write an ordinary loop and start a structured handler for each returned stream.

## Cancellation and Close

`accept(stopToken)` reuses the existing per-operation Asio cancellation signal. Asio 1.38.1
documents terminal, partial, and total per-operation cancellation for `async_accept` on POSIX and
Windows.

- Resource validation and single-accept admission happen before pre-cancellation.
- A pre-stopped token starts no native accept and leaves the listener usable.
- An active stop request cancels only that accept and leaves the listener usable.
- If accept completion already won, the connected stream remains the result.
- Explicit listener close or context shutdown winning first completes the accepted operation once
  with `OperationCancelled`.
- Only the Asio accept handler resumes an initiated accept; stop and close merely request that
  handler, preventing duplicate resume.

`close()` atomically marks the listener closed, rejects later work, and posts native close to its
`IoContext`. It is thread-safe, idempotent, non-blocking, non-throwing, and a no-op on a moved-from
handle. A close request racing driver initiation is checked before `async_accept()` so the already
admitted operation still completes as `OperationCancelled`.

Closing a listener never closes an accepted `TcpStream`. Closing the shared `IoContext` still
closes both listeners and streams because the context owns the native driver lifetime.

## Context Shutdown

`IoContextState` keeps a second linear weak registry for listeners. This avoids introducing a
polymorphic resource hierarchy for two concrete native resource types.

Shutdown remains ordered through the existing admission mutex and driver queue:

1. reject new context posts;
2. close registered listeners and tag pending accepts as context cancellation;
3. close registered streams and tag their pending operations;
4. release the work guard;
5. drain native handlers and enqueue every return-Scheduler hop;
6. join the driver.

An operation racing shutdown is either accepted then completed/cancelled exactly once, or rejected
with `std::logic_error`. A listener may outlive its context, but it is closed and later accepts fail
safely. Its last local port remains inspectable.

## Errors and Completion Affinity

- `OperationCancelled`: accepted bind/accept work lost to its token, listener close, or context
  shutdown as described above;
- `std::system_error`: address parsing or native open/bind/listen/accept failure;
- `std::logic_error`: moved-from/closed listener use, expired/stopping context, overlapping accept,
  or an invalid return Scheduler.

Every bind or accept success and error attempts `returnTo.schedule()` before publication. If that
hop fails, the Scheduler exception is observable. A successfully accepted but unpublished stream
is closed by RAII; a successfully bound but unpublished listener is likewise closed.

CMP does not retry bind or accept errors. Application policy owns retry delay, admission limits,
protocol handling, and shutdown structure.

## Minimal Implementation Boundary

Keep implementation changes to:

- `src/tcp.cppm`: listener state, bind bridge, public `TcpListener`, and context registry extension;
- `tests/tcp_test.cpp`: portable loopback listener contracts and race/load checks;
- `benchmarks/v1-readiness/src/main.cpp`: replace only its blocking TCP server with `TcpListener`;
- the existing README and architecture variants after verification;
- this Design, its Plan, and `.agent/HANDOFF.md`.

No new module partition, dependency, executor abstraction, generic endpoint, resource base class,
callback API, or public native handle is needed.

`examples/basic` stays unchanged. A live socket example would make the basic coroutine sample
longer while tests and the readiness consumer already compile and run the complete public server
path.

## Validation Contract

Leave deterministic tests for:

- exact listener copy/move/noexcept traits;
- bind laziness, expired-context rejection, owned temporary address storage, and an accept Task
  retaining state across a later listener-handle move;
- numeric IPv4 bind, ephemeral port discovery, accept, bidirectional exchange, and RunLoop
  affinity;
- IPv6 loopback when available;
- invalid numeric address and occupied port as `std::system_error` on the return Scheduler;
- pre-cancelled bind skipping invalid-address parsing;
- bind and accept return through a `ThreadPool::Scheduler`, with invalid return Scheduler cleanup;
- pre-cancelled and actively cancelled accept leaving the listener usable;
- deterministic rejection of a second accept both before native completion and while a successful
  stream still waits for return-Scheduler publication;
- close during accept, concurrent idempotent close, move ownership, and moved-from rejection;
- accepted streams surviving listener close;
- stop/close versus accept completion races with exactly one result;
- context destruction draining a pending accept and closing a surviving listener;
- multiple concurrent clients accepted and echoed without lost or duplicate completion;
- the readiness benchmark preserving hard success/failure accounting with a native async server.

Tests use only loopback addresses, port zero, gates/barriers, and bounded deadlock guards. They do
not access the public internet, depend on fixed sleeps, or assert platform-specific error numbers.

## Acceptance Criteria

Phase 6C v1 is locally complete when:

1. a consumer imports only `mcpplibs.cmp` and uses the shown bind/accept path;
2. no public Asio type leaks through CMP;
3. bind and accept outcomes return through the selected Scheduler;
4. listener close, operation cancellation, and context shutdown complete accepted work exactly
   once;
5. accepted stream ownership is independent from listener ownership;
6. strict cache-off Dev and Release builds and all tests pass;
7. `examples/basic` still passes unchanged;
8. the readiness benchmark reports zero unexpected failures in five Release rounds;
9. English, Simplified Chinese, and Traditional Chinese docs state the same boundary.

Linux, macOS, and Windows CI remain the final cross-platform gate and can be recorded only after a
separately authorized push/PR workflow.

## Self-review Corrections

The implementation review tightened four boundaries before coding:

1. Register each candidate socket before `async_accept()`, not after success. Otherwise context
   shutdown could finish its registry scan while a selected connection still waited for handler
   publication.
2. Accept success must use a non-terminating “mark open if still present” path. Context shutdown is
   allowed to close the registered candidate before a queued success handler runs; that outcome is
   cancellation, not an invariant failure.
3. A stop request cancels only the pending accept. It never closes the listener or a connection
   whose successful handler already won.
4. Unrelated native accept errors leave listener policy to the caller instead of silently closing
   a socket that may still accept later connections.

## Local Completion Evidence

- `TcpListener` bind, accept, cancellation, close, move, context shutdown, and accepted-stream
  ownership are implemented in the existing TCP partition without a new dependency or public Asio
  type.
- Bind, connect, and accept recheck an unpublished resource after the return-Scheduler hop;
  context shutdown winning that publication window reports `OperationCancelled`.
- Read, write, and accept operation slots rely only on their pending-admission flags and may
  replace a completed weak operation without waiting for the previous Task frame to be destroyed.
- Stream and listener concurrent-close gates passed 100 Release runs with 800 concurrent close
  calls; each pending read or accept completed exactly once.
- Strict cache-off Dev and Release builds passed; both complete suites passed 161/161 tests across
  ten binaries, including 45 TCP tests. The deterministic publication-window gate also passed 20
  repeated Release runs.
- Five repeated Release race runs covered 100 close/accept and 500 stop/accept races with one
  outcome each. The standalone example passed unchanged.
- Five final Release readiness rounds kept all compute, file, and native async TCP client/server
  hard counts exact with zero unexpected failures; network median was 2,239.147 ms / 8,976.6 ops/s.

Linux, macOS, and Windows CI remain unclaimed until a separately authorized push and PR run.

## Deliberately Excluded

- DNS, service names, interface enumeration, and endpoint result objects;
- TLS, HTTP, framing, serialization, proxying, and authentication;
- UDP, Unix-domain sockets, pipes, terminals, and files;
- configurable backlog, reuse/exclusive-address flags, native handles, and arbitrary options;
- peer address inspection and accepted-socket option inheritance APIs;
- timeouts, rate limits, connection quotas, backpressure queues, and overload policy;
- multiple simultaneous accepts on one listener or multiple I/O driver threads;
- callbacks, futures, detached handlers, automatic TaskGroup ownership, and implicit executors;
- automatic retry, logging, metrics, graceful protocol drain, and process signal integration.

Each excluded capability needs a concrete consumer and a separate contract. None is required to
prove portable listener ownership and accept completion.

## References

- Phase 6B design: `docs/superpowers/specs/2026-08-29-cmp-phase6b-tcp-client-v1-design.md`
- Asio 1.38.1 packaged `basic_socket_acceptor.hpp`, including documented POSIX/Windows
  per-operation cancellation for `async_accept`
- [Standalone Asio `basic_socket_acceptor`](https://think-async.com/Asio/asio-1.38.2/doc/asio/reference/basic_socket_acceptor.html)
- [Standalone Asio per-operation cancellation](https://think-async.com/Asio/asio-1.38.2/doc/asio/overview/core/cancellation.html)
