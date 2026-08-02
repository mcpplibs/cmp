# CMP Bootstrap Design

**Date:** 2026-08-02
**Status:** Approved for implementation
**Base:** `main` at `13b95c3`

## Purpose

Turn the repository from the generic `mcpplibs.mylib` scaffold into the initial CMP project
without implementing a coroutine runtime. The result must be a clean, honest C++23 module
package that builds, tests, and can be imported by a standalone consumer.

## Approved Identity

- Project name: **CMP**
- Expansion: **Coroutine Machine Process**
- Package identity: `mcpplibs` + `cmp`
- C++ module: `mcpplibs.cmp`
- C++ namespace reserved for future public APIs: `mcpplibs::cmp`
- Version: `0.1.0`
- C++ baseline: explicitly set `standard = "c++23"`
- License: Apache-2.0
- Repository: `https://github.com/mcpplibs/cmp`
- Package description: `A C++23 coroutine runtime project built with mcpp and C++ Modules`

The letter `C` is intended as CMP's lightweight coroutine execution-unit concept, analogous
in naming motivation to Go runtime's `G`. The documentation must not claim semantic or runtime
equivalence before CMP implements and verifies such behavior.

## Scope

### Included

1. Finish local Windows cleanup left by the clice evaluation.
2. Make local generated/tool artifacts consistently ignored.
3. Replace project-specific `mylib` identity with CMP throughout package metadata, source,
   tests, example, documentation, CI, and repository-specific agent guidance.
4. Remove the two generic mcpp publishing templates and their smoke-test script.
5. Keep a minimal import-only module, test, and standalone consumer example.
6. Validate the result on the verified Windows-native mcpp/LLVM toolchain.
7. Prepare the repository's first PR description locally, including the bootstrap scope,
   template-removal rationale, ecosystem review, and validation evidence.

### Excluded

- Coroutine `task` types or promises
- Schedulers, workers, work stealing, timers, cancellation, async I/O, or blocking pools
- A CMake build alongside mcpp
- Publishing CMP to mcpp-index
- Staging, committing, pushing, or opening a PR during the current editing session
- Uninstalling the disabled clice VS Code extension
- Claiming that arbitrary blocking calls become non-blocking or that CMP already behaves like
  the Go runtime

## Chosen Approach

Use a pure identity bootstrap. The root module exports no placeholder function. In particular,
do not rename `hello_mcpplibs()` to `hello_cmp()` and do not invent metadata APIs solely to make
the bootstrap test call a symbol. This avoids creating a public API that the first runtime work
would immediately remove.

The module interface will contain only the module declaration:

```cpp
export module mcpplibs.cmp;
```

`mcpplibs::cmp` remains the namespace policy for future exported runtime APIs. The bootstrap
test and example prove that an independent translation unit can import `mcpplibs.cmp`.

## Local Windows Cleanup

The cleanup is local-only except for the tracked `.gitignore` improvement:

1. Confirm no `clice.exe` process is running and clangd resolves to xlings LLVM tools 20.1.7.
2. Delete these clice experiment directories after resolving and verifying the exact paths:
   - `E:\CPPCodes\mcpplibs\cmp\.clice`
   - `C:\Users\80945\AppData\Local\clice-mcpp\E__CPPCodes_mcpplibs_cmp`
   - `C:\Users\80945\AppData\Local\Temp\cmp-clice-eval`
3. Remove only the stale `[cpp].editor.defaultFormatter = "clice-io.clice"` property from the
   VS Code user settings, preserving all other settings and any other `[cpp]` properties.
4. Delete existing `.cache/` directories so clangd can rebuild them.
5. Preserve the `.rpiv/` handoff locally; it is project-management state, not PR content.
6. Once tracked ignores cover the paths, remove redundant `.vscode/` and `.clice/` entries from
   `.git/info/exclude` while keeping its standard comments intact.

No tracked source file is touched until the cleanup and baseline checks complete.

## Ignore Policy

Keep the existing build-product rules and add root-anchored ignores for repository-local tool
state:

```gitignore
.cache/
/.clice/
/.rpiv/
/.vscode/
```

The cache rule intentionally applies at every depth because both the repository root and the
standalone example can acquire clangd caches. The other rules are root-anchored so they do not
hide similarly named content in future examples or fixtures. The handoff remains available
locally under `.rpiv/`, while `git status` stays focused on product changes.

## Repository Changes

### Package and module

- Update `mcpp.toml` with the approved identity, explicit C++23 baseline, description, and CMP
  repository URL.
