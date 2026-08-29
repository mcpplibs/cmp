# CMP Phase 6B TCP Client v1 Design

**Date:** 2026-08-29
**Status:** Locally implemented and verified — remote CI pending
**Baseline:** Phase 6A is locally committed as `701aa8b` and passes 116/116 tests

## Purpose

Add the first kernel-backed asynchronous I/O surface to CMP: a small TCP client stream that works
on Linux, macOS, and Windows without occupying one CMP worker per pending socket operation.

The developer-facing path stays explicit and short:

```cpp
IoContext io {};
RunLoop loop {};

auto request(RunLoop::Scheduler caller) -> Task<std::string> {
    auto stream = co_await TcpStream::connect(
        io,
        caller,
        "127.0.0.1",
        8080);

    const std::array requestBytes {
        std::byte { 'p' },
        std::byte { 'i' },
        std::byte { 'n' },
        std::byte { 'g' }
    };
    co_await stream.write_all(caller, requestBytes);

    std::array<std::byte, 256> reply {};
    const auto size = co_await stream.read_some(caller, reply);
    co_return bytes_to_string(std::span { reply }.first(size));
}
```

`IoContext` owns the operating-system completion driver. Every operation accepts an explicit
return Scheduler, so raw I/O completion never leaks the private I/O thread into application code.
`TcpStream` owns the socket while each pending operation borrows its supplied byte buffer.

This phase is deliberately one narrow vertical slice. It proves the backend, lifetime,
cancellation, close, shutdown, and return-affinity contracts before CMP adds more resource
families.

## Scope Decision

Phase 6B v1 includes only:

- outbound TCP connections to numeric IPv4 or IPv6 addresses;
- one move-only connected stream type;
- partial reads, write-all, explicit close, and open-state inspection;
- one private I/O driver thread per explicit `IoContext`;
- per-operation cancellation through `std::stop_token`;
- completion through a caller-selected CMP-compatible Scheduler.

It excludes TCP listening, hostname resolution, TLS, UDP, Unix-domain sockets, file I/O, timeouts,
and socket options. None is needed to prove the first portable async-I/O contract.

TCP is selected before file I/O because the available standalone Asio package exposes portable
socket support on all three CI systems. Its file support is not a matching three-platform surface:
the packaged configuration disables Linux `io_uring` support and macOS has no equivalent Asio file
backend. A single portable networking slice is smaller and more useful than three unrelated file
adapters.

Numeric addresses avoid hiding blocking name resolution inside an API advertised as non-blocking.
DNS can be designed later with its own resolver lifetime, result list, cancellation, and timeout
rules.

## Backend and Dependency

Add one exact runtime dependency:

```text
chriskohlhoff.asio@1.38.1
```

The package is already present in `mcpp-index`, exposes the C++23 module `asio`, uses standalone
Asio with separate compilation, and supports Linux, macOS, and Windows. CMP privately imports it;
the public module never uses an Asio type and never `export import`s `asio`.

This dependency is preferred because:

1. C++23 has no standard networking API.
2. Asio already maps Linux to epoll, macOS to kqueue, and Windows to IOCP while presenting one
   completion contract.
3. Hand-writing and maintaining those three backends would add substantially more race and
   shutdown code without improving CMP's public API.
4. Standalone Asio is narrower than adding Boost and does not require a second general-purpose
   event-loop framework.
5. The exact mcpp package version makes local and CI resolution reproducible.

Updating the dependency is not part of Phase 6B implementation. A version change requires its own
compatibility check instead of silently following the newest upstream release.

## Public API

Add one `mcpplibs.cmp:tcp` partition and export these two types from `mcpplibs.cmp`:

