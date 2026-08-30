# CMP 当前工作状态

## 项目概览

CMP 是使用 mcpp 构建的 C++23 Modules 协程运行时库，公开模块为 `mcpplibs.cmp`。
v1 当前包含：

- 懒启动、单消费者 `Task<T>`；
- 变参/vector `when_all()` 与静止点 `TaskGroup`；
- `OneShotEvent`、`AsyncManualResetEvent`、RAII `AsyncMutex`；
- 带定时和 `std::stop_token` 取消的调用线程 `RunLoop`；
- 固定大小 `ThreadPool` 与结构化同步调用隔离 `run_blocking()`；
- 单私有 I/O driver 的 `IoContext`、数值地址 TCP client `TcpStream`，以及数值地址
  TCP server `TcpListener`。

项目固定 mcpp `2026.8.28.1`、LLVM `22.1.8`、Asio `1.38.1` 与 gtest `1.15.2`。
三平台 CI 引导 xlings 固定为 `2026.8.27.5`。`examples/basic` 是独立 path-dependency
consumer。

## 当前目标与状态

PR #10 已合并，`main` 已同步到 squash commit `9492075`。当前分支为
`feature/phase6c-tcp-server-v1`。Phase 6C TCP Server v1 已通过全部本地门禁，核心实现提交
为 `e0248ca`（`实现协程 TCP 服务端`），当前分支已推送至 `origin`。

Phase 7 是最后一个已规划的 v1 阶段，只做发布验收，不增加运行时 API。它的 Design/Plan
已完成复核，并已执行不依赖远程权限的早期包转换预检；正式实施仍等待 Phase 6C 合并后
切换到独立分支。

用户已授权当前持续目标内的 Git commit 与 push。Phase 6C PR #11 已创建，Linux、macOS、
Windows CI 均通过；合并仍需用户另行明确授权。

## 已完成工作

- 在现有 `mcpplibs.cmp:tcp` 分区增加 move-only `TcpListener`：
  - `bind(IoContext&, Scheduler, numericAddress, port, stop_token)`；
  - `accept(Scheduler, stop_token)`；
  - `close()`、`is_open()`、`local_port()`。
- bind/accept 保持懒执行；每个成功或错误都尝试通过调用方选择的返回 Scheduler 发布。
- 支持数值 IPv4/IPv6、端口零、系统最大 backlog、一个 pending accept、取消后复用、
  线程安全幂等 close，以及 listener 与已接受 `TcpStream` 的独立所有权。
- `IoContext` 增加私有 listener 弱引用 registry；shutdown 先关闭 listener，再关闭
  stream，并排空 native handler 后 join driver。
- accept 候选 socket 在 native initiation 前登记，并在返回 Scheduler 后复核待发布资源；
  自审同时纠正 bind/connect/accept 的 context shutdown 发布窗口，关闭获胜时返回
  `OperationCancelled`，不会发布已关闭对象。
- read/write/accept 的 pending 标志是唯一重叠门禁；driver 可覆盖已完成 operation 的弱引用，
  不再错误依赖上一 Task 帧已经销毁。
- 增加 21 项 Phase 6C TCP 测试；`tcp_test` 现为 45 项，完整套件为 10 个二进制、161 项。
- readiness 的正常 TCP server 已从阻塞 POSIX socket 迁移到
  `TcpListener`/`TcpStream`；POSIX 只保留未监听失败端口构造。
- 三种 README、三种架构文档、Phase 6C Design/Plan 和 readiness 报告已同步。
- 新增并复核 Phase 7 v1 发布验收 Design/Plan，明确三平台双配置 CI、独立 consumer、
  源码归档、mcpp-index Form A、GitHub Release 与各远程授权边界；Phase 7 后不自动规划
  Phase 8。
- 预检纠正两项 mcpp `2026.8.28.1` 行为假设：生成的描述符不能直接作为 index 文件，
  `--allow-dirty` 也不会把未提交工作树内容装入源码归档。

## 重要决策

- 只提供数值地址 bind；不隐藏阻塞 DNS。
- port 0 交由系统选择，`local_port()` 返回实际端口；close 后保留最后绑定端口，
  moved-from 返回 0。
- v1 使用系统最大 backlog，不隐式设置 `reuse_address`，不开放任意 socket option。
- 同一 listener 只允许一个 accepted-but-not-yet-published accept；连续服务由普通 accept
  循环与结构化 handler 组合。
- pre/active accept cancellation 保持 listener 可用；listener close 或 context shutdown
  获胜时 pending accept 恰好以 `OperationCancelled` 完成一次。
- 继续使用一个私有 I/O driver 和两类线性弱引用 registry；只有基准证明瓶颈后才扩展。
- 不增加 DNS、TLS、UDP、endpoint 类型、超时、连接配额、detached handler、通用 I/O
  抽象或新依赖。