- Rename `src/mylib.cppm` to `src/cmp.cppm` and replace its contents with the import-only root
  module declaration.
- Rename `tests/mylib_test.cpp` to `tests/cmp_test.cpp`; import `mcpplibs.cmp` and use a single
  gtest smoke case whose success is compilation and execution.

### Standalone example

- Rename the package in `examples/basic/mcpp.toml` to `cmp-basic`.
- Explicitly set `standard = "c++23"` in the standalone example as well as the root package.
- Change its path dependency to `cmp = { path = "../.." }` under
  `[dependencies.mcpplibs]`.
- Import `mcpplibs.cmp` in `examples/basic/src/main.cpp` and return success without relying on a
  placeholder CMP API.

### Documentation

Rewrite, rather than mechanically search-and-replace, these documents:

- `README.md`
- `README.zh.md`
- `README.zh.hant.md`
- `docs/architecture.md`
- `docs/architecture.zh.md`
- `docs/architecture.zh.hant.md`

All three language variants must communicate the same facts:

- CMP's full name and C++23/mcpp/C++ Modules baseline
- bootstrap-stage status and the absence of runtime APIs
- the intended long-term direction: standard stackless coroutines, explicit `co_await`, and
  later incremental work on scheduling, timers, async I/O, cancellation, and blocking pools
- the boundary between naming inspiration and actual Go-runtime equivalence
- Windows/Linux/macOS build and test entry points
- the current repository layout and standalone consumer example
- Apache-2.0 licensing

Replace template-repository URLs, badges, issue links, and agent prompts with CMP URLs and remove
instructions that tell readers to use or rename the generic template.

#### Architecture documentation style

The three architecture documents are technical references for contributors, not product pitches
or speculative runtime designs. Their revision follows these rules:

- state the current repository structure and verified tool behavior in the present tense;
- separate current implementation from possible future work;
- keep future work to a short, explicitly non-binding section;
- remove promotional adjectives, process narration, and phrases that merely announce that a
  statement is factual;
- omit one-off handoff details such as the separate WSL checkout and local Agent state;
- retain the Windows LLVM depfile limitation because it changes how authoritative local
  verification is performed;
- use common technical terms where they improve precision, but avoid unnecessary English nouns
  in the Chinese documents;
- preserve the same facts and section order across all three languages without translating
  sentence by sentence.

The target structure is: current status, package and module identity, repository layout, build and
test relationships, standalone consumer, current constraints, possible extensions, and
verification. No source, manifest, CI, or README behavior changes are part of this editorial pass.

### Publishing templates and CI

Delete:

- `templates/basic/`
- `templates/lib/`
- `tools/template_smoke.sh`

Remove template-smoke invocations and template-specific prose from all three CI workflows and
documentation. Keep `examples/basic` as the supported external-consumer smoke test. Purpose-built
CMP templates may be designed later, after a stable runtime API exists.

`templates/basic` and `templates/lib` are package publishing assets for `mcpp new`; they are not
the library's own build targets or examples. `tools/template_smoke.sh` renders every published
template, substitutes the mcpp template placeholders, rewrites the generated dependency to point
at the current checkout, and then builds or runs the generated project. Once CMP stops publishing
the two inherited templates, the script has no input and its CI step would no longer test a CMP
artifact. This relationship and the reason for deleting all three pieces must be stated in the
first PR description.

### Ecosystem convention review

The bootstrap was compared on 2026-08-02 with current default-branch snapshots of these primary
sources:

- [`mcpplibs/template` at `e999fc1`](https://github.com/mcpplibs/template/tree/e999fc164b6ebdd1f107b848f7b2e1f19c5f4c13)
  for the current mcpp library scaffold;
- [`mcpplibs/cmdline` at `d228811`](https://github.com/mcpplibs/cmdline/tree/d2288114322d5b648b3c7ed8d49d97948be28eac)
  for a small mcpp module library without publishing templates;
- [`mcpplibs/llmapi` at `a48d0cd`](https://github.com/mcpplibs/llmapi/tree/a48d0cd3147b5bf67b4c25f98764d47cf7f028d1)
  for a multi-module library with package-specific templates and a template smoke script;
- [`mcpplibs/imgui-m` at `3e9f3d3`](https://github.com/mcpplibs/imgui-m/tree/3e9f3d315c1aa086abb134dd22ef1d69b226fa31)
  for a package that uses an explicit library root and ships templates without the same smoke
  script;
- [`mcpplibs/primitives` at `14dae86`](https://github.com/mcpplibs/primitives/tree/14dae8635629f6ca61ca5aec78bf899fb4e68d28)
  as evidence that the organization also contains non-mcpp project layouts;
- the current [mcpp manifest guide](https://github.com/mcpp-community/mcpp/blob/main/docs/05-mcpp-toml.md)
  and [mcpp-index](https://github.com/mcpplibs/mcpp-index).

The comparison supports the following decisions:

1. `namespace = "mcpplibs"`, `name = "cmp"`, `src/cmp.cppm`, and module
   `mcpplibs.cmp` follow the standard package and library-root conventions used by the template,
   `cmdline`, and `llmapi`.
2. An explicit `standard = "c++23"` is valid and makes the baseline visible even though C++23 is
   mcpp's default.
3. A gtest-only file under `tests/` with gtest in `[dev-dependencies]`, plus a standalone example
   using a root path dependency, matches the current scaffold pattern.
4. Publishing templates are optional package assets. `cmdline` has none; `llmapi` and `imgui-m`
   ship templates tied to implemented APIs. A template smoke script is therefore not an
   organization-wide requirement.
5. CI layouts vary between repositories. CMP's three workflows retain the scaffold's
   build-test-example checks and remove only the step whose input was deleted.
6. Lock-file policy also varies. CMP continues the scaffold policy of ignoring `mcpp.lock`; the
   documentation treats this as a repository choice, not a rule for all libraries.
7. The tracked ignore rules extend the scaffold defaults with repository-local cache, editor,
   clice, and handoff paths. Comparable mcpplibs repositories also ignore local editor and cache
   directories; CMP's root anchoring avoids hiding similarly named product content elsewhere.
8. `mcpp search cmp` returned no package match on 2026-08-02, while `cmdline` and `llmapi` resolved
   under `mcpplibs`. The chosen identity has no current index collision. Publishing remains out of
   scope.

The review establishes compatibility with current practice; it does not turn choices from one
repository into organization-wide requirements.

### Agent guidance

Update repository-specific paths and descriptions in `.agents/skills/more-details/SKILL.md` and
other directly affected guidance. Generic educational examples that deliberately use `mylib` may
remain only when they are clearly not describing this repository. Every search hit will be
reviewed rather than blindly replaced.

## Error and Compatibility Policy

The bootstrap introduces no runtime error model. Build, test, or example failures block the PR.
Because the repository is still the untouched scaffold at version `0.1.0`, replacing the
placeholder import path is intentional and no compatibility alias for `mcpplibs.mylib` will be
provided.

## Validation

Before tracked implementation changes:

```powershell
mcpp --version
mcpp self doctor
mcpp test --cache=off
```

After implementation:

```powershell
mcpp build --cache=off
mcpp test --cache=off
Push-Location examples\basic
mcpp run
Pop-Location
```

Repository checks:

```powershell
rg --hidden -n 'mylib|mcpplibs\.mylib|mcpplibs/template' -g '!.git/**' -g '!.rpiv/**'
& 'E:\Git\cmd\git.exe' diff --check
& 'E:\Git\cmd\git.exe' status --short
```

Any remaining search hit must be either removed or explicitly identified as a generic teaching
example. There is no template smoke command after the templates are deleted.

## Git and PR

- Work on `codex/bootstrap-cmp`, never directly on `main`.
- During the current session, do not stage, commit, push, create or switch branches, or open a PR.
- Keep the proposed PR body under `.rpiv/artifacts/pr-drafts/`; `.rpiv/` remains ignored and the
  body is copied into GitHub only after the user authorizes PR creation.
- The draft must state that the change initializes identity only and contains no coroutine runtime
  implementation.
- The draft must explain what the removed templates and `tools/template_smoke.sh` did, why their
  deletion is coupled, and why `examples/basic` remains.
- The draft must record the ecosystem sources reviewed, the convention conclusions, the dated
  `mcpp search cmp` result, and the local validation commands.
- Before any later push, re-check the remote default branch and obtain the user's authorization for
  staging, committing, pushing, and opening the PR.

## Acceptance Criteria

The bootstrap is complete when:

1. Tracked project files consistently identify CMP and `mcpplibs.cmp`.
2. `mcpp.toml` explicitly declares C++23 and retains Apache-2.0.
3. The old publishing templates and template smoke infrastructure are absent.
4. Local tool artifacts no longer pollute Git status.
5. The root package, test suite, and standalone consumer all pass with `--cache=off` where
   applicable.
6. Documentation accurately distinguishes current bootstrap state from long-term runtime goals.
7. A complete first-PR body is available locally with validation and convention-review evidence;
   the working tree remains unstaged and uncommitted until separately authorized.
