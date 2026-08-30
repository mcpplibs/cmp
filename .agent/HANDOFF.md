# CMP 当前工作状态

## 项目概览

CMP 是使用 mcpp 构建的 C++23 Modules 协程运行时库，公开根模块为 `mcpplibs.cmp`。
当前 v1 能力包括 `Task<T>`、`RunLoop`、定时与协作取消、`when_all()`、`TaskGroup`、
异步事件与互斥、固定大小 `ThreadPool`、`run_blocking()`，以及单 driver `IoContext`、
数值地址 `TcpStream`/`TcpListener`。

包版本保持 `0.1.0`，项目固定 mcpp `2026.8.28.1`、LLVM `22.1.8`、Asio `1.38.1`、
gtest `1.15.2`。DNS、TLS、UDP、detached task、自动取消传播、work stealing 与通用 I/O
不属于当前 v1 契约。

## 当前目标与状态

Phase 6C PR #11 已在 Linux x86_64、macOS arm64、Windows x86_64 全部通过后授权合并；
`main` 的对应 squash commit 为 `02cb6b9`。本地已从该提交创建
`release/phase7-v1-release-qualification`。

Phase 7 是最后一个已规划的 v1 阶段，只做发布资格验收，不增加运行时 API。当前已完成
本地实现、测试、readiness、早期源码归档和 Form A 描述符预检；Phase 7 项目 PR、远程
三平台 CI、干净合并候选归档和公开发布仍未完成。验收门禁提交为 `941e42c`
（`完善 v1 发布验收门禁`），当前分支已推送并跟踪
`origin/release/phase7-v1-release-qualification`。

Phase 7 PR #12 已获授权并创建。首轮 Windows CI 在真正编译测试前因 mcpp 已知 depfile
降级失败；根因修正提交 `b2c0911`（`修正 Windows 发布验收门禁`）已通过 Linux、macOS、
Windows 三平台。PR 当前保持 OPEN、CLEAN、MERGEABLE，尚未获得合并授权。

用户已授权当前持续目标内的 Git commit 与 push。PR #12 的修改、PR 合并、tag、GitHub
Release、mcpp-index PR 和 index 合并仍必须按各自远程门禁单独授权。

## 本阶段修改

- 仅加强现有 `.github/workflows/ci-linux.yml`、`ci-macos.yml`、`ci-windows.yml`：
  - Linux/macOS 根包运行严格、关闭缓存的 Dev/Release build 和 test；
  - Windows 先运行严格探针，只允许已知 depfile 降级，再执行无 `--strict` 的同配置无缓存
    完整 build/test；
  - 独立 example 运行一次，并增加对应平台的无缓存 Release build。
- 三种 README 与三种架构文档同步相同的本地验证命令和真实发布状态。
- Phase 7 Design/Plan 记录已完成的本地证据和仍待完成的远程门禁。
- 未修改运行时 API、源码、测试、版本、依赖或打包实现。

## 本地验证

- 本机 Linux 根包 Dev：`mcpp build --profile dev --strict --cache=off` 通过；
  `mcpp test --profile dev --strict --cache=off` 为 10 个二进制、161/161 通过。
- 根包 Release：`mcpp build --profile release --strict --cache=off` 通过；
  `mcpp test --profile release --strict --cache=off` 为 10 个二进制、161/161 通过。
- `examples/basic`：`mcpp run` 输出预期十行并退出 0；严格无缓存 Release build 通过。
- readiness Release 连续五轮全部 PASS，每轮硬计数均为：
  - compute：49,000 成功 / 1,000 预期失败 / 0 非预期失败；
  - file I/O：1,000 / 100 / 0；
  - network：20,000 / 100 / 0。
- 五轮中位数：compute `38.475 ms / 1,299,546.0 ops/s`，file I/O
  `217.363 ms / 5,060.7 ops/s`，network `2,622.017 ms / 7,665.9 ops/s`。
- 三个 workflow 均通过 PyYAML 解析，并包含一致的发布资格流程。
- `mcpp publish --dry-run --allow-dirty` 基于已提交的 Phase 6C 合并基线生成早期归档：
  `cmp-0.1.0.tar.gz`，206,341 bytes，临时 SHA-256
  `fe36976fe76225f6121cce81ca22a1b2392a833795795fb9a00ed52d98dba4de`。
- 临时描述符规范化为 namespace `mcpplibs`、原子包名 `cmp`、无内联 `mcpp` 段的 Form A
  后，`mcpp xpkg parse` 报告三平台 `0.1.0` 和 `parse OK`。
- 解压后的早期源码归档使用已安装的 mcpp `2026.8.28.1` 完成 Dev/Release 严格无缓存
  构建和 161/161 测试；归档内 example 运行和 Release build 也通过。
- 本阶段没有源码或测试变更，按 Phase 7 规则无需新增 analyzer 检查。
- 最终本地自审确认仅三个现有 workflow 与发布状态文档发生变化；三平台命令一致性、
  YAML 结构、CRLF 和 `git diff --check` 均通过。
- Windows 严格探针通过 Bash 语法检查；在本机严格构建成功路径退出 0，合成日志验证精确
  已知降级会放行、增加任意 warning 会失败，且临时日志由 trap 清理。
- PR #12 的 `b2c0911` 三平台检查全部通过：Linux 2m53s、macOS 3m08s、Windows 5m22s。
  完整日志确认每个平台的 Dev/Release 各为 10 个二进制、161/161，example 均输出预期十行
  并完成 Release build；Windows 探针只出现已知 depfile warning 与对应 strict 汇总。

## 已知限制

- 早期归档只包含已提交的 `02cb6b9`，不包含当前未提交的 Phase 7 workflow/文档变更；
  正式 SHA 必须在 Phase 7 合并并同步干净 `main` 后重新生成。
- mcpp `2026.8.28.1` 自动描述符的内联 `mcpp` 段缺少 `sources`，不能直接解析；index
  候选必须使用已验证的 Form A，且无真实 CN 镜像时保持普通字符串 URL。
- 本机 `~/.mcpp` 的 vendored xlings `2026.8.11.2` 较旧且缺少 `subos_info`，因此显示
  runtime facts inconclusive 警告；构建、测试、示例和 readiness 均实际通过，未修改用户
  全局环境。离开项目环境做归档验证时使用已安装的 mcpp `2026.8.28.1` 绝对路径。
- readiness 性能仅是本机诊断数据，不是 CI 阈值或 SLA。

## 重要文件

- Phase 7 Design：
  `docs/superpowers/specs/2026-08-30-cmp-phase7-v1-release-qualification-design.md`
- Phase 7 Plan：
  `docs/superpowers/plans/2026-08-30-cmp-phase7-v1-release-qualification.md`
- CI：`.github/workflows/ci-linux.yml`、`ci-macos.yml`、`ci-windows.yml`
- 用户文档：`README.md`、`README.zh.md`、`README.zh.hant.md`
- 架构文档：`docs/architecture.md`、`docs/architecture.zh.md`、
  `docs/architecture.zh.hant.md`

## 剩余工作

1. PR 描述仍写着三平台统一 `--strict`；按 Git 安全规则，修正 PR 描述需要用户另行授权。
2. 合并前复核 PR #12 最新 HEAD 的三平台检查；获得单独授权后合并，同步干净 `main`，
   无 `--allow-dirty` 重新生成并验证正式归档。
3. tag、GitHub Release、mcpp-index PR、index 合并和最终独立 consumer 验证按 Design 的
   独立授权门禁逐项执行。

## 推荐下一步

停在 PR #12 描述修改门禁；不自行修改或合并 PR。
