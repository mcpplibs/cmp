# CMP Phase 6B TCP Client v1 Implementation Plan

**Date:** 2026-08-30
**Design:** `docs/superpowers/specs/2026-08-29-cmp-phase6b-tcp-client-v1-design.md`
**Status:** In progress — developer documentation synchronized
**Baseline:** Local Phase 6A commit `701aa8b`, 116/116 Dev and Release tests

## Execution Rule

Implement the approved TCP-client slice in the order below. Keep the public API and exclusions in
the Design unchanged. At each gate, run the smallest focused check first and stop on a failure
instead of adding a fallback backend or broadening the API.

The implementation remains one module partition and one test file. Reuse standalone Asio for the
OS backend, CMP `Task` for ownership, existing Scheduler types for return affinity, and
`OperationCancelled` for local cancellation. Do not add a generic I/O hierarchy, public server,
future, callback API, or hidden process-wide executor.

## 1. Prove the packaged backend

Add the exact dependency through mcpp:

```text
mcpp add chriskohlhoff.asio@1.38.1
```

Confirm the manifest records the explicit `chriskohlhoff` namespace and exact `1.38.1` version.
Do not add Asio separately to `examples/basic` or `benchmarks/v1-readiness`; both consume CMP as a
path dependency.

Create `src/tcp.cppm` with:

```cpp
export module mcpplibs.cmp:tcp;

import std;
import asio;
import :cancellation;
import :task;
```

Re-export `:tcp` from `src/cmp.cppm`. Keep `asio` private: no public signature, alias, exported
declaration, or root-module re-export may mention an Asio type.

Run a strict cache-off Dev build immediately. If the packaged module does not compile with the
pinned LLVM/mcpp environment, stop and diagnose that package boundary; do not replace it with
platform headers or three handwritten backends.

**Completion — 2026-08-30:** mcpp 2026.8.11.2 added
`[dependencies.chriskohlhoff] asio = "1.38.1"`; `src/tcp.cppm` privately imports `asio` and the root
module re-exports only `:tcp`. `mcpp build --profile dev --strict --cache=off` passed with LLVM
22.1.8. No Phase 6B public type or platform fallback was added at this gate.

## 2. Implement `IoContext` ownership and shutdown

In `src/tcp.cppm`, add the public immovable `IoContext` and only the private state required by the
Design:

- one `asio::io_context`;
- one persistent work guard;
- one admission mutex and accepting flag;
- one vector of weak socket states for shutdown;
- one `std::jthread` owned by `IoContext`.

Keep the shared context state declared before the driver member, as the existing `ThreadPool` keeps
its shared state alive until worker join. Let the driver capture the shared state while
`IoContext` itself remains the object that closes admission and joins the thread. Socket handles
retain only a weak context reference and cannot extend the public driver lifetime.

Linearize every accepted initiation/close post and the shutdown post through the same admission
mutex. Destruction must:

1. reject new posts;
2. queue shutdown after all previously accepted posts;
3. close every live registered socket on the I/O thread;
4. reset the work guard;
5. drain native completion handlers far enough to enqueue their return-Scheduler hops;
6. join the driver.

Do not call `io_context::stop()`. Detect destruction from the driver thread and terminate rather
than self-join. Propagate construction/thread-creation failures without leaving a thread alive.

Use a linear weak-state registry for v1 and prune expired entries while registering or shutting
down. Add a short `ponytail:` comment that the O(n) scan should change only if measured shutdown or
registration cost becomes significant.

Start `tests/tcp_test.cpp` with compile-time checks for the exact copy/move traits and runtime checks
for clean construction/destruction. The test imports `std`, `asio`, and `mcpplibs.cmp`; it must not
include platform socket headers.

