# CMP Bootstrap Implementation Plan

**Status:** Complete; merged via PR #1

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [x]`) syntax for tracking.

**Goal:** Replace the generic `mcpplibs.mylib` scaffold with an import-only CMP C++23 module project, remove premature publishing templates, clean local clice artifacts, and leave a fully verified but uncommitted working tree.

**Architecture:** Keep one empty root module, one import smoke test, and one standalone path-dependency consumer. Documentation describes CMP's intended runtime direction without implementing or promising runtime behavior; mcpp remains the only build system.

**Tech Stack:** C++23 Modules, mcpp 2026.8.1.1, xlings, LLVM 20.1.7, gtest 1.15.2, GitHub Actions, PowerShell, Git Bash.

---

## Execution Constraints

- Execute inline in the current session; do not dispatch subagents.
- Continue on the existing `codex/bootstrap-cmp` branch.
- Do not create or switch branches.
- Do not stage, commit, push, or create a pull request.
- Preserve unrelated local files and settings.
- Use Windows Git (`E:\Git\cmd\git.exe`) as the authority for this worktree.

## File Map

### Create

- `src/cmp.cppm` — import-only CMP root module.
- `tests/cmp_test.cpp` — import smoke test.
- `docs/superpowers/specs/2026-08-02-cmp-bootstrap-design.md` — approved design.
- `docs/superpowers/plans/2026-08-02-cmp-bootstrap.md` — this execution plan.

### Create locally (ignored)

- `.rpiv/artifacts/pr-drafts/2026-08-02-cmp-bootstrap.md` — proposed first PR title and body;
  retained locally until PR creation is separately authorized.

### Modify

- `.gitignore` — generated/tool-state ignore policy.
- `mcpp.toml` — CMP package identity and explicit C++23 baseline.
- `examples/basic/mcpp.toml` — standalone CMP consumer identity and dependency.
- `examples/basic/src/main.cpp` — import-only external consumer.
- `README.md`, `README.zh.md`, `README.zh.hant.md` — product-oriented entry points.
- `docs/architecture.md`, `docs/architecture.zh.md`, `docs/architecture.zh.hant.md` — current architecture and boundaries.
- `.github/workflows/ci-linux.yml`, `.github/workflows/ci-macos.yml`, `.github/workflows/ci-windows.yml` — remove publishing-template smoke steps and stale comments.
- `.agents/skills/more-details/SKILL.md` — CMP-specific repository navigation.
- `.agents/skills/README.md` — remove template-specific repository wording if present.
- `.git/info/exclude` — remove rules made redundant by tracked `.gitignore`.
- `C:\Users\80945\AppData\Roaming\Code\User\settings.json` — remove only the stale clice formatter association.

### Delete

- `src/mylib.cppm`
- `tests/mylib_test.cpp`
- `templates/basic/.gitignore`
- `templates/basic/template.toml`
- `templates/basic/mcpp.toml.in`
- `templates/basic/src/main.cpp.in`
- `templates/lib/template.toml`
- `templates/lib/mcpp.toml.in`
- `templates/lib/src/lib.cppm.in`
- `templates/lib/tests/lib_test.cpp.in`
- `tools/template_smoke.sh`

## Task 1: Finish Local Windows Cleanup

**Files:**
- Modify: `C:\Users\80945\AppData\Roaming\Code\User\settings.json`
- Delete locally: the verified clice/cache directories listed below

- [x] **Step 1: Verify process and directory targets**

Run:

```powershell
Get-Process -Name clice,clangd -ErrorAction SilentlyContinue |
    Select-Object ProcessName,Id,Path

