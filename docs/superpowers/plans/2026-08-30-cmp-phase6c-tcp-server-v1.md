# CMP Phase 6C TCP Server v1 Implementation Plan

**Date:** 2026-08-30
**Design:** `docs/superpowers/specs/2026-08-30-cmp-phase6c-tcp-server-v1-design.md`
**Status:** Complete locally; cross-platform CI pending
**Baseline:** `feature/phase6c-tcp-server-v1` from `main` commit `9492075`; 140/140 tests

## Execution Rule

Implement only the approved listener slice. Reuse the existing `:tcp` partition, `IoContext`,
`TcpSocketState`, native-operation bridge, `TcpStream`, Scheduler constraints, and cancellation
exception. Run the smallest focused check after each implementation gate and stop on the first
failure.

Do not add DNS, TLS, generic endpoints, socket options, a resource hierarchy, multiple driver
threads, detached handlers, or another dependency.

## 1. Extend context ownership for listeners

In `src/tcp.cppm`:

- forward-declare one private `TcpListenerState`;
- add one linear weak-listener registry beside the existing weak-socket registry;
- register listeners only on the I/O driver;
- close listeners before streams during context shutdown;
- keep the existing admission mutex, ordered shutdown post, work-guard drain, and driver join.

Do not replace the two concrete registries with a virtual resource base. Add one short
`ponytail:` comment explaining that the linear registry changes only after measured cost.

Run a strict cache-off Dev build before adding public APIs.

## 2. Add driver setup bridge and listener state

Add one private void driver-operation awaiter used only for bind setup. Its shared state owns the
private continuation and any exception. `await_suspend()` posts one callable through
`IoContextState::post()`; the callable runs on the sole driver, stores its outcome, and resumes the
private helper. `await_resume()` rethrows a stored exception.

Add `TcpListenerState` with:

- weak context ownership;
- one optional Asio acceptor;
- atomic logical-open and close-requested flags;
- atomic single-accept admission;
- one weak active accept operation;
- the bound local port;
- driver-only bind, operation attach, cancel-and-close, and native-close helpers.

All driver callables and native close paths are non-throwing at their outer boundary. Original
system errors are captured for the helper Task rather than escaping the driver loop.

Run a strict cache-off Dev build.

## 3. Implement lazy `TcpListener::bind()`

Add the exact public traits and signature from the Design. The public function is a non-coroutine
wrapper that copies the weak context state, Scheduler, address string, port, and stop token into a
private lazy helper.

On the driver:

1. reject a pre-requested stop before parsing;
2. parse numeric IPv4/IPv6 only;
3. open, bind, and listen using the native maximum backlog;
4. query/store the actual local port;
5. mark open and register the listener.

Every outcome then awaits the uncancellable return Scheduler. A failed return hop closes an
undelivered bound listener through RAII.

Add focused tests for traits, laziness/address ownership, IPv4/IPv6, port zero, invalid address,
occupied port, pre-cancellation, ThreadPool return affinity, and invalid return Schedulers.

Run focused Dev and Release `tcp_test` checks.

## 4. Implement cancellable `accept()`

Reuse `NativeOperationAwaiter` and pre-create one candidate `TcpSocketState` on the driver. Register
the candidate with the context before initiation, attach the active operation to the listener,
then call `async_accept()` with the candidate socket and the existing cancellation-bound completion
handler.

On success, mark the candidate socket open only if context shutdown has not already closed it, then
construct `TcpStream`. On error or cancellation, close the candidate before publishing. Hold
listener accept admission until after the return hop. Do not close the listener for an unrelated
native accept error.

Add focused tests for one accepted bidirectional exchange, pre/active cancellation with reuse,
single-pending rejection, return affinity, and accepted-stream independence from listener close.

Run focused Dev and Release checks.

## 5. Complete close, move, shutdown, and race boundaries

Implement public `close()`, `is_open()`, and `local_port()`. Destruction uses the same close request;
move construction transfers ownership and the port snapshot.