```cpp
namespace mcpplibs::cmp {

class IoContext final {
public:
    IoContext();

    IoContext(const IoContext&) = delete;
    IoContext& operator=(const IoContext&) = delete;
    IoContext(IoContext&&) = delete;
    IoContext& operator=(IoContext&&) = delete;

    ~IoContext();
};

class TcpStream final {
public:
    TcpStream() = delete;
    TcpStream(const TcpStream&) = delete;
    TcpStream& operator=(const TcpStream&) = delete;
    TcpStream(TcpStream&&) noexcept;
    TcpStream& operator=(TcpStream&&) = delete;

    ~TcpStream();

    template<typename ReturnScheduler>
    requires (
        std::move_constructible<ReturnScheduler> &&
        requires(const ReturnScheduler& scheduler) {
            scheduler.schedule();
        }
    )
    [[nodiscard]] static Task<TcpStream> connect(
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
    [[nodiscard]] Task<std::size_t> read_some(
        ReturnScheduler returnTo,
        std::span<std::byte> buffer,
        std::stop_token stopToken = {});

    template<typename ReturnScheduler>
    requires (
        std::move_constructible<ReturnScheduler> &&
        requires(const ReturnScheduler& scheduler) {
            scheduler.schedule();
        }
    )
    [[nodiscard]] Task<void> write_all(
        ReturnScheduler returnTo,
        std::span<const std::byte> buffer,
        std::stop_token stopToken = {});

    void close() noexcept;
    [[nodiscard]] bool is_open() const noexcept;
};

}  // namespace mcpplibs::cmp
```

The API adds no executor base class. `ReturnScheduler` follows the existing `run_blocking()`
pattern and works with `RunLoop::Scheduler`, `ThreadPool::Scheduler`, or another scheduler with the
same `schedule()` awaitable expression.

`IoContext` is immovable because it is the visible lifetime and shutdown anchor for its driver
thread. `TcpStream` is move-constructible so a connected stream can be returned from `Task`, but v1
does not invent move-assignment semantics for replacing a live socket with pending operations.

## Laziness and Argument Ownership

All three asynchronous operations return ordinary lazy CMP Tasks. Calling a function only captures
its inputs; address parsing, context admission, socket initiation, and buffer access begin when the
Task is awaited.

The public templates are non-coroutine wrappers that copy the necessary handles before returning a
private helper coroutine. This prevents lazy coroutine frames from retaining a dangling `this` or
`IoContext&` if the visible object is moved or destroyed before the Task starts.

Argument lifetime rules are:

- `numericAddress` is copied into owned storage before `connect()` returns;
- `ReturnScheduler` and `std::stop_token` are stored by value in the Task;
- the lazy Task holds the private socket state by shared ownership, and an accepted operation keeps
  that state alive through its handler and return hop;
- read and write spans are borrowed, and their underlying storage must remain valid and unmodified
  by the caller until the returned Task completes;
- destroying an unstarted Task performs no I/O and releases its captured values.

The shared private socket state is an implementation lifetime device, not shared public socket
ownership. Only the move-only `TcpStream` is the logical owner.

## I/O Context Model

Each `IoContext` owns exactly one private `asio::io_context`, one work guard, and one driver thread.
Construction starts the driver; destruction performs graceful shutdown and joins it. Applications
normally share one `IoContext` across many streams rather than creating one context per connection.
If context allocation or driver-thread creation fails, construction releases partial state and
propagates the original exception; no background thread escapes a failed constructor.

One driver thread is intentional for v1:

- socket initiation, cancellation, close, and raw completion are serialized;
- Asio documents shared socket objects as unsafe for unsynchronized concurrent access;
- the public return Scheduler keeps application continuations off this thread;
- an adjustable worker count and strands add no value until a benchmark proves this driver is the
  limiting resource.

No user callback, Task parent, or ordinary CMP continuation runs directly from an Asio completion
handler. The handler resumes only the private operation helper. That helper stores the outcome and
then awaits `returnTo.schedule()` before publishing it.

`IoContext` is not a general public executor: it has no `run()`, `post()`, worker-count, native
handle, or Asio access API.

## Connect Contract

`connect()` accepts a numeric address in the syntax understood by Asio's address parser and a
16-bit port. It copies and parses the address, creates the socket, registers its private state with
the context, and starts one asynchronous connect.

On success it returns an open `TcpStream`. On failure the temporary socket is closed and no stream
escapes. Invalid text and operating-system connect errors are reported as `std::system_error` with
the original `std::error_code`; CMP does not create a duplicate networking error enum.

Port zero is passed to the operating system rather than rejected by a CMP-specific rule. Hostnames
such as `localhost` are invalid in v1 because no resolver is invoked.

Asio guarantees that an async-connect handler is not invoked inline, including an immediately
available result. CMP additionally performs the explicit return-Scheduler hop, so success and
failure are observed in the same execution context.

## Read Contract