$cleanupPaths = @(
    'E:\CPPCodes\mcpplibs\cmp\.clice',
    'C:\Users\80945\AppData\Local\clice-mcpp\E__CPPCodes_mcpplibs_cmp',
    'C:\Users\80945\AppData\Local\Temp\cmp-clice-eval',
    'E:\CPPCodes\mcpplibs\cmp\.cache',
    'E:\CPPCodes\mcpplibs\cmp\examples\basic\.cache'
)
$cleanupPaths | ForEach-Object {
    [pscustomobject]@{ Path = $_; Exists = Test-Path -LiteralPath $_ }
}
```

Expected: no clice process; clangd path ends in `llvm-tools\20.1.7\bin\clangd.exe`; every
existing path resolves exactly to one entry in `$cleanupPaths`.

- [x] **Step 2: Delete only verified local experiment/cache directories**

For each existing path, resolve it and require exact equality before deletion:

```powershell
$cleanupPaths | ForEach-Object {
    if (Test-Path -LiteralPath $_) {
        $resolvedCleanupPath = (Resolve-Path -LiteralPath $_).Path
        if ($resolvedCleanupPath -ne $_) {
            throw "Unexpected cleanup target: $resolvedCleanupPath"
        }
        Remove-Item -LiteralPath $resolvedCleanupPath -Recurse -Force
    }
}
```

Expected: both clice caches and both clangd cache directories are absent; the already-absent
repository `.clice` directory remains absent. These caches are not recoverable but are generated
and can be rebuilt.

- [x] **Step 3: Remove the stale clice formatter association**

Use `apply_patch` to remove exactly this otherwise-empty object from VS Code user settings:

```json
"[cpp]": {
    "editor.defaultFormatter": "clice-io.clice"
},
```

Preserve `"C_Cpp.intelliSenseEngine": "disabled"` and all unrelated settings. Validate that
the JSONC structure still has correct commas around `"go.alternateTools"` and
`"gitlens.ai.model"`.

- [x] **Step 4: Verify local cleanup**

Run the Step 1 checks again plus:

```powershell
rg -n 'clice-io\.clice' 'C:\Users\80945\AppData\Roaming\Code\User\settings.json'
```

Expected: no directory exists, no clice process exists, and `rg` returns no formatter match.

## Task 2: Verify the Original Baseline

**Files:** none

- [x] **Step 1: Confirm pinned tools and environment**

Run from `E:\CPPCodes\mcpplibs\cmp`:

```powershell
mcpp --version
mcpp self doctor
```

Expected: mcpp `2026.8.1.1`; doctor reports all checks passed.

- [x] **Step 2: Run the untouched scaffold tests without cache**

```powershell
mcpp test --no-cache
```

Expected: the existing `MyLibTest.HelloMcpplibs` test passes before tracked identity changes.

## Task 3: Establish a Failing CMP Import Test

**Files:**
- Create: `tests/cmp_test.cpp`
- Test: `tests/cmp_test.cpp`

- [x] **Step 1: Add the desired import smoke test before the CMP module exists**

Create with `apply_patch`:

```cpp
#include <gtest/gtest.h>

import mcpplibs.cmp;

TEST(CmpModuleTest, Imports) {
    SUCCEED();
}
```

- [x] **Step 2: Run the test suite and verify the intended RED state**

```powershell
mcpp test --no-cache
```

Expected: failure because `mcpplibs.cmp` is not provided yet. The failure must be module
resolution/compilation, not malformed gtest syntax.

## Task 4: Implement the CMP Package Identity

**Files:**
- Modify: `mcpp.toml`
- Create: `src/cmp.cppm`
- Delete: `src/mylib.cppm`
- Delete: `tests/mylib_test.cpp`
- Test: `tests/cmp_test.cpp`

- [x] **Step 1: Replace root package metadata**

Set `mcpp.toml` to:

```toml
[package]
namespace   = "mcpplibs"
name        = "cmp"
version     = "0.1.0"
standard    = "c++23"
description = "A C++23 coroutine runtime project built with mcpp and C++ Modules"
license     = "Apache-2.0"
authors     = ["mcpplibs"]
repo        = "https://github.com/mcpplibs/cmp"

