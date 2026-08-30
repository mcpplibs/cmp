# CMP Phase 7 v1 Release Qualification Design

**Date:** 2026-08-30
**Status:** Reviewed design; implementation waits for Phase 6C merge and a new branch
**Package version:** `0.1.0`
**Baseline:** Phase 6C local tree, 161/161 Dev and Release tests

## Purpose

Turn the completed v1 capability set into a reproducible first release candidate. Phase 7 proves
that the public package can be built, tested, consumed, packaged, and validated on its declared
platforms without adding another runtime feature.

Here, “v1” names the first usable CMP capability set. The package remains `0.1.0`: the project has
not yet made the compatibility promise implied by semantic version `1.0.0`.

## Scope

Phase 7 includes only:

- freezing the public API implemented through Phase 6C;
- strict cache-off Dev and Release CI on Linux x86_64, macOS arm64, and Windows x86_64;
- standalone path-consumer verification in Dev and Release configurations;
- local readiness and release-package validation;
- checking package metadata and the generated mcpp-index descriptor;
- recording exact local, CI, package, and index evidence.

It does not add Task, Scheduler, synchronization, network, DNS, TLS, UDP, file-I/O, logging,
metrics, work-stealing, allocator, or compatibility APIs. A release failure is fixed at its root;
it is not hidden by weakening tests or expanding the runtime surface.

## Entry Gate

Phase 7 implementation starts only after:

1. Phase 6C is committed and pushed with explicit user authorization;
2. its PR passes the existing Linux, macOS, and Windows workflows;
3. the PR is merged with separate authorization;
4. local `main` is synchronized and a dedicated Phase 7 branch is created with authorization.

The Design and Plan may exist before that gate. Runtime, CI, version, and release-file changes may
not be mixed into the Phase 6C implementation branch.

## Release Candidate Contract

The release candidate exports the existing root module `mcpplibs.cmp` and keeps all current
contracts unchanged:

- lazy single-consumer `Task<T>` and explicit root driving;
- RunLoop timers and cooperative cancellation;
- `when_all`, TaskGroup, events, and AsyncMutex;
- fixed-size ThreadPool and structured `run_blocking()`;
- one-driver `IoContext`, numeric-address `TcpStream`, and `TcpListener`.

The existing exclusions remain exclusions. Phase 7 does not reinterpret them as release defects.
In particular, the first package does not promise DNS, TLS, UDP, generic I/O, detached tasks,
automatic cancellation propagation, or a multi-driver scheduler.

## Version and Metadata

Keep the manifest version at `0.1.0`. Before packaging, verify that `mcpp.toml` contains the exact
package identity and current repository metadata:

```toml
[package]
namespace   = "mcpplibs"
name        = "cmp"
version     = "0.1.0"
standard    = "c++23"
license     = "Apache-2.0"
repo        = "https://github.com/mcpplibs/cmp"
```

The release source must include the license, public module sources, required package metadata,
tests, and the standalone example. mcpp may preserve other tracked repository documentation in
the source archive; only `mcpp.toml` controls the package's build inputs. Generated build
directories, local tool environments, editor state, and generated release artifacts must not
become package inputs.

## CI Qualification

Keep the three existing platform workflows and their current pinned xlings/mcpp environment. Each
workflow must execute the same release-relevant commands:

```text
mcpp build --profile dev --strict --cache=off
mcpp test --profile dev --strict --cache=off
mcpp build --profile release --strict --cache=off
mcpp test --profile release --strict --cache=off
cd examples/basic
mcpp run
mcpp build --profile release --strict --cache=off
```

The example run remains the external import and runtime check. Its Release build adds a consumer
configuration check without depending on an unstable generated executable path.

Readiness throughput is not a CI threshold. The benchmark is Linux-local evidence because timing
and the current failure-endpoint helper are host-specific. CI remains deterministic and checks
correctness, exact test counts, and process exit status.

## Local Qualification

Before requesting the Phase 7 PR, run:

- strict cache-off Dev and Release root builds and all tests;
- the standalone example run and strict cache-off Release build;
- the readiness consumer in Release for five rounds, requiring exact success/failure counts and
  zero unexpected failures;
- focused Release race gates only when the implementation changed or a platform failure points at
  a race. Do not exhaust this host's 301-port ephemeral range with unbounded TCP repetitions;
- analyzer checks for every source or test file changed during release fixes.

Performance remains diagnostic data. A throughput change is reported, not failed, unless a future
separate Spec defines a reproducible threshold and environment.

