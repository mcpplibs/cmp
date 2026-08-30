# CMP Phase 7 v1 Release Qualification Implementation Plan

**Date:** 2026-08-30
**Design:** `docs/superpowers/specs/2026-08-30-cmp-phase7-v1-release-qualification-design.md`
**Status:** PR #12 open; Windows CI correction in progress, publication pending
**Target package:** `mcpplibs.cmp` `0.1.0`

## Execution Rule

Qualify the existing API; do not add another runtime feature. Stop at the first failing gate,
record the exact evidence, and fix only the root cause. Do not weaken tests, float tool versions,
or add packaging infrastructure already supplied by mcpp.

Do not commit, push, create or merge a PR, create a tag or Release, or modify mcpp-index without
the corresponding explicit user authorization.

## 1. Satisfy the entry gate

- submit Phase 6C only after authorization;
- require its Linux x86_64, macOS arm64, and Windows x86_64 checks to pass;
- merge only after separate authorization;
- synchronize local `main` and create a Phase 7 branch only after authorization;
- confirm that no Phase 6C worktree changes were lost or mixed with unrelated work.

No Phase 7 implementation file changes occur before this gate.

## 2. Freeze and audit the release surface

Read the root module, public partitions, README variants, architecture variants, manifest,
license, example, and three CI workflows. Confirm that every exported v1 type is documented and
that every documented type is exported.

Search public source and consumer-facing docs for:

- leaked Asio or platform socket types;
- stale test/module counts;
- claims for DNS, TLS, UDP, detached ownership, work stealing, or other excluded behavior;
- stale toolchain, xlings, mcpp, Asio, gtest, version, repository, or license metadata.

Correct only factual mismatches.

## 3. Strengthen the existing CI matrix

Edit only `.github/workflows/ci-linux.yml`, `ci-macos.yml`, and `ci-windows.yml` unless a proven
platform defect requires a targeted source/test fix.

Replace the default-profile build/test steps with strict cache-off Dev and Release commands on
Linux and macOS. Keep the existing pinned bootstrap and platform runner choices. Run the example
once and add its cache-off Release build.

On Windows, mcpp `2026.8.28.1` with LLVM reports the known missing-depfile degradation before tests
and `--strict` makes it fatal. Run one strict cache-off Dev probe and accept failure only when its
warning/error set exactly matches that single degradation. Then run the same cache-off Dev/Release
builds, all tests, example, and Release consumer build without `--strict`. Do not suppress output or
allow any other diagnostic. If a future mcpp fixes the degradation, the strict probe passes directly.

Do not add a new workflow, matrix abstraction, benchmark job, cache service, sanitizer job, or
floating dependency.

Validate YAML structure locally with the smallest available parser; if no parser is installed,
review indentation and let GitHub Actions be the authoritative workflow parser.

## 4. Run local release qualification

From the Linux qualification host's repository root:

```text
mcpp build --profile dev --strict --cache=off
mcpp test --profile dev --strict --cache=off
mcpp build --profile release --strict --cache=off
mcpp test --profile release --strict --cache=off
```

From `examples/basic`:

```text
mcpp run
mcpp build --profile release --strict --cache=off
```

From `benchmarks/v1-readiness`:

```text
mcpp build --profile release --strict --cache=off
<newly-built-current-binary>  # five sequential runs
```

Select the executable produced by the current build rather than another retained target hash.
All five rounds must keep the documented hard counts and report zero unexpected failures.

Run analyzer checks only for files changed by any release fix. Keep focused race repetitions
bounded to avoid host ephemeral-port exhaustion.

## 5. Validate the package transformation

Run an early preflight with:

```text
mcpp publish --dry-run --allow-dirty
```

Treat that archive as evidence only for the committed baseline: with the pinned tool,
`--allow-dirty` permits the command but does not include uncommitted content. After the release
candidate is merged and clean, repeat without `--allow-dirty`. Verify the printed namespace,
name, version, repository URL, license, archive name, hash, and release URL.

Copy the generated descriptor to a temporary directory. For the pinned mcpp `2026.8.28.1`,
normalize that temporary/index candidate to the native-package Form A before validating it:

```text
namespace = "mcpplibs"
name = "cmp"
omit the inline mcpp segment
mcpp xpkg parse <temporary>/cmp.lua
```