[dev-dependencies]
gtest = "1.15.2"
```

- [x] **Step 2: Replace the root module**

Create `src/cmp.cppm` with exactly:

```cpp
export module mcpplibs.cmp;
```

Delete `src/mylib.cppm` with `apply_patch`.

- [x] **Step 3: Remove the obsolete test**

Delete `tests/mylib_test.cpp` with `apply_patch`. Keep the RED test from Task 3 unchanged.

- [x] **Step 4: Verify GREEN**

```powershell
mcpp build --cache=off
mcpp test --cache=off
```

Expected: the package is inferred as library `cmp`; `CmpModuleTest.Imports` passes.

## Task 5: Convert the Standalone Consumer

**Files:**
- Modify: `examples/basic/mcpp.toml`
- Modify: `examples/basic/src/main.cpp`

- [x] **Step 1: Replace the example manifest**

Use:

```toml
[package]
name        = "cmp-basic"
version     = "0.1.0"
standard    = "c++23"
description = "Basic consumer for the mcpplibs.cmp module"
license     = "Apache-2.0"

[dependencies.mcpplibs]
cmp = { path = "../.." }
```

- [x] **Step 2: Replace the example source**

Use:

```cpp
import mcpplibs.cmp;

int main() {
    return 0;
}
```

- [x] **Step 3: Run the independent consumer**

```powershell
Push-Location examples\basic
mcpp run
Pop-Location
```

Expected: exit code 0 with no program output.

## Task 6: Normalize Ignore Rules

**Files:**
- Modify: `.gitignore`
- Modify locally: `.git/info/exclude`

- [x] **Step 1: Add repository tool-state ignores**

Append this focused section to `.gitignore` with `apply_patch`:

```gitignore
# Local editor and agent state
.cache/
/.clice/
/.rpiv/
/.vscode/
```

The recursive `.cache/` rule covers root and nested examples; other paths remain root-only.

- [x] **Step 2: Remove redundant local exclude entries**

Use `apply_patch` to remove the local `.vscode/` and `.clice/` comment/rule blocks from
`.git/info/exclude`, leaving its stock comments unchanged.

- [x] **Step 3: Verify ignore behavior**

```powershell
& 'E:\Git\cmd\git.exe' -C 'E:\CPPCodes\mcpplibs\cmp' check-ignore -v `
    '.cache\probe' 'examples\basic\.cache\probe' '.clice\probe' `
    '.rpiv\probe' '.vscode\settings.json'
```

Expected: every probe is ignored by the tracked root `.gitignore`.

## Task 7: Remove Premature Publishing Templates

**Files:**
- Delete: all tracked files under `templates/basic/` and `templates/lib/`
- Delete: `tools/template_smoke.sh`
- Modify: `.github/workflows/ci-linux.yml`
- Modify: `.github/workflows/ci-macos.yml`
- Modify: `.github/workflows/ci-windows.yml`

- [x] **Step 1: Delete the publishing template files and smoke script**

Use `apply_patch` delete operations for every path listed in the File Map. Do not delete
`examples/basic/`.

- [x] **Step 2: Update Linux CI**

Change the opening flow comment to `install → build → test → example`, remove the
`Smoke-test templates/` step, and leave the other commands unchanged.

- [x] **Step 3: Update macOS CI**

Remove the `Smoke-test templates/` step. Keep the arm64 runner and LLVM comments unchanged.

- [x] **Step 4: Update Windows CI**

Change comments that say `template` steps to `example` steps and remove the
`Smoke-test templates/` step. Keep PowerShell installation and Git Bash build/test/example
commands unchanged.

- [x] **Step 5: Confirm no CI references a deleted path**

```powershell
rg -n 'template_smoke|Smoke-test templates|templates/' .github README*.md docs .agents
```

Expected after documentation tasks complete: no project-specific instruction invokes the
deleted script.

## Task 8: Rewrite Product Documentation

**Files:**
- Modify: `README.md`
- Modify: `README.zh.md`
- Modify: `README.zh.hant.md`
- Modify: `docs/architecture.md`
- Modify: `docs/architecture.zh.md`
- Modify: `docs/architecture.zh.hant.md`

- [x] **Step 1: Write the English README as the canonical entry point**

Use these sections and facts:

1. `# CMP` and `> Coroutine Machine Process — a C++23 coroutine runtime project.`
2. C++23, Module, CI, and Apache-2.0 badges targeting `mcpplibs/cmp`.
3. A prominent bootstrap-status note: the package currently exports only `mcpplibs.cmp` and
   has no runtime API.
