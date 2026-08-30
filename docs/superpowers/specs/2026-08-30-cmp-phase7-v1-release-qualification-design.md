# CMP Phase 7 v1 Release Qualification Design

**Date:** 2026-08-30
**Status:** PR #12 open; three-platform qualification passed, merge and publication pending
**Package version:** `0.1.0`
**Baseline:** Phase 6C PR #11 merged as `02cb6b9`, 161/161 Dev and Release tests

## Purpose

Turn the completed v1 capability set into a reproducible first release candidate. Phase 7 proves
that the public package can be built, tested, consumed, packaged, and validated on its declared
platforms without adding another runtime feature.

Here, “v1” names the first usable CMP capability set. The package remains `0.1.0`: the project has
not yet made the compatibility promise implied by semantic version `1.0.0`.

## Scope

Phase 7 includes only:

- freezing the public API implemented through Phase 6C;
- strict cache-off Dev and Release CI on Linux x86_64 and macOS arm64, plus an allowlisted strict
  probe and full cache-off Dev/Release CI on Windows x86_64;
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

The gate was satisfied on 2026-08-30: PR #11 passed Linux x86_64, macOS arm64, and Windows x86_64,
was merged with authorization, local `main` was fast-forwarded to `02cb6b9`, and the dedicated
Phase 7 branch was created.

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

Keep the three existing platform workflows and their current pinned xlings/mcpp environment.
Linux and macOS execute these release-relevant commands:

```text
mcpp build --profile dev --strict --cache=off
mcpp test --profile dev --strict --cache=off
mcpp build --profile release --strict --cache=off
mcpp test --profile release --strict --cache=off
cd examples/basic
mcpp run
mcpp build --profile release --strict --cache=off
```

The mcpp `2026.8.28.1` Windows LLVM backend intentionally reports `build/depfile` because it cannot
emit GNU depfiles. `--strict` converts that known incremental-build degradation into a fatal error
before the test suite runs. The Windows workflow therefore first executes a strict cache-off Dev
build as a probe. A failed probe is accepted only when its complete warning/error set is exactly
the single known depfile degradation and the corresponding strict-mode summary. It then executes
the same Dev/Release profiles and example flow without `--strict`, retaining `--cache=off` for every
explicit build/test gate. Any different strict diagnostic, compile failure, test failure, or example
failure remains fatal. A future mcpp version that fixes Windows depfiles makes the probe pass
directly without changing this workflow.

This exception does not hide stale objects: cache-off qualification rebuilds from source, while the
shared manifest remains under strict validation on Linux and macOS. Requiring an impossible Windows
strict compile would test the build tool's documented limitation instead of CMP.

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

## Local Qualification Evidence

The Phase 7 branch completed the local gates on 2026-08-30:

- strict cache-off Dev and Release root builds passed; both suites ran 10 binaries and all
  161 tests passed;
- the standalone example ran with its expected ten lines and its strict cache-off Release build
  passed;
- five Release readiness rounds preserved the exact hard counts in every round: compute
  `49,000/1,000/0`, file I/O `1,000/100/0`, and network `20,000/100/0` for
  success/expected failure/unexpected failure;
- the five-round medians were `38.475 ms / 1,299,546.0 ops/s` for compute,
  `217.363 ms / 5,060.7 ops/s` for file I/O, and
  `2,622.017 ms / 7,665.9 ops/s` for network;
- all three edited workflows parsed as YAML and contain matching profiles and cache-off gates;
  Windows additionally contains the exact allowlisted strict probe described above;
- an early `mcpp publish --dry-run --allow-dirty` from committed merged Phase 6C produced
  `cmp-0.1.0.tar.gz` (206,341 bytes, provisional SHA-256
  `fe36976fe76225f6121cce81ca22a1b2392a833795795fb9a00ed52d98dba4de`);
- the normalized temporary descriptor parsed as native-package Form A, and the extracted archive
  passed strict cache-off Dev and Release root builds and all 161 tests, the example run, and its
  Release build.

That archive intentionally excludes the dirty Phase 7 workflow and documentation edits. It proves
the merged Phase 6C package contents and transformation only. The authoritative archive and hash
must be regenerated from clean merged Phase 7. No runtime source or test file changed in this local
qualification, so no new analyzer run was required.

## Project PR Evidence

PR #12 evaluated correction commit `b2c0911` on 2026-08-30 and passed all three workflows:

- Linux x86_64 completed in 2m53s;
- macOS arm64 completed in 3m08s;
- Windows x86_64 completed in 5m22s, including the exact allowlisted strict probe.

The complete logs show two suites per platform—Dev and Release—and every suite ran 10 test
binaries with 161/161 tests passing. Each platform also printed the expected ten-line example and
completed its Release consumer build. The Windows strict probe contained exactly the known missing
GNU depfile warning and its strict-mode summary; the following cache-off Dev/Release gates passed.
PR #12 remains open and merge authorization has not been granted.

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
2. strict cache-off Dev and Release suites pass all 161 tests on Linux and macOS; the allowlisted
   Windows strict probe and cache-off Dev/Release suites pass all 161 tests;
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

The reviews corrected eight likely scope errors:

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
   baseline, so final Phase 7 package evidence must wait for a clean merged candidate.
8. Do not require mcpp's known Windows LLVM depfile degradation to disappear. Allowlist it with an
   exact strict probe, then use cache-off full builds; Linux and macOS retain strict manifest gates.

## Final Roadmap Boundary

Phase 7 is the final planned CMP v1 stage. After it, maintenance fixes and demand-driven proposals
may be designed independently. Optional TaskGroup result handles, channels, DNS/TLS adapters,
other native I/O, allocator policies, or profiling-gated scheduler changes are not scheduled work.