Keep ordinary string URLs unless a real CN mirror exists and is authorized. Do not copy the
manifest's source list into the descriptor; the extracted source's `mcpp.toml` is the single
build description.

Extract the generated source archive into another temporary directory. There, run strict Dev and
Release root build/tests, the example run, and its Release build. Delete only those explicit
temporary directories after validation.

Do not check generated archives or descriptors into this repository.

## 6. Self-review the complete Phase 7 change

Review every changed line for:

- accidental API or version changes;
- inconsistent commands across the three workflows;
- Windows shell/path assumptions;
- claims ahead of evidence;
- generated or local files entering release inputs;
- weakened tests, an unbounded strict-mode exception, or cache behavior;
- remote steps that blur authorization boundaries.

Run whitespace/encoding checks without changing unrelated CRLF files. Update `.agent/HANDOFF.md`
with exact local results and remaining remote gates.

## 7. Run the authorized project PR gates

Only after separate authorization:

- create the local commit with a concise Chinese message;
- push the Phase 7 branch;
- create the PR;
- wait for Linux, macOS, and Windows checks;
- diagnose and fix any failure through the same Design/Plan loop;
- merge only after separate authorization.

Record actual workflow URLs and outcomes. “PR opened” is not equivalent to “CI passed”.

## 8. Run the separately authorized publication gates

From merged, synchronized `main`, and only with authorization for each remote mutation:

1. rerun the clean-tree package preflight;
2. create and push annotated tag `v0.1.0`;
3. create the GitHub Release and attach the exact generated archive;
4. prepare the mcpp-index Form A descriptor, minimal consumer test member, catalogue entries, and
   index-required design record;
5. create the mcpp-index package PR and wait for descriptor validation and three-platform consumer
   builds;
6. merge the index PR only with authorization;
7. verify `mcpp search cmp` and an isolated `mcpp add cmp@0.1.0` consumer after index publication.

Only then update public documentation to say the package is installable from mcpp-index.

## 9. Close the v1 roadmap

Update the Design, this Plan, public documentation, and HANDOFF with evidence rather than intent.
Mark Phase 7 complete only after every acceptance criterion in the Design is proven.

Do not start a Phase 8 automatically. A future feature begins with a concrete consumer problem and
a new independently reviewed Design.

## Completion Record

Local implementation record, 2026-08-30:

- entry gate satisfied: Phase 6C PR #11 passed Linux, macOS, and Windows, merged with
  authorization, and local `main` was synchronized to `02cb6b9` before creating
  `release/phase7-v1-release-qualification`;
- release-surface audit found no public Asio leak, version drift, missing export, or unsupported
  capability claim;
- the Linux and macOS workflows run strict cache-off Dev/Release root gates, the example, and its
  Release build; Windows runs an exact allowlisted strict probe followed by matching cache-off
  profiles without `--strict`; all three files parse as YAML;
- local Dev and Release root suites each passed 10 binaries and 161/161 tests; the example run and
  Release build passed;
- five readiness rounds preserved compute `49,000/1,000/0`, file I/O `1,000/100/0`, and network
  `20,000/100/0` success/expected-failure/unexpected-failure counts in every round;
- the committed Phase 6C package preflight produced a provisional 206,341-byte archive with
  SHA-256 `fe36976fe76225f6121cce81ca22a1b2392a833795795fb9a00ed52d98dba4de`;
- its normalized Form A descriptor parsed, and the extracted archive passed strict cache-off Dev
  and Release root builds and 161 tests, the example run, and its Release build;
- final local self-review confirmed that only the three existing workflows and release-state
  documentation changed; YAML structure, command parity, CRLF, and `git diff --check` passed.

Project PR #12 was created with authorization. Its first Windows run proved that a universal
`--strict` gate stops on mcpp's documented missing-depfile degradation before CMP tests execute;
the exact-probe correction above is in progress. Its extracted Bash step passes syntax checking,
runs successfully when strict mode succeeds, accepts a synthetic exact known degradation, and
rejects a synthetic additional warning. That failed CI run is not release evidence.

The Phase 7 project PR, three-platform CI, clean merged archive, tag, GitHub Release, mcpp-index PR,
index CI/merge, and isolated indexed consumer check remain pending. Each remote mutation retains
the authorization boundary defined by the Design.