**Completion — 2026-08-30:** Added the public immovable `IoContext`. Its shared private state owns
one `asio::io_context`, persistent work guard, admission mutex/flag, and linear weak socket
registry; `IoContext` owns one `std::jthread`. Shutdown rejects new posts, queues socket closure and
guard release behind accepted posts, drains `io_context` without `stop()`, detects self-join, and
joins the driver. `tests/tcp_test.cpp` locks the public traits and clean lifecycle. The focused test
passed 1/1; strict cache-off Dev build and the full Dev suite passed 117/117 across 10 binaries.

## 3. Add the single native-completion bridge

Implement one private callback-to-CMP awaiter that normalizes the Asio completion forms to:

```text
std::error_code + transferred byte count
```

Connect wraps its one-argument handler with a zero byte count; read and write use their native
counts. Keep the bridge private in `src/tcp.cppm` rather than adding a public completion-token or
executor abstraction.

The per-operation state must keep alive:

- the suspended private coroutine continuation;
- its `asio::cancellation_signal`;
- the error code and transferred count;
- cancellation provenance;
- a stop callback that holds weak operation/context references.

Initiate the Asio operation on the sole I/O thread, then install the stop callback. The callback
sets cancellation provenance and asks the context to emit `cancellation_type::all` on that same I/O
thread. It must never resume the coroutine or touch the socket from the requesting thread.

After native initiation, only the Asio handler may resume the private coroutine. Close and stop
only request that handler. This is the exactly-once rule; do not add a competing manual-resume
branch.

Every public template is a non-coroutine wrapper that copies the context/socket state, concrete
return Scheduler, stop token, address string, and span descriptor before returning a private lazy
helper Task. This avoids storing a lazy `this`, `IoContext&`, or address `string_view` reference.

Each helper captures value/error/cancellation, then awaits `returnTo.schedule()` without a stop
token before publishing the outcome. Use `std::scope_exit` for direction-admission cleanup rather
than creating another guard class. A return-Scheduler failure remains the observable exception.

**Completion — 2026-08-30:** Added one private `NativeOperationAwaiter` and shared operation state.
The completion handler overloads normalize connect and byte-transfer signatures to
`std::error_code` plus byte count. The state owns the continuation, cancellation signal,
error/count, cancellation origin, exception, and stop callback; the callback holds only weak
operation/context references and queues signal emission onto the I/O thread. After successful
native initiation, only the Asio handler resumes the private coroutine; a synchronous initiation
exception is the sole pre-initiation direct-resume path. An internal concrete instantiation checks
the otherwise-private template until Step 4 supplies real socket initiations. Strict cache-off Dev
and Release builds passed; both full suites passed 117/117 across 10 binaries. Runtime
completion/cancellation races remain intentionally gated on Step 4's real loopback connect rather
than a public test hook.

## 4. Implement connect and the loopback fixture

Add `TcpStream::connect()` with the Design signature. On the I/O thread:

1. accept the context post or retain its `std::logic_error`;
2. honor a pre-stopped token before parsing or opening a socket;
3. copy/parse only numeric IPv4 or IPv6 text;
4. construct and register one private socket state;
5. start `async_connect()` through the native-completion bridge;
6. return a move-only `TcpStream` only after a successful return-Scheduler hop.

Use Asio's original `std::error_code` in `std::system_error`. Do not add a CMP network error enum,
reject port zero, or invoke a resolver. If the return hop fails after native connect succeeds, the
undelivered stream's RAII destruction must close the socket.

In `tests/tcp_test.cpp`, add one small synchronous Asio loopback-server fixture on a `std::jthread`.
It binds port zero, exposes the selected port, uses latches/semaphores for protocol gates, owns all
server-side buffers, and joins deterministically. Keep protocol variants in this one fixture; do
not build a reusable production server layer for tests.

Lock connect behavior with focused tests for:

- Task laziness and copied temporary address storage;
- numeric IPv4 success and RunLoop return-thread affinity;
- IPv6 loopback when the host can bind it;
- invalid address and a test-owned non-listening endpoint as `std::system_error`;
- pre-cancelled connect with invalid address text yielding `OperationCancelled`, proving address
  parsing and native initiation were skipped;