## 路线完整性审计

- 从 bootstrap、RunLoop、timer、cancellation、`when_all`、TaskGroup、events、AsyncMutex，
  到 Phase 4–7 及 CI 冷缓存修复，共有 16 组逻辑 Design/Plan；已逐一核对，全部成对存在。
- Phase 4 已完成 v1 基础契约和 readiness；Phase 5 已完成 ThreadPool；Phase 6A 已完成
  blocking offload；Phase 6B 已完成 TCP client，并由 PR #10 三平台验证。
- Phase 6C TCP server 的本地实现、161 项门禁和 PR #11 三平台 CI 均已完成，只待授权合并。
- Phase 7 是最后一个计划阶段，仅做发布资格、三平台双配置、干净归档、GitHub Release 和
  mcpp-index 验收。没有遗漏的已规划 Phase 8；未来能力必须由真实需求启动独立 Design。

## 修改 / 重要文件

- 实现：`src/tcp.cppm`
- 测试：`tests/tcp_test.cpp`
- readiness consumer：`benchmarks/v1-readiness/src/main.cpp`
- readiness 数据：`docs/benchmarks/2026-08-29-cmp-v1-readiness.md`
- Design：
  `docs/superpowers/specs/2026-08-30-cmp-phase6c-tcp-server-v1-design.md`
- Plan：
  `docs/superpowers/plans/2026-08-30-cmp-phase6c-tcp-server-v1.md`
- Phase 7 Design：
  `docs/superpowers/specs/2026-08-30-cmp-phase7-v1-release-qualification-design.md`
- Phase 7 Plan：
  `docs/superpowers/plans/2026-08-30-cmp-phase7-v1-release-qualification.md`
- 用户文档：`README.md`、`README.zh.md`、`README.zh.hant.md`
- 架构文档：`docs/architecture.md`、`docs/architecture.zh.md`、
  `docs/architecture.zh.hant.md`
- 当前状态：`.agent/HANDOFF.md`

## 验证情况

- `mcpp build --profile dev --strict --cache=off`：通过，LLVM `22.1.8`。
- `mcpp test --profile dev --strict --cache=off`：10 个二进制、161/161 通过。
- `mcpp build --profile release --strict --cache=off`：通过。
- `mcpp test --profile release --strict --cache=off`：10 个二进制、161/161 通过。
- `mcpp build --profile dist --strict --cache=off`：通过。
- focused Release 发布窗口门禁连续 20 轮通过：bind、connect、accept 在返回 Scheduler
  阻塞期间遇到 context shutdown 均返回取消，未发布已关闭资源。
- focused Release 原生 operation 复用门禁连续 5 轮、25 项执行全部通过，覆盖
  read/write/accept 的重叠拒绝与取消后复用。
- Release `tcp_test` 使用随机顺序 seed `60830` 运行，45/45 通过，未发现用例顺序依赖。
- 静态可移植性扫描确认 `src/`、`tests/`、`examples/` 未引入平台 socket 头或全局
  POSIX socket 调用；此类调用只存在于明确标注 POSIX-only 的 readiness consumer。
- 基于 mcpp 生成的 26 条模块编译数据库运行 `clang-analyzer-*` 检查
  `src/tcp.cppm` 与 `tests/tcp_test.cpp`：两者退出码均为 0、无诊断。
- stream pending read 与 listener pending accept 的并发 close 门禁在 Release 连续 100 轮
  通过：合计 800 次并发 `close()` 均保持幂等，两类操作每轮都只取消完成一次。
- focused Release listener 竞态连续 5 轮通过：共覆盖 100 次 close/accept 与 500 次
  stop/accept，均恰好产生一个合法结果。
- focused Release 生命周期门禁连续 20 轮、共 100 项执行全部通过，覆盖 ThreadPool 返回、
  失效返回 Scheduler、主动取消后复用、context shutdown 与 32 客户端负载。
- 额外尝试的 Release TCP 全套 50 轮压力并未作为通过项：前 11 轮 43/43 全通过，随后本机
  仅 `60700-61000` 的临时端口范围被短连接 TIME_WAIT 占满，port-0 bind 开始返回
  `Address already in use`；现场有 2,333 个 TIME_WAIT。未修改系统参数，也未让库隐式开启
  `reuse_address` 来掩盖资源耗尽。
- TIME_WAIT 自然回落到 2 后，正常单轮 Release TCP 套件立即恢复为 43/43 通过，确认该失败
  来自宿主端口资源压力，而非持续 socket 泄漏或实现状态损坏。