4. `Why CMP`: standard stackless coroutines, explicit `co_await`, and future incremental work.
5. `Boundaries`: no goroutine equivalence, automatic deblocking, or signal-handler switching.
6. `Quick Start`: `xlings install`, `mcpp build`, `mcpp test`, and `examples/basic`.
7. Current repository layout without `templates/` or `tools/template_smoke.sh`.
8. Development/CI commands, project roadmap, contributing links, and Apache-2.0 license.

- [x] **Step 2: Write the Simplified Chinese README**

Mirror the English structure with the title `CMP` and tagline
`Coroutine Machine Process——一个基于 C++23 的协程运行时项目。` Preserve identical commands,
paths, status, runtime boundaries, roadmap ordering, and repository links.

- [x] **Step 3: Write the Traditional Chinese README**

Mirror the English structure with the title `CMP` and tagline
`Coroutine Machine Process——一個基於 C++23 的協程執行期專案。` Preserve identical commands,
paths, status, runtime boundaries, roadmap ordering, and repository links.

- [x] **Step 4: Rewrite the English architecture document**

Use sections `Current Baseline`, `Package Identity`, `Layout`, `Module Boundary`,
`Standalone Consumer`, `Planned Evolution`, `Runtime Boundaries`, and `Verification`.
State that `mcpplibs::cmp` is reserved for future APIs and the current module deliberately has
no public declarations.

- [x] **Step 5: Rewrite both Chinese architecture documents**

Translate all English architecture facts without adding implementation claims. Keep commands,
module/package identifiers, and the phase order exactly aligned across three languages.

- [x] **Step 6: Check language and repository consistency**

```powershell
rg -n 'mcpplibs/template|hello_mcpplibs|mcpplibs\.mylib|src/mylib|tests/mylib' `
    README.md README.zh.md README.zh.hant.md docs
rg -n 'mcpplibs/cmp|mcpplibs\.cmp|C\+\+23|Apache-2\.0' `
    README.md README.zh.md README.zh.hant.md docs
```

Expected: the first command has no matches except historical wording in the approved design;
the second command confirms all language families carry the approved identity.

## Task 9: Update Repository-Specific Agent Guidance

**Files:**
- Modify: `.agents/skills/more-details/SKILL.md`
- Modify if affected: `.agents/skills/README.md`

- [x] **Step 1: Update the repository navigation map**

In `more-details/SKILL.md`, replace current-project paths with `src/cmp.cppm` and
`tests/cmp_test.cpp`, describe `examples/basic/` as the import-only consumer, and remove claims
that this repository ships `templates/` or `tools/template_smoke.sh`.

- [x] **Step 2: Classify remaining generic `mylib` examples**

Run:

```powershell
rg --hidden -n 'mylib|mcpplibs\.mylib|mcpplibs/template' `
    -g '!.git/**' -g '!.rpiv/**' -g '!docs/superpowers/**'