- returning through `ThreadPool::Scheduler`;
- inactive/expired return Scheduler behavior.

Use timeouts only as deadlock guards. Do not use elapsed time or platform-specific error numbers as
pass conditions.

**Self-review — 2026-08-30:** The public wrapper must copy the address and weak context handle
before returning the lazy Task. A pre-stopped token is checked only after the context accepts the
I/O-thread initiation, preserving resource-error precedence while still skipping parse/open. The
bridge operation state gains one private lifetime anchor so the temporary socket survives through
its native handler. Invalid return-Scheduler tests connect successfully first and exercise the
post-connect cleanup path. Step 4 adds only this internal destruction path;
public `close()` and `is_open()` remain Step 6 work.

**Completion — 2026-08-30:** Added move-only `TcpStream` and its lazy numeric-address `connect()`.
The wrapper owns the address and a weak context handle before returning; accepted initiation checks
pre-cancellation, parses, creates/registers the socket, and calls `async_connect()` only on the I/O
thread. The bridge now retains an operation-specific lifetime anchor. Success, cancellation, parse
errors, connect errors, and context admission failures all attempt the explicit return-Scheduler
hop before publishing. Internal destruction closes an undelivered connected socket without
exposing Step 6 APIs early. The single synchronous loopback fixture covers IPv4, capability-gated
IPv6, laziness/address ownership, refusal, pre-cancellation, RunLoop/ThreadPool affinity, and
invalid return schedulers. Focused Dev and Release tests passed 8/8; the Release binary passed 100
repetitions, and both full suites passed 124/124 across 10 binaries.

## 5. Implement read, write, EOF, and overlap

Add `read_some()` and `write_all()` against the same private socket state.

All socket initiation occurs on the I/O thread. Maintain atomic logical-open/close state for
thread-safe `is_open()` and one pending flag per direction. One read and one write may coexist;
same-direction admission remains held until the public Task publishes its outcome. Reject overlap
with `std::logic_error` after the requested return hop rather than queueing hidden operations.

For reads:

- validate the resource before pre-cancellation and empty-buffer success;
- perform one `async_read_some()` for a non-empty span;
- return partial bytes immediately;
- retain EOF/error reported alongside bytes and deliver it on the next read;
- keep EOF sticky while preserving the write direction;
- mark the stream closed on a non-EOF socket error, while delivering one retained error before
  later closed-state rejection.

For writes:

- validate the resource before pre-cancellation and empty-buffer success;
- use `asio::async_write()` for the all-or-error contract;
- close after any initiated write cancellation or socket error;
- return `void` on the exact full transfer;
- never retry a partial write internally.

Add deterministic loopback tests for empty buffers, full write, partial read, server-send-then-close
with all bytes observed before repeated EOF, simultaneous read/write, a rejected second read, and a
rejected second write. To keep the second-write check deterministic, hold the first operation at a
gated return Scheduler until the overlap result is observed.

**Self-review — 2026-08-30:** Read terminal state is synchronized separately from the atomic
logical-open flag: an error delivered with bytes is retained and observed once before later reads
reject the closed stream, while EOF remains sticky without closing the write direction. A local
cancellation that arrives with bytes returns those bytes and retains no cancellation error.
Read/write admission uses one atomic flag per direction and is held through the return-Scheduler
hop. The configured LLVM 22 libc++ `std` module does not provide C++23 `std::scope_exit`, so the
helper releases explicitly on both schedule success and exception without adding a guard type.
Context/resource validation precedes pre-cancellation and empty-buffer
success; those immediate paths touch no socket. The existing bridge lifetime anchor retains the
socket for native operations. Tests extend the one loopback fixture with small protocol callbacks;
the read gate has a timeout only to break deadlocks, and the second-write case gates the first
return hop behind an occupied one-worker `ThreadPool`.