- `examples/basic` 的 `mcpp run`：通过，既有十行输出正确，退出码 0。
- `examples/basic` 的 `dist` 构建及生成二进制运行：通过，同样输出正确十行，退出码 0。
- readiness Release 严格无缓存构建通过；最终 5 轮共 15 行场景全部 PASS，所有轮次硬计数均为：
  - compute：49,000 成功 / 1,000 预期失败 / 0 非预期失败；
  - file I/O：1,000 成功 / 100 预期失败 / 0 非预期失败；
  - network：20,000 成功 / 100 预期失败 / 0 非预期失败。
- 最终中位数：compute `25.443 ms / 1,965,140.8 ops/s`，file I/O
  `147.661 ms / 7,449.5 ops/s`，native async TCP client/server
  `2,239.147 ms / 8,976.6 ops/s`。
- Phase 6C 当前工作树再次执行 Dev/Release 严格无缓存构建与测试，均为 10 个二进制、
  161/161 通过；`src/tcp.cppm` 与 `tests/tcp_test.cpp` 的 `clang-analyzer-*` 再次退出 0、
  无诊断；独立 example 再次正确输出十行并退出 0。
- 当前 readiness 二进制重新连续运行 5 轮，所有轮次仍保持 compute
  `49,000/1,000/0`、file I/O `1,000/100/0`、network `20,000/100/0` 的
  成功/预期失败/非预期失败硬计数。
- `mcpp publish --dry-run --allow-dirty` 成功生成早期归档
  `cmp-0.1.0.tar.gz`（184,046 bytes，SHA-256
  `08e467dfe45cfdddde3664be7223995b56c7e172a6c6e568ce7ae9f3da4db062`）与描述符。
  该 SHA 只属于当前已提交基线，Phase 6C 合并后的最终归档必须重新生成。
- 生成描述符直接解析会因内联 `mcpp` 段缺少 `sources` 失败；临时候选改为短包名
  `cmp` 并移除内联 `mcpp` 段后，`mcpp xpkg parse` 明确报告 Form A、三平台
  `0.1.0`、`parse OK`。
- 早期源码归档不含 `TcpListener`，确认 `--allow-dirty` 未装入 Phase 6C。归档对应的
  Phase 6B 基线使用已安装的 mcpp `2026.8.28.1` 在隔离目录完成 Dev/Release 构建，
  两套均为 10 个测试二进制、140/140 通过；归档内 example 的运行和 Release 构建也通过。
- PR #11 的 Linux x86_64、macOS arm64、Windows x86_64 检查均通过；首轮耗时分别为
  2m43s、2m45s、2m49s。

## 已知问题 / 风险

- 本机 `~/.mcpp` 仍保存旧 vendored xlings `2026.8.11.2` 且缺少 `subos_info`；
  mcpp 提示 runtime facts inconclusive，但当前 LLVM 双配置构建、测试、示例和 benchmark
  均成功。本阶段未修改用户全局环境。
- 单 I/O driver 是 v1 简化；当前 readiness 只提供本机回归数据，不是 SLA。
- 本机 IPv4 临时端口范围只有 301 个端口；不要连续重复完整 TCP 套件 50 轮。当前 5 轮竞态与
  20 轮定向生命周期门禁是避免耗尽宿主资源的有界压力门。
- bind 取消只在 driver setup 开始前协作生效；短同步 open/bind/listen 开始后由完成获胜。
- 内置 RunLoop/ThreadPool 返回 Scheduler 会离开 I/O driver；自定义 Scheduler 必须自行提供
  所需的线程转移语义，inline 实现不得在 driver continuation 中析构 `IoContext`。
- listener 不提供 DNS、TLS、backlog 配置、socket option、多 pending accept 或自动 handler
  所有权；这些都需要具体 consumer 与独立设计。
- 运行中的 blocking callable 仍不可抢占；ThreadPool 仍为无界共享 FIFO。
- 离开项目本地 `.xlings` 环境后，普通 xlings shim 目前只激活 mcpp `2026.8.11.2`，
  无法解析 `.xlings.json` 要求的 `2026.8.28.1`；已安装的 `2026.8.28.1` 实际二进制可用，
  归档隔离构建用它完成。不要把本地 `.xlings` 目录塞进发布包；正式 CI/index consumer
  应按固定版本安装工具。
- mcpp `2026.8.28.1` 自动描述符的内联 `mcpp` 段缺少 `sources`，且生成名是兼容全名；
  index 候选必须规范化为短名 `cmp` 的 Form A。没有真实 CN 镜像时保留普通字符串 URL。

## 剩余工作

1. 用户审查三平台全绿的 PR #11，并明确授权后再合并。
2. 合并后同步 `main`、新建 Phase 7 分支，按 Design/Plan 加强三平台 Dev/Release CI，
   并从干净候选重跑 161 项、example、readiness、归档及 Form A 验收。
3. tag、GitHub Release、mcpp-index PR 与 index 合并均是独立远程操作，逐项等待授权。

## 推荐下一步

由用户审查 PR #11 并确认是否授权合并；在此之前不执行 merge。
