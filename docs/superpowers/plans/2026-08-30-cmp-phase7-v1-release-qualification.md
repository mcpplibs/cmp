# CMP Phase 7 v1 Release Qualification Implementation Plan

**Date:** 2026-08-30
**Design:** `docs/superpowers/specs/2026-08-30-cmp-phase7-v1-release-qualification-design.md`
**Status:** Pending Phase 6C merge and authorized branch transition
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

Replace the default-profile build/test steps with strict cache-off Dev and Release commands. Keep
the existing pinned bootstrap and platform runner choices. Run the example once and add its strict
cache-off Release build.

Do not add a new workflow, matrix abstraction, benchmark job, cache service, sanitizer job, or
floating dependency.

Validate YAML structure locally with the smallest available parser; if no parser is installed,
review indentation and let GitHub Actions be the authoritative workflow parser.

## 4. Run local release qualification

From the repository root:

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
- weakened test, strictness, or cache behavior;
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

Pending. Fill this section only with commands and remote checks actually completed.