**Completion — 2026-08-30:** Added lazy non-coroutine `read_some()` and `write_all()` wrappers over
the existing socket state and native bridge. One atomic admission flag per direction permits a
read and write together while rejecting same-direction overlap through the requested return
Scheduler. Reads support empty buffers, partial delivery, retained terminal errors, sticky EOF,
and bytes-before-error; writes use Asio's composed `async_write()` and enforce all-or-error. The
configured libc++ lacks `std::scope_exit`, so admission release is explicit on both return-schedule
success and failure. The loopback fixture now accepts small synchronous protocols and enables
`reuse_address` for repeated tests. Focused Dev/Release passed 15/15; both full suites passed
131/131 across 10 binaries. Ten complete Release repetitions passed 150/150, and the two overlap
tests passed 100/100 across 50 repetitions. An earlier 100-repeat probe stopped in iteration 25
when this host's 301-port ephemeral range was exhausted; no CMP assertion or operation failed.

## 6. Implement cancellation, close, and shutdown races

Add thread-safe `close()` and `is_open()`. `close()` marks logical state closed synchronously, then
queues native close; it is idempotent, non-blocking, non-throwing, and a no-op on a moved-from
handle. `TcpStream` destruction performs the same request, while move construction transfers the
single logical ownership.

Track enough private provenance to distinguish local token/close/context cancellation from an
unrelated socket error when Asio reports `operation_aborted`. Translate only the local cases to
`OperationCancelled`.

Add deterministic tests for:

- active pending-read cancellation with the stream remaining usable;
- close during a pending read and idempotent repeated close;
- moved-from `close()`/`is_open()` and rejected moved-from operations;
- close-versus-completion and stop-versus-completion repeated races, each selecting one outcome;
- active write cancellation closing the stream;
- `IoContext` destruction with a pending read, handler drain, return handoff, and driver join;
- a stream outliving its destroyed context and failing later operations safely.

Prove a read is admitted before requesting active cancellation by using the deterministic
same-direction-overlap rejection while the server withholds data. For races, release server data
and request stop/close from a barrier; accept either documented winner but assert exactly one
completion and no duplicate buffer access.

**Self-review — 2026-08-30:** Each socket keeps weak connect/read/write operation slots populated
on the I/O thread before native initiation. Stop, explicit close, and context shutdown serialize on
that thread, record only the first local cancellation origin, and then ask the same Asio handler to
complete; none resumes a coroutine directly. Local cancel-and-close tags every live operation,
while an unrelated socket error performs an untagged native close and remains `system_error`.
Because an initiated write cancellation closes the whole stream, it also tags a simultaneous read
as locally cancelled. `close()` flips the atomic logical state before posting, so `is_open()` and
later admission observe closure immediately. Race tests accept exactly one documented outcome and
use barriers/semaphores only; the close race uses a bounded connection count to respect this host's
small ephemeral-port range.

The review also found a validation-to-initiation close window. A socket-local atomic close-request
marker now makes an already-admitted read/write report `OperationCancelled` if explicit close wins
before native initiation; closed-resource validation still precedes a pre-stopped token.

**Completion — 2026-08-30:** Added thread-safe `close()`/`is_open()` and made destruction use the
same idempotent close request. Weak per-direction operation slots let stop, close, write
cancellation, and context shutdown record local provenance on the I/O thread before native close;
only the Asio completion handler resumes an initiated operation. Tests cover active/pre-
cancellation, stream reuse, close idempotence and moved-from behavior, write cancellation,
close/completion and stop/completion races, context drain/join, surviving handles, return affinity,
and resource-error precedence. Focused Dev/Release passed 23/23; both full suites passed 139/139
across 10 binaries. Five focused Release repetitions passed 10/10 tests, representing 100 close
races and 500 stop races with no lost or duplicate completion.

## 7. Add the load and cross-platform gate