`read_some()` performs at most one socket read and returns the number of bytes placed in the
borrowed span. A successful read may be shorter than the span; the caller loops when its protocol
requires an exact length.

- An empty span completes successfully with zero without touching the socket.
- For a non-empty span, zero means orderly remote EOF.
- EOF is sticky: later non-empty reads also return zero until the stream is explicitly closed.
- If Asio reports bytes together with EOF or another terminal read error, CMP returns the bytes
  first and retains the terminal condition for the next read. Received data is never discarded.
- A retained EOF produces zero; a retained non-EOF error throws `std::system_error`.
- A non-EOF socket error makes the stream logically closed. When it accompanied bytes, the retained
  error is still delivered once before later operations reject the closed stream.
- Remote EOF closes only the receive direction. The caller may still write until it closes the
  stream or another error makes the socket unusable.

CMP does not retry a failed application-level read and does not add a read-exactly operation in
this phase. Repeated `read_some()` is the sufficient primitive.

## Write Contract

`write_all()` uses Asio's composed asynchronous write operation. It succeeds only after the entire
borrowed span has been consumed by the socket layer. An empty span succeeds without touching the
socket.

After native write initiation, any cancellation or socket error closes the stream and throws. A
local cancellation throws `OperationCancelled`; any other socket error throws
`std::system_error`. A pre-cancelled operation initiates no write and leaves an otherwise open
stream usable. Because a failed initiated write may already have sent some prefix, callers must not
blindly retry it unless their protocol makes that safe.

The method returns `void` because a successful call always transfers `buffer.size_bytes()` and a
failed all-or-error operation cannot make the partial count safe to reuse automatically. A separate
`write_some()` surface is unnecessary for v1.

## Concurrent Operations

A stream permits one pending read and one pending write at the same time, preserving normal TCP
full-duplex use. It rejects a second read or second write with `std::logic_error` rather than
silently interleaving buffers or inventing an ordering queue.

Direction admission is selected exactly once when the lazy Task starts and remains held until that
Task publishes its outcome. A failed admission still attempts the requested return-Scheduler hop
before throwing. This makes overlap deterministic and prevents raw-completion timing from changing
whether a second operation is accepted.

`connect()` cannot overlap with stream operations because no `TcpStream` exists until connection
success. `close()` may race any accepted operation and is governed by the close protocol below.

## Cancellation

Each operation accepts an optional `std::stop_token` and owns one Asio cancellation signal for that
operation.

The protocol is:

1. validate the resource and acquire context/direction admission;
2. if the token is already stopped, initiate no socket operation;
3. otherwise initiate the Asio operation on the I/O thread;
4. install a stop callback that queues cancellation onto that same I/O thread;
5. let the Asio completion handler select and publish the single outcome;
6. remove the callback, release direction admission, and return through `returnTo`.

Initiation occurs before the callback can emit its Asio signal. A stop request racing callback
installation is still observed because `std::stop_callback` invokes a newly registered callback
for an already-stopped token. The callback never emits a signal or resumes a coroutine from the
requesting thread; it only asks the context to serialize cancellation.

Cancellation is cooperative with the operating system. If completion has already won, success or
the socket error remains the result. If cancellation wins:

- connect throws `OperationCancelled` and destroys its temporary socket;
- a read with no bytes throws `OperationCancelled` and leaves the stream usable;
- a read that delivered bytes returns those bytes and leaves the stream usable;
- an initiated `write_all()` throws `OperationCancelled` and closes the stream, even when Asio
  reports that zero bytes were transferred;
- cancellation never resumes the awaiting Task more than once.

The Asio handler is the only resume source after native initiation. Cancellation and close request
the handler; they do not separately resume the operation. This removes the usual duplicate-resume
race.

A pre-stopped token still completes asynchronously through a valid return Scheduler. Resource
lifetime errors take precedence when the context or stream could not accept the operation at all.

## Close Contract

`close()` is thread-safe, idempotent, non-blocking, and non-throwing. It marks the logical stream
closed immediately, rejects later operations, and queues the native socket close onto its
`IoContext` thread. Calling it on a moved-from stream is a no-op.

Destroying an owning `TcpStream` performs the same close request. Moving transfers that ownership,
so destruction of the moved-from handle does nothing and the destination becomes responsible for
closing the stream.