```

Update every project-specific match. Generic examples in `mcpp/SKILL.md` and
`mcpp-style-ref/SKILL.md` may remain only when they clearly illustrate reusable conventions and
do not claim to describe CMP's current files.

## Task 10: Review Ecosystem Conventions and Prepare the First PR Body

**Files:**
- Modify: `docs/superpowers/specs/2026-08-02-cmp-bootstrap-design.md`
- Modify: `docs/architecture.md`, `docs/architecture.zh.md`, `docs/architecture.zh.hant.md`
- Create locally: `.rpiv/artifacts/pr-drafts/2026-08-02-cmp-bootstrap.md`

- [x] **Step 1: Inspect current primary sources**

Review the default branches of `mcpplibs/template`, `cmdline`, `llmapi`, `imgui-m`, and
`primitives`, plus the current mcpp manifest guide and mcpp-index. Compare repository layout,
package identity, root module placement, tests, examples, CI, publishing templates, lock-file
handling, and ignore rules.

- [x] **Step 2: Check the selected package identity against mcpp-index**

Run `mcpp search cmp` and control queries for known packages. Record the date and result without
publishing or editing the index.

- [x] **Step 3: Record the convention conclusions**

Add the sources, snapshot commits, common conventions, repository-specific exceptions, and
template-smoke rationale to the approved design. Do not present a single repository's optional
choice as an organization-wide rule.

- [x] **Step 4: Refine the architecture references**

Rewrite the three architecture documents as concise technical references. State current behavior
in the present tense, separate unimplemented work, remove one-off environment narrative, and keep
the same section order and facts across languages.

- [x] **Step 5: Draft the first PR description locally**

Write a copy-ready PR body under the ignored `.rpiv/artifacts/pr-drafts/` directory. Include the
identity-only scope, deleted-template relationship, ecosystem evidence, index query, validation,
and non-goals. Do not stage, commit, push, or create the PR.

## Task 11: Final Verification and Uncommitted Handoff

**Files:** all modified files

- [x] **Step 1: Run clean builds and tests**

```powershell
mcpp build --cache=off
mcpp test --cache=off
Push-Location examples\basic
mcpp run --cache=off
Pop-Location
```

Expected: build succeeds, one CMP smoke test passes, and example exits 0.

- [x] **Step 2: Run repository quality checks**

```powershell
& 'E:\Git\cmd\git.exe' -C 'E:\CPPCodes\mcpplibs\cmp' diff --check
& 'E:\Git\cmd\git.exe' -C 'E:\CPPCodes\mcpplibs\cmp' status --short --branch
& 'E:\Git\cmd\git.exe' -C 'E:\CPPCodes\mcpplibs\cmp' diff --stat
```

Expected: no whitespace errors; branch is `codex/bootstrap-cmp`; only intended tracked and
untracked design/plan files appear; local caches and `.rpiv/` do not appear.

- [x] **Step 3: Review the complete diff without staging**

Inspect `git diff -- .` and untracked design/plan files. Verify identity, documentation parity,
CI paths, deletion scope, and absence of runtime implementation. Fix all Critical or Important
issues inline and rerun Step 1 and Step 2.

- [x] **Step 4: Stop with the worktree uncommitted**

Do not run `git add`, `git commit`, `git push`, or any PR command. Report modified/deleted/new
files, validation results, local cleanup performed, and the earlier SSH pull failure.

## Execution Result

- Completed inline on `codex/bootstrap-cmp` without staging or committing.
- Verified the TDD RED state: `mcpplibs.cmp` was missing before the root module was added.
- Verified the GREEN state: `mcpp build --no-cache` and `mcpp test --no-cache` succeeded with
  one passing `CmpModuleTest.Imports` test.
- Verified `examples/basic` resolves the root path dependency and exits with status 0.
- Verified `git diff --check`, ignore rules, documentation fences, and old-identity searches.
- Compared the project with current `mcpplibs/template`, `cmdline`, `llmapi`, `imgui-m`,
  `primitives`, the mcpp manifest guide, and mcpp-index; recorded common conventions separately
  from package-specific choices.
- Confirmed `mcpp search cmp` has no current match and prepared the ignored, copy-ready first PR
  body under `.rpiv/artifacts/pr-drafts/`.
- Rewrote the three architecture references as factual current-state documents and recorded the
  publishing-template and `tools/template_smoke.sh` relationship.
- Re-ran build, test, and example verification with `--cache=off`. A strict diagnostic reported
  only the documented Windows LLVM no-depfile degradation and no manifest schema warning.
- Completed an independent review; after correcting lock-policy and stale template wording, no
  Critical or Important findings remain.
- Left all product changes, this plan, and the approved design document uncommitted as requested.