Extend the same test file with many concurrent IPv4 loopback clients. Give every client independent
stream/buffer ownership, count success/cancellation/error exactly once, and assert no lost or
duplicate completion. Keep the count high enough to exercise one I/O driver but low enough for all
three hosted CI systems.

Run the full `tcp_test` under both Dev and Release, then repeat the two completion-race tests in the
Release binary for at least 100 rounds. A hang, duplicate completion, unexpected exception, or
buffer mismatch fails the gate.

The existing Linux/macOS/Windows workflows already build the root package and auto-discover
`tests/tcp_test.cpp`; do not edit them merely to name the new test. Remote three-platform results
remain an acceptance gate and can be recorded only after the user separately authorizes the
required remote workflow.

**Self-review — 2026-08-30:** Use 32 IPv4 clients: this admits many simultaneous connect/read/write
operations on the one driver while staying well below hosted-CI and local ephemeral-port limits.
Extend the existing synchronous fixture with an expected connection count and process tiny echo
sessions sequentially on its test thread; the clients remain concurrent because all Tasks start
before the group joins. Each client coroutine owns its stream and request/reply arrays, writes one
unique payload, reads until that payload is complete, and records exactly one success,
cancellation, or error in its own result slot. Aggregate completion, outcome, payload, and return-
thread counts only after structured join. Keep the existing internally repeated race tests; five
focused binary repetitions represent 100 close races and 500 stop races without creating 100
copies of their already-repeated inner loops. No workflow or production API change belongs here.

**Completion — 2026-08-30:** Extended the existing test fixture with a bounded connection count
and added one 32-client IPv4 echo load. All client Tasks start before structured join; every Task
owns its stream and buffers, verifies its unique echo, returns to the RunLoop thread, and records
one outcome. Focused Dev/Release passed 24/24 with 32/32 successes and zero cancellations, errors,
affinity mismatches, or duplicate completions. Both full strict cache-off suites passed 140/140
across 10 binaries. Five focused Release repetitions passed 10/10 race tests, covering 100 close
races and 500 stop races. No production API or workflow changed; macOS/Windows confirmation remains
a separately authorized remote CI gate.

## 8. Migrate only the readiness TCP client

Keep `benchmarks/v1-readiness` POSIX-only and retain its synchronous loopback server on the existing
blocking `ThreadPool`. Do not touch the compute or file scenarios.

For each network worker:

1. create a test-owned listening endpoint before starting its tasks;
2. run accept/read/echo on `run_blocking()`;
3. connect one `TcpStream` through a shared benchmark `IoContext`;
4. call `write_all()` and loop `read_some()` until the 256-byte echo is complete;
5. count the existing 5,000 successes;
6. connect to a held, non-listening loopback endpoint for the existing 25 expected failures.

Preserve `NETWORK_WORKERS`, `NETWORK_ROUNDS`, `NETWORK_FAILURES`, payload size, CSV columns, and
hard accounting. Remove only the old blocking client socket operations that `TcpStream` replaces.
Timing remains local evidence, not a pass threshold.

Build the benchmark in Release strict mode and run five measured rounds. Record the new raw data in
`docs/benchmarks/2026-08-29-cmp-v1-readiness.md`, explicitly labeling the server as blocking and the
client as Phase 6B native async TCP.

**Self-review — 2026-08-30:** Preserve the current workload shape: each of four workers creates one
ephemeral listener, runs one synchronous accept plus a 5,000-round echo loop through
`run_blocking()`, and uses one long-lived `TcpStream` for those rounds. All four streams share one
benchmark `IoContext`; each write and read explicitly returns to the measuring RunLoop. A worker
owns its listener, server Task, stream, and 256-byte buffers until structured join. For the 25
failure rounds, keep one bound-but-not-listening socket alive and accept only
`std::errc::connection_refused` as expected. If a blocking server fails after the client has counted
every round, convert one success to unexpected failure so hard accounting remains exact. Reuse the
existing POSIX RAII socket and byte send/receive loops; remove only pair creation and blocking
client connect/send/receive code. Do not alter compute/file workloads, constants, CSV, or add a
benchmark dependency.