The I/O thread linearizes native close against handlers already ready to run:

- a completion selected before close retains its value or socket error;
- close selected first cancels pending operations, which complete with `OperationCancelled`;
- every accepted operation still completes exactly once through its requested return Scheduler.

`is_open()` is a thread-safe snapshot of logical local state. It becomes false as soon as close is
requested or a fatal operation error closes the stream. Remote EOF alone does not make it false
because the write direction may remain usable. A moved-from stream reports false.

No synchronous close-and-wait or half-close API is added. Structured code awaits its outstanding
operations before discarding their buffers and Scheduler owners.

## Errors and Completion Affinity

Public error mapping is intentionally small:

- `OperationCancelled`: an accepted operation was cancelled by its token, explicit close, or
  context shutdown before a result won;
- `std::system_error`: address parsing or an operating-system socket operation failed;
- `std::logic_error`: moved-from/closed stream use, an expired or closing context, same-direction
  overlap, or another violated lifetime contract.

Success, cancellation, system errors, and logic errors are all published only after
`returnTo.schedule()` succeeds. If native initiation cannot be accepted, the helper performs that
return hop from its current thread before reporting the failure.

If the return Scheduler is expired, closed, or inactive, its scheduling exception propagates from
the current completion thread. CMP cannot provide affinity after the selected completion context
has ceased accepting work. The caller must keep the return Scheduler's owner active until the Task
finishes.

CMP does not translate transient codes into automatic retries. TCP connect, read, and write retry
policy belongs to the application protocol; retrying a partially completed write can duplicate
data.

## Context Shutdown

`IoContext` destruction is graceful. It waits for accepted native handlers to be delivered, but
not for user Tasks after they have been handed to another Scheduler:

1. close operation admission under the context's synchronization boundary;
2. ensure all initiation and close requests accepted before that point precede shutdown work;
3. close every registered live socket on the I/O thread;
4. let every accepted Asio operation run its completion handler exactly once;
5. release the persistent Asio work guard;
6. drain queued I/O handlers far enough to enqueue each return-Scheduler handoff;
7. join the driver thread before releasing the public context.

Normal shutdown must not call `asio::io_context::stop()`: Asio documents that `stop()` abandons
unfinished operations and ready handlers. Closing sockets and draining the context preserves CMP's
exactly-once contract.

An operation racing shutdown is either accepted and cancelled/drained or rejected with
`std::logic_error`; it is never silently lost. A `TcpStream` handle may outlive its `IoContext`, but
it is permanently closed and later operations fail safely. Its private shared state does not keep
the public context or driver thread alive.

Destroying `IoContext` from its own driver thread cannot join safely and terminates, matching the
existing `ThreadPool` self-destruction boundary. Public continuations are not run there, so normal
CMP use does not encounter this case.

The context cannot keep an application's return `RunLoop` or `ThreadPool` alive. Structured owners
must preserve the Task, its borrowed buffer, and its return executor until completion. Shutdown can
otherwise surface the return Scheduler's lifetime error as documented above.

## Minimal Implementation Boundary

Implementation should be limited to:

- `mcpp.toml`: add the exact standalone Asio dependency;
- `src/tcp.cppm`: public types and the smallest private context/socket/operation state;
- `src/cmp.cppm`: re-export `:tcp`;
- `tests/tcp_test.cpp`: portable loopback contract and race tests;
- `benchmarks/v1-readiness/src/main.cpp`: replace only the TCP client adapter with `TcpStream`;
- existing README and architecture documents after behavior is verified.

The loopback test server may import Asio directly and use synchronous operations on its own test
thread. CMP does not need a public server API merely to test a client. The existing readiness
benchmark may retain its blocking loopback server and Phase 6A file path; Phase 6B changes only the
client side that it claims to measure.

No generic I/O base class, backend interface, socket factory, polymorphic executor, type-erased
handler, public endpoint type, or detached operation is added. The Asio module already supplies the
portable backend abstraction.

## Validation Contract

The implementation plan must leave deterministic tests for:

- compile-time move/copy traits and operation laziness;
- numeric IPv4 connect, write-all, partial read, orderly EOF, and repeated EOF;
- an IPv6 loopback path when the host reports IPv6 loopback availability, without failing a host
  that has IPv6 disabled;
