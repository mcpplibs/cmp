# CMP CI 冷缓存工具链修复计划

**日期：** 2026-08-30
**设计：** `docs/superpowers/specs/2026-08-30-cmp-ci-cold-cache-toolchain-design.md`
**状态：** 完成

## 执行规则

只处理 PR #10 已证实的冷缓存工具链失配。每一步先验证再继续；不修改 CMP 源码、依赖、
编译器版本或未失败的 workflow 引导流程。

## 任务

- [x] 核对 PR #10 三平台结果与 Linux 失败步骤。
- [x] 核对 mcpp 官方修复提交、版本边界与 xlings 版本下限。
- [x] 自审设计：发现 mcpp 沙箱会复制 workflow 的 xlings，纠正为同步提升两个 pin。
- [x] 将 `.xlings.json` 的 mcpp 固定为 `2026.8.28.1`。
- [x] 将三个 workflow 的引导 xlings 固定为 `2026.8.27.5`，不改执行流程。
- [x] 安装项目工具并确认实际 mcpp 版本。
- [x] 运行 Dev/Release 严格无缓存构建和完整测试：两套均为 140/140。
- [x] 运行 `examples/basic`，全部既有输出与退出状态正确。
- [x] 自审最终差异并更新 HANDOFF；纠正 workflow 中已过时的版本下限注释。
- [x] 本地提交并推送当前分支：`7c81a10 修复 CI 冷缓存工具链`。
- [x] PR #10 的 Linux、macOS、Windows 检查全部通过。
- [x] 根据实际远程结果同步项目当前状态文档。