Explicit close and context shutdown tag the active accept operation before closing the acceptor.
Only the Asio handler resumes an initiated accept. Check close-requested immediately before native
initiation to cover validation-to-initiation races.

Add deterministic tests for close during accept, repeated close, moved-from behavior, context
shutdown with a pending accept, listener survival after context destruction, and repeated
stop/close versus completion races with exactly one outcome.

Run the focused Release race tests repeatedly without exhausting this host's small ephemeral-port
range.

## 6. Add bounded listener load and migrate readiness server

In `tests/tcp_test.cpp`, run a bounded multi-client loop through one listener. Accept sequentially
and use structured child Tasks for established streams. Assert one completion and correct payload
for every client, with zero cancellation, errors, wrong-thread publication, or duplicates.

In `benchmarks/v1-readiness/src/main.cpp`, replace only the blocking POSIX accept/read/write server
path with `TcpListener` plus accepted `TcpStream` operations. Preserve compute/file workloads,
network workers/rounds/failure probes, payload size, CSV columns, and exact accounting. Retain POSIX
only where the benchmark already needs it for the held non-listening failure endpoint.

Build the benchmark in Release strict cache-off mode and run five rounds. Timing is evidence, not
a threshold; all hard counts must pass with zero unexpected failures.

## 7. Synchronize documentation

After behavior is verified, update all README and architecture language variants with:

- `TcpListener::bind()` and `accept()`;
- numeric-address and port-zero behavior;
- one-pending-accept rule;
- explicit return affinity;
- accept cancellation, close, and context-shutdown behavior;
- accepted-stream independence;
- updated module/test counts and readiness server description.

Keep `examples/basic` unchanged and run it as the external path-dependency compatibility check.

## 8. Full verification and state record

Run:

```text
mcpp build --profile dev --strict --cache=off
mcpp test --profile dev --strict --cache=off
mcpp build --profile release --strict --cache=off
mcpp test --profile release --strict --cache=off
cd examples/basic && mcpp run
cd benchmarks/v1-readiness && mcpp build --profile release --strict --cache=off
```

Then run five readiness rounds and the focused Release accept races repeatedly. Record only checks
actually executed.

Finally:

- self-review the complete diff for lifetime, cancellation, duplicate-resume, return-affinity,
  and scope errors;
- mark the Design implemented locally and this Plan complete only after all local gates pass;
- update `.agent/HANDOFF.md` with current branch, exact test totals, benchmark counts, known limits,
  and remaining remote CI.

Do not commit, push, create a PR, or merge without separate user authorization.

## Completion Record

All eight implementation gates are complete locally. `TcpListener` reuses the existing TCP
partition and native operation path; the readiness server now uses `TcpListener` and accepted
`TcpStream` operations, while the POSIX helper remains only for the non-listening failure probe.
Self-review also corrected the bind/connect/accept shutdown publication window by rechecking each
unpublished resource after its return-Scheduler hop. Reusable read/write/accept operation slots
now rely on their pending-admission flags instead of the previous Task frame's destruction timing.

Strict cache-off Dev and Release builds passed, and both complete suites passed 161/161 tests in
ten binaries. The TCP binary contains 45 tests, including ThreadPool and invalid-Scheduler return
paths for both bind and accept, a deterministic shutdown-before-publication gate, a 32-client
listener load, context shutdown, and repeated completion races. The publication gate passed 20
repeated Release runs; the stream/listener concurrent-close gate passed 100 Release runs with 800
concurrent close calls;
five race repetitions passed 100 close/accept and 500 stop/accept races. The unchanged standalone
example passed. Five final readiness rounds reported exact
success/expected failure counts and zero unexpected failures; native async TCP client/server
median throughput was 8,976.6 ops/s.

No commit, push, PR, merge, or remote CI action is included in this local implementation.

## Scope Guard

If implementation requires public endpoints, backlog/options, multiple simultaneous accepts,
multiple drivers, DNS, TLS, detached ownership, or a generic I/O resource base, stop and write a
separate design. None belongs in Phase 6C v1.