- invalid numeric address and refused/failed connection as `std::system_error`;
- an empty read and empty write;
- every success and error resuming on the selected `RunLoop` thread;
- returning through a different `ThreadPool::Scheduler`;
- pre-cancellation without native initiation;
- cancellation of a pending connect or read and continued stream usability where specified;
- repeated cancellation-versus-completion races with exactly one outcome;
- one simultaneous read and write succeeding;
- deterministic rejection of a second pending read and second pending write;
- close before use, idempotent close, close during a pending read, and moved-from behavior;
- a server sending bytes and then closing, with all bytes observed before sticky EOF regardless of
  how the platform groups its raw completions;
- context destruction with a pending operation, including exactly-once cancellation and driver
  join;
- many concurrent loopback clients without lost or duplicate completions;
- invalid or inactive return Scheduler behavior.

Tests use loopback addresses, ephemeral ports, latches/barriers, and explicit server protocol
messages. They do not access the public internet, use fixed sleeps as correctness conditions, or
assert platform-specific error numbers. IPv6 is the only capability-gated case; the required IPv4
suite runs on Linux, macOS, and Windows CI.

Dev and Release strict builds and the entire existing test suite must remain green. The readiness
benchmark retains hard success, expected-failure, and unexpected-failure counts. Timing is recorded
for comparison with the Phase 6A thread-blocking adapter but is not a CI threshold.

## Acceptance Criteria

Phase 6B v1 is complete only when:

1. a consumer imports only `mcpplibs.cmp` and uses the API shown above;
2. pending sockets consume the one `IoContext` driver rather than one ThreadPool worker each;
3. no public Asio type or header leaks through the CMP module;
4. buffers and socket state remain valid through every accepted completion path;
5. cancellation, close, and shutdown resume each accepted operation exactly once;
6. user code observes all normal outcomes on its explicit return Scheduler;
7. loopback tests pass under Linux, macOS, and Windows CI;
8. the readiness benchmark reports zero unexpected failures;
9. docs distinguish native async TCP from Phase 6A blocking isolation;
10. excluded features remain excluded unless a failing acceptance case requires them.

## Deliberately Excluded

- async files, directories, pipes, terminals, and process I/O;
- TCP acceptors/servers and generic socket wrappers;
- DNS and service-name resolution;
- TLS, proxy negotiation, HTTP, framing, and serialization;
- UDP, multicast, Unix-domain sockets, and raw sockets;
- connect/read/write timeouts and deadline composition;
- `read_exactly()`, `read_until()`, `write_some()`, scatter/gather, and zero-copy APIs;
- half-close, keepalive, Nagle controls, bind, native handles, and arbitrary socket options;
- multiple I/O driver threads, strands, work stealing, metrics, and runtime tuning;
- implicit current-executor capture, callbacks, futures, or detached operations;
- automatic reconnect, retry, buffering, backpressure, and protocol queues.

Each item can receive a separate design when a concrete consumer needs it. None is required to
validate this phase's portable ownership and completion model.

## References

- [mcpp-index standalone Asio package](https://github.com/mcpplibs/mcpp-index/blob/main/pkgs/c/chriskohlhoff.asio.lua)
- [Standalone Asio upstream](https://github.com/chriskohlhoff/asio)
- [Asio platform implementation notes](https://think-async.com/Asio/asio-1.38.2/doc/asio/overview/implementation.html)
- [`io_context` work and shutdown semantics](https://think-async.com/Asio/asio-1.38.2/doc/asio/reference/io_context.html)
- [Per-operation cancellation](https://think-async.com/Asio/asio-1.38.2/doc/asio/overview/core/cancellation.html)
- [`basic_stream_socket::async_read_some`](https://think-async.com/Asio/asio-1.38.2/doc/asio/reference/basic_stream_socket/async_read_some.html)
- [`async_write`](https://think-async.com/Asio/asio-1.38.2/doc/asio/reference/async_write/overload1.html)
- [`basic_socket::async_connect`](https://think-async.com/Asio/asio-1.38.2/doc/asio/reference/basic_socket/async_connect.html)
- [Asynchronous operation requirements](https://think-async.com/Asio/asio-1.38.2/doc/asio/reference/asynchronous_operations.html)
- [`basic_socket::close`](https://think-async.com/Asio/asio-1.38.2/doc/asio/reference/basic_socket/close.html)
