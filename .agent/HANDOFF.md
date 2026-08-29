# CMP 当前工作状态

## 项目概览

CMP 是使用 mcpp 构建的 C++23 Modules 协程运行时库，公开模块为 `mcpplibs.cmp`。
v1 当前包含：

- 懒启动、单消费者 `Task<T>`；
- 变参/vector `when_all()` 与静止点 `TaskGroup`；
- `OneShotEvent`、`AsyncManualResetEvent`、RAII `AsyncMutex`；
- 带定时和 `std::stop_token` 取消的调用线程 `RunLoop`；
- 固定大小 `ThreadPool` 与结构化同步调用隔离 `run_blocking()`；
- 单私有 I/O driver 的 `IoContext`，以及数值地址 TCP client `TcpStream`。

项目固定 mcpp `2026.8.28.1`、LLVM `22.1.8`、Asio `1.38.1` 与 gtest `1.15.2`。
三平台 CI 引导 xlings 固定为 `2026.8.27.5`。`examples/basic` 是独立 path-dependency
consumer。

## 当前目标与状态

Phase 4、Phase 5、Phase 6A 和 Phase 6B 第 1–10 项均已本地完成。当前分支为
`feature/phase4-v1-readiness`，PR #10 已创建：

<https://github.com/mcpplibs/cmp/pull/10>

PR 初次检查中，macOS arm64 与 Windows x86_64 已通过；Linux x86_64 在编译 CMP 前因
mcpp/xlings 冷缓存运行时绑定失配失败。专用 Spec/Plan 已完成、自审并通过本地门禁；修复正
交付到 PR #10，新的三平台结果待确认。用户已授权本次远程读取、当前分支推送和 PR 操作，
未授权合并。

## 已完成工作

- Phase 4 补齐定时、取消、结构化并发、事件和互斥原语。
- Phase 5 实现固定大小多 worker `ThreadPool` 并完成基准验证。
- Phase 6A 实现显式 blocking worker 与返回 Scheduler 的 `run_blocking()`。
- Phase 6B 实现一个跨平台原生异步 TCP client 切片：numeric connect、partial read、
  write-all、单读单写并发、取消、关闭、context drain/join、return affinity 和 exactly-once
  完成；未加入 DNS、server、TLS、UDP、文件 I/O、timeout 或隐式全局 executor。
- 当前共有 11 个公开模块分区、10 个测试文件和 140 项测试。
- readiness benchmark 的计算、文件 I/O 与网络回环场景均有成功/预期失败硬计数；网络 client
  使用 `IoContext`/`TcpStream`，同步 server 通过 `run_blocking()` 隔离。
- 审查全部 13 份既有 Spec/Plan 后，已纠正早期状态；Phase 6B 本地任务全部完成。
- 创建 PR #10 并取得首轮远程结果：macOS、Windows 成功；Linux 在 `Build library` 前置
  工具链初始化失败，CMP 源码与测试未开始执行。
- 根因是 mcpp `2026.8.11.2` 携带的旧 xlings 声明 `glibc@2.44`，而当前索引安装
  `2.44.2`。官方 mcpp `2026.8.28.1` 固定 xlings `2026.8.27.5` 并包含版本精化兼容路径。
- 自审时纠正“只提升 mcpp”的初稿：Linux 日志证明 mcpp 冷启动会复制 workflow 的系统
  xlings，因此同时提升 `.xlings.json` 与三个 workflow pin，其他步骤不变。

## 重要决策

- 只修改有证据的工具版本：mcpp `2026.8.28.1`、CI xlings `2026.8.27.5`；不升级到包含
  无关构建规则变化的 mcpp `2026.8.29.1`。
- 不用固定旧 glibc、增加缓存或手工安装载荷来隐藏冷缓存失配。
- LLVM、Asio、gtest、CMP API/源码和 workflow 执行流程保持不变。
- Asio 仅作私有三平台 socket backend，公共 API 不暴露 Asio 类型。
- `IoContext` v1 保持单 driver；只有基准证明瓶颈后才考虑多 driver/strand。
- `run_blocking()` 隔离同步调用，但不宣称可抢占，也不替代原生异步 I/O。

## 修改 / 重要文件

- 工具环境：`.xlings.json`
- CI：`.github/workflows/ci-linux.yml`、`ci-macos.yml`、`ci-windows.yml`
- 当前修复设计：
  `docs/superpowers/specs/2026-08-30-cmp-ci-cold-cache-toolchain-design.md`
- 当前修复计划：
  `docs/superpowers/plans/2026-08-30-cmp-ci-cold-cache-toolchain.md`
- Phase 6B：`src/tcp.cppm`、`tests/tcp_test.cpp`、
  `docs/superpowers/specs/2026-08-29-cmp-phase6b-tcp-client-v1-design.md`、
  `docs/superpowers/plans/2026-08-30-cmp-phase6b-tcp-client-v1.md`
- 当前状态：`.agent/HANDOFF.md`

## 验证情况

- `xlings update && xlings install -y`：成功；`mcpp --version` 为 `2026.8.28.1`。
- `mcpp build --profile dev --strict --cache=off`：通过，LLVM `22.1.8`。
- `mcpp test --profile dev --strict --cache=off`：10 个二进制、140/140 通过。
- `mcpp build --profile release --strict --cache=off`：通过。
- `mcpp test --profile release --strict --cache=off`：10 个二进制、140/140 通过。
- `examples/basic` 的 `mcpp run`：通过，全部十行既有示例输出正确，退出码 0。
- Phase 6B 最终本地竞态门禁：100 次 close/completion 与 500 次 stop/completion 无丢失或
  重复完成。
- readiness Release 五轮共 15 行场景全部 PASS，非预期失败为 0；网络中位数
  2,078.127 ms / 9,672.2 ops/s。
- PR #10 首轮 CI：macOS、Windows 通过；Linux 因旧工具冷缓存失配失败。修复后的远程 CI
  尚未执行。

## 已知问题 / 风险

- 本机 `~/.mcpp` 仍保存旧 xlings `2026.8.11.2` 且缺少 `subos_info`；mcpp
  `2026.8.28.1` 会提示 runtime facts inconclusive，但本地 LLVM 双配置构建、测试与示例
  均成功。没有为本次验证修改用户的全局 xlings；远程 workflow 会在冷环境直接安装
  `2026.8.27.5`。
- 工具版本修复只有在新一轮 Linux 冷缓存 CI 通过后才能视为完成。
- 运行中的 blocking callable 不可抢占；永久阻塞会占用 worker。
- ThreadPool 是无界共享 FIFO；应用需要限制持续生产时的结构化 in-flight 数量。
- 单 I/O driver 是 v1 简化；尚无基准证据要求扩展。

## 剩余工作

1. 将本地修复交付到 PR #10，并等待 Linux、macOS、Windows 新一轮检查全部通过。
2. 按实际远程结果更新 README、Phase 6B/工具修复文档与本 HANDOFF；再次提交、推送并确认
   最终 CI。
3. 不合并 PR #10，除非用户另行明确授权。

## 推荐下一步

提交并推送当前冷缓存工具链修复，然后监控 PR #10 三平台检查；若失败，只处理日志证实的
兼容问题。
