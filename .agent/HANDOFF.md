# CMP 当前工作状态

## 项目概览

CMP 是使用 mcpp 构建的 C++23 Modules 协程运行时库，公开模块为 `mcpplibs.cmp`。当前已实现
懒启动单消费者 `Task<T>`、变参/vector `when_all()`、静止点 `TaskGroup`、一次性与可复用
事件、RAII `AsyncMutex`、带定时和取消的调用线程 `RunLoop`、固定大小的 `ThreadPool`，以及
用于隔离同步调用的 `run_blocking()`。

`.xlings.json` 固定 mcpp 2026.8.11.2；当前工具链为 LLVM 22.1.8，测试依赖为
`compat.gtest` 1.15.2。`examples/basic` 是独立 path-dependency consumer。

## 当前目标与状态

Phase 6A blocking offload v1 已完成本地实现、文档同步和验证；远程尚未更新。原生异步 I/O
不属于 6A，下一项设计工作是 Phase 6B Spec。

## 已完成工作

- 新增 `mcpplibs.cmp:blocking` 分区并由根模块导出一个公共函数模板：
  `run_blocking(blockingWorkers, returnTo, operation, stopToken)`。
- helper 按值持有 callable，支持 `void`、可移动值、move-only callable/result 和异常传播；
  callable 在指定 ThreadPool 上执行一次，结果只在显式返回 Scheduler 上发布。
- 复用现有 `ThreadPool` 的调度、取消、队列和关闭契约；没有新增 `BlockingPool`、隐藏全局
  executor、future、类型擦除、依赖或 manifest 配置。
- 新增 11 项确定性测试，覆盖懒启动、worker/返回线程亲和、返回另一 ThreadPool、值/void、
  move-only、异常、预取消/排队取消/晚取消、取消竞态、过期与 inactive Scheduler、RunLoop
  响应性，以及 5,000 个并发 offload 的 exactly-once 行为。
- `examples/basic` 新增独立 blocking ThreadPool，并在协程中打印 `Blocking result: 42`。
- `benchmarks/v1-readiness` 的文件与回环网络场景已从手写 `jthread` adapter 迁移到一个专用
  CMP ThreadPool 和 `run_blocking()`；负载及成功/失败硬检查保持不变。
- 三份 README、三份架构文档、Phase 6A Design/Plan 和 v1 readiness 数据报告已同步到实现
  事实。

## 重要决策

- 独立的普通 `ThreadPool` 实例就是 blocking pool；不维护行为相同的第二种线程池类型。
- `run_blocking()` 是懒 `Task`，callable 和两个 Scheduler 都按值进入协程帧，避免临时对象
  悬空。
- 第一次调度接受可选 `std::stop_token`；取消只能跳过尚未被 worker claim 的 callable。
  已经开始的同步调用不能被抢占。
- 返回调度不接受取消 token，确保值、异常或取消结果不会滞留在 blocking worker。
- 返回 Scheduler 失败时直接传播其异常；失效的完成上下文本身无法被 helper 修复。
- Phase 6A 仍是每个运行中同步调用占用一个系统线程的隔离方案，不宣称原生非阻塞 I/O。
- 队列沿用 ThreadPool 的无界共享 FIFO；背压、超时、强制中断和 worker replacement 均未在
  没有实测需求前增加。

## 修改 / 重要文件

- 核心：`src/blocking.cppm`、`src/cmp.cppm`
- 测试：`tests/blocking_test.cpp`
- 示例：`examples/basic/src/main.cpp`
- 压测：`benchmarks/v1-readiness/src/main.cpp`、
  `docs/benchmarks/2026-08-29-cmp-v1-readiness.md`
- 方案：`docs/superpowers/specs/2026-08-29-cmp-phase6a-blocking-offload-v1-design.md`、
  `docs/superpowers/plans/2026-08-29-cmp-phase6a-blocking-offload-v1.md`
- 公共文档：`README.md`、`README.zh.md`、`README.zh.hant.md`、`docs/architecture.md`、
  `docs/architecture.zh.md`、`docs/architecture.zh.hant.md`

## 验证情况

- `mcpp build --profile dev --strict --cache=off`：通过。
- `mcpp test --profile dev --strict --cache=off`：9 个二进制、116/116 通过。
- `mcpp build --profile release --strict --cache=off`：通过。
- `mcpp test --profile release --strict --cache=off`：9 个二进制、116/116 通过。
- Dev 定向 `blocking_test`：11/11 通过。
- Release `CancellationRaceInvokesAtMostOnce` 连续执行 100 轮：100/100 通过。
- `examples/basic` 的 `mcpp run`：通过，包含 `Blocking result: 42`，退出码 0。
- `benchmarks/v1-readiness` Release strict 构建通过；迁移后执行 5 轮，compute、file_io 和
  network_loopback 每轮均 PASS，五轮非预期失败总数为 0。
- 文件/网络每轮计数分别为 1,000/20,000 成功、100/100 预期失败、0 非预期失败；原始耗时和
  吞吐已写入 benchmark 报告。
- 当前 mcpp 仍输出 SubOS 缺少 `subos_info` 的既有环境提示，但所有构建和运行成功。
- 以上均为本机 Linux/WSL2 结果；尚未执行 GitHub 三平台 CI 或其他远程操作。

## 已知问题 / 风险

- 运行中的同步调用不可抢占；永久阻塞会永久占用 worker，并使等待它的 Task 和 pool 析构
  无法完成。callable 如需协作取消，必须自行捕获并检查 token。
- ThreadPool 使用无界共享 FIFO；持续生产快于消费时，应用需要限制自己的结构化 in-flight
  数量。
- 返回 Scheduler 的 owner 必须持续存活；RunLoop Scheduler 还必须处于 active `run()` 中。
- `run_blocking()` 是线程隔离，不是 epoll、io_uring、kqueue 或 IOCP 等原生异步 I/O。
- 三平台兼容性仍需远程 CI 确认。

## 剩余工作

1. 用户审查 Phase 6A 本地改动；Commit、Push、PR 与 CI 均需分别获得明确授权。
2. 用户确认进入下一设计阶段后，为一个窄资源族编写 Phase 6B 原生异步 I/O Spec；实现前仍需
   单独批准。

## 推荐下一步

先审查并提交 Phase 6A；随后只设计 Phase 6B 的最小原生异步 I/O 边界，优先选择单一资源族，
不把文件、网络和所有平台后端一次性绑定在同一阶段。