## Package and Index Preflight

Use mcpp's existing release path instead of introducing packaging scripts:

```text
mcpp publish --dry-run
```

`mcpp publish --dry-run` may use `--allow-dirty` only for an early local preflight. With the pinned
tool, that flag permits the command but does not put uncommitted worktree content into the source
archive. Such an archive proves only the committed baseline's packaging path. The final
qualification runs from the merged clean release candidate without that flag. The generated
archive is extracted into a temporary directory and its root package, tests, and bundled example
are built from that extracted source to catch missing package inputs.

The pinned mcpp `2026.8.28.1` emits a useful starting descriptor, but its inline `mcpp` segment
does not contain `sources`, so that generated file does not parse directly. Normalize only a
temporary/index candidate to the mcpp-index native-package Form A:

- keep `namespace = "mcpplibs"` and use the atomic `name = "cmp"`;
- remove the inline `mcpp` segment so the source archive's own `mcpp.toml` remains authoritative;
- keep ordinary string URLs unless a real, separately authorized CN mirror exists;
- run `mcpp xpkg parse <normalized-temporary-file>` on that candidate.

Do not duplicate the source list in the index descriptor to accommodate this generator mismatch.
The package manifest already owns that information.

Do not add a second packager, release script, checked-in generated descriptor, or checked-in
archive. mcpp already owns those transformations.

## Remote Release Boundary

These are distinct, separately authorized remote operations:

1. push the Phase 7 branch;
2. create its PR;
3. merge the PR after all three CI workflows pass;
4. create and push the `v0.1.0` tag;
5. create the GitHub Release and attach the generated archive;
6. open the mcpp-index package PR with its Form A descriptor, minimal consumer test member,
   catalogue updates, and index-required design record;
7. merge the index PR after its validation and three-platform consumer matrix pass.

Authorization for one item does not authorize the next. A successful project PR is not evidence
that the GitHub Release or package-index entry exists.

## Documentation

English, Simplified Chinese, and Traditional Chinese README and architecture variants must agree
on:

- the implemented v1 capability set and deliberate exclusions;
- package version `0.1.0`;
- the exact local verification commands;
- whether Phase 6C and Phase 7 CI have actually passed;
- whether the package is actually available from mcpp-index.

Do not publish an index installation command before the index entry exists. The existing Spec and
Plan are sufficient release notes for the first candidate; no speculative changelog framework is
needed.

## Acceptance Criteria

Phase 7 is complete only when all of the following have authoritative evidence:

1. Phase 6C and Phase 7 PRs are merged without unresolved checks;
2. strict cache-off Dev and Release suites pass all 161 tests on Linux, macOS, and Windows;
3. the standalone example runs and its Release consumer build passes on all three platforms;
4. five final local readiness rounds have zero unexpected failures and exact hard counts;
5. the clean-tree publish dry-run succeeds;
6. the normalized Form A descriptor parses, and the extracted release source rebuilds, tests, and
   runs its example;
7. the `v0.1.0` GitHub Release contains the generated archive;
8. the mcpp-index PR passes validation and its three-platform consumer matrix;
9. public documentation describes only evidence that actually exists.

Until the remote items pass, CMP may be called locally release-ready but not publicly released or
index-verified.

## Self-review Corrections

The reviews corrected seven likely scope errors:

1. Keep `0.1.0`; a first usable capability set is not evidence for a `1.0.0` compatibility promise.
2. Reuse `mcpp publish --dry-run`; a custom release script or redundant checked-in descriptor adds
   another failure path.
3. Keep readiness timing out of CI; only deterministic correctness gates belong in the platform
   matrix.
4. Separate project CI, GitHub Release, and package-index publication. Each proves a different
   boundary and requires separate authorization.
5. End the planned roadmap here. Post-release features require a concrete consumer and their own
   Design; Phase 7 does not create a speculative Phase 8 backlog.
6. Treat mcpp's generated descriptor as a starting point, not index-ready truth: the pinned tool
   emits an invalid inline source segment and the index requires the atomic name plus Form A.
7. Do not mistake `--allow-dirty` for packaging dirty content. Its archive covers the committed
   baseline, so final Phase 6C package evidence must wait for a clean merged candidate.

## Final Roadmap Boundary

Phase 7 is the final planned CMP v1 stage. After it, maintenance fixes and demand-driven proposals
may be designed independently. Optional TaskGroup result handles, channels, DNS/TLS adapters,
other native I/O, allocator policies, or profiling-gated scheduler changes are not scheduled work.