**Completion — 2026-08-30:** Replaced only the readiness benchmark's blocking client with four
`TcpStream` clients sharing one `IoContext`. Each worker keeps one long-lived connection for 5,000
256-byte echoes; its synchronous POSIX accept/read/write server remains isolated through
`run_blocking()`. One bound non-listening endpoint per worker drives 25 exact
`connection_refused` outcomes. Compute/file workloads, constants, CSV, and hard accounting are
unchanged. Release strict cache-off build passed, followed by five final runs in which every
scenario passed; network was 20,000 successes, 100 expected failures, and zero unexpected failures
per round. Raw data and the Phase 6A median comparison are recorded in the readiness report.

## 9. Synchronize the developer documentation

After behavior is verified, update the three README and three architecture variants with:

- the `IoContext`/`TcpStream` public surface;
- numeric-address-only connect;
- borrowed buffer lifetime;
- explicit return Scheduler affinity;
- one-read/one-write concurrency and close/cancellation behavior;
- the distinction between Phase 6A thread isolation and Phase 6B native async TCP;
- the verified test and benchmark counts.

Do not add a live-network call or a platform-specific loopback server to `examples/basic`; the
portable test fixture and readiness benchmark already exercise the API, while the basic example
should stay short and deterministic. Still run the unchanged example to verify that adding the
runtime dependency does not break an external path consumer.

**Self-review — 2026-08-30:** Keep one semantic checklist across all six documents rather than
copying implementation internals: `IoContext` owns one driver and is shared; `TcpStream` is
move-only; connect accepts numeric IPv4/IPv6 only; spans borrow storage through Task completion;
every outcome attempts the explicit return Scheduler; one read and one write may coexist while
same-direction overlap fails; pre-cancellation leaves an open stream usable, initiated write
cancellation closes it, and close/context shutdown cancel accepted work exactly once. State that
Phase 6A isolates arbitrary synchronous work on threads while Phase 6B is native async TCP only,
not generic async I/O. Update module/test trees, the exact local 10-binary/140-test counts, and the
readiness result. Add a short documentation-only TCP snippet, but leave `examples/basic` unchanged.
English, Simplified Chinese, and Traditional Chinese must remain structurally equivalent.

**Completion — 2026-08-30:** Synchronized all three README and architecture variants with the
verified `IoContext`/`TcpStream` surface, lifecycle and concurrency contracts, Phase 6A/6B
boundary, module/test layout, 140-test total, and readiness result. Removed stale statements that
native async I/O was wholly absent. Kept `examples/basic` unchanged; its path-consumer `mcpp run`
completed successfully with the expected output and exit status 0.

## 10. Full verification and state record

Run focused checks first, then the complete local matrix:

```text
mcpp build --profile dev --strict --cache=off
mcpp test --profile dev --strict --cache=off
mcpp build --profile release --strict --cache=off
mcpp test --profile release --strict --cache=off
cd examples/basic && mcpp run
cd benchmarks/v1-readiness && mcpp build --profile release --strict --cache=off
```

Run the readiness benchmark for five rounds and the focused Release cancellation/close races for
100 rounds. Record exact commands, test totals, race totals, benchmark counts, and failures; do not
describe an unexecuted platform or check as passing.

Only after local verification:

- mark the Design implemented locally;
- mark this Plan complete and add a concise completion record;
- update `.agent/HANDOFF.md` with implementation facts, remaining remote CI, and known limits.

Phase 6B reaches the Design's final completion state only after Linux, macOS, and Windows CI all
pass. Until then, report it as locally implemented and verified with remote compatibility pending.

## Scope Guard

If implementation reveals a need for DNS, listener APIs, timeouts, generic endpoints, socket
options, multiple driver threads, an operation queue, or a second I/O resource family, stop and
write a separate design. None belongs in this plan.
