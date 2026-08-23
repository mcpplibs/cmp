# CMP 当前工作状态

## 项目概览

CMP 是使用 mcpp 构建的 C++23 Modules 协程运行时库。当前公开核心包括惰性、唯一所有权的
`Task<T>`，单消费者 `RunLoop`，以及可复制的 `RunLoop::Scheduler`。Scheduler 支持普通
`schedule()`、相对期限 `schedule_after()` 和绝对期限 `schedule_at()`；定时等待可显式接收
`std::stop_token`，取消获胜时抛出 `OperationCancelled`。由 Scheduler 入队的 continuation
均由调用 `RunLoop::run()` 的线程恢复。

项目使用 `mcpplibs.cmp` 模块；`.xlings.json` 固定 mcpp 2026.8.11.2，`mcpp.toml` 固定
LLVM 22.1.8，测试依赖为 `compat.gtest` 1.15.2。实现位于 `src/`，契约测试位于 `tests/`，
`examples/basic` 是独立的 path-dependency 消费示例。

## 当前目标与状态

当前分支为 `main`，已包含 PR #5 的 squash 合并提交 `d7dd418`，本地与远端已同步。
结构化并发汇合 `when_all` v1 已完成设计、实现、契约测试、独立示例、三语公开文档、本地
Dev/Release 验证和三平台 PR CI。远端功能分支保留，下一阶段尚未创建分支。

## 已完成工作

- 导出 `OperationCancelled`，并为相对、绝对定时等待增加 `std::stop_token` 重载；原有无 token
  重载保持不变。
- 使用标准 `std::vector` 堆替换私有 `std::priority_queue`，保留普通 Timer 的 O(log n)
  插入和到期路径。
- 在 awaiter 协程帧内保存最小取消状态，以同步 stop callback 构造竞态、deadline 和取消终态。
- stop callback 只在 RunLoop 互斥量下标记取消、重排 Timer 并通知，不向 ready 队列分配，
  不在 `request_stop()` 线程 inline 恢复用户协程。
- deadline 和取消只有一个终态；已经进入 ready 队列的 deadline 完成不会被较晚停止请求替换。
- 增加预取消、跨线程、RunLoop 线程、非最早 Timer、晚取消、竞态恰好一次、无效 Scheduler、
  无 stop-state token、清理复用和十万次预取消栈安全等测试。
- 独立示例现在从协程内依次打印正常定时、并发汇合和取消结果。
- 已同步 README、架构说明、设计文档和实施计划的英文、简体中文、繁体中文事实。
- 已完成 `when_all` v1 设计：输入 Task 由汇合 awaiter 结构化持有，全部结束后按参数顺序
  返回结果或抛出第一个异常；`void` 映射为 `std::monostate`。
- 已导出 variadic `when_all(Task<Results>...)`；支持零输入、异构与 move-only 结果、void
  占位、同步完成和跨线程完成。
- 新增 8 项汇合契约测试、10 万次同步汇合栈安全压力，以及独立 consumer 的并发结果打印。
- README 与架构说明的英文、简体中文、繁体中文事实已同步到 `when_all` v1。

## 重要决策

- 直接复用 `std::stop_source`、`std::stop_token` 和 `std::stop_callback`，不自造 token/source。
- 仅定时等待增加 token 重载；普通 `schedule()`、Task 隐式传播和结构化并发不属于 v1。
- token 重载即使收到预先请求停止的 token 也始终挂起，并通过 RunLoop 队列恢复。
- 取消通过现有 Task 异常路径传播；调用方可以在协程内捕获 `OperationCancelled`。
- Timer entry 只增加一个非 owning 取消状态指针；外部销毁已发布协程帧仍属于无效使用。
- 取消当前用 O(n) 扫描并重建堆；只有实测为瓶颈后才引入可删除或带索引的堆。
- 不增加后台线程、依赖、公开 Timer 句柄、TaskGroup、detached 或测试专用公共接口。
- `when_all` v1 使用“子任务数 + 1”原子计数哨兵，覆盖同步完成和跨线程完成竞态。
- `when_all` 等待全部子任务收尾后再传播异常，不允许因 fail-fast 提前销毁仍在运行的帧。
- 当前阶段只实现固定参数包汇合；动态 TaskGroup、spawn 和隐式取消传播继续独立设计。

## 修改 / 重要文件

- `src/run_loop.cppm`：公开异常、token 重载、取消状态、回调和显式 Timer 堆。
- `src/when_all.cppm`：join coroutine、原子计数哨兵和 variadic 公开 API。
- `src/cmp.cppm`：导出 `:when_all` 分区。
- `tests/run_loop_test.cpp`：Cancellation v1 契约、边界、竞态和栈安全测试。
- `tests/when_all_test.cpp`：结构化所有权、结果、异常、线程和栈安全测试。
- `examples/basic/src/main.cpp`：协程内正常输出和预取消输出示例。
- `README.md`、`README.zh.md`、`README.zh.hant.md`：当前 API、示例和路线图。
- `docs/architecture.md`、`docs/architecture.zh.md`、`docs/architecture.zh.hant.md`：实现契约、
  边界与验证基线。
- `docs/superpowers/specs/2026-08-23-cmp-cancellation-v1-design.md`：已实现设计。
- `docs/superpowers/plans/2026-08-23-cmp-cancellation-v1.md`：已完成实施清单和本地结果。
- `docs/superpowers/specs/2026-08-24-cmp-when-all-v1-design.md`：当前汇合 API 设计。
- `docs/superpowers/plans/2026-08-24-cmp-when-all-v1.md`：当前实施与交付清单。
- `.agent/HANDOFF.md`：本文件。

## 验证情况

- 新测试先因缺少 `OperationCancelled` 和 token 重载按预期编译失败，完成实现后转绿。
- Dev 严格无缓存构建通过，完整测试 38/38（8 个 Task、30 个 RunLoop）。
- Cancellation v1 合并前的 Release 严格无缓存构建通过，完整测试 38/38。
- Release 的完整 RunLoop 30 项套件额外连续执行 10 次，全部通过；未见双恢复、死锁或丢唤醒。
- `examples/basic` 的 `mcpp run` 状态为 0，依次输出 `Coroutine result: 42` 和
  `Concurrent result: 42`、`Coroutine cancelled`。
- 功能提交 `3a997ab` 的 Linux x86_64、macOS arm64、Windows x86_64 手动 CI 均通过构建、
  38 项测试和独立示例。
- PR #4 最终 head `67d0efc` 的三平台检查全部通过；合并提交 `ad80fb0` 触发的 `main` 三平台
  CI 也全部通过。
- mcpp 仍提示本机 SubOS 缺少 `subos_info`，但工具链解析为 LLVM 22.1.8，未影响构建或运行。
- 已按用户授权提交、推送、创建 PR #4 并 squash 合并；未删除远端功能分支或修改其他远端
  资源。
- 新测试先因缺少 `when_all` 导出按预期编译失败；实现后聚焦测试 8/8 通过。
- Dev 与 Release 严格无缓存构建通过；两个 profile 的完整测试均为 46/46（8 个 Task、
  30 个 RunLoop、8 个 when_all）。
- Release 的完整 `when_all` 8 项套件额外连续执行 50 次，全部通过。
- 更新后的独立示例构建运行通过并输出三行预期结果。
- PR #5 head `d6ea2a8` 的 Linux x86_64、macOS arm64、Windows x86_64 CI 均通过构建、
  46 项测试和独立示例；已 squash 合并为 `d7dd418`。

## 已知问题 / 风险

- 三套 Actions 均提示 `actions/checkout@v4` 的 Node.js 20 已弃用并被强制使用 Node.js 24；
  当前不影响 CI 结果，workflow 升级应作为后续独立修改。
- 取消定位为 O(n)，适合当前最小实现；大量并发取消的性能上限尚未基准测试。
- 外部销毁已经发布到 RunLoop 的协程帧仍不受支持；Cancellation v1 不提供 detached 安全。
- 取消只覆盖 Scheduler 定时等待，不会中断阻塞调用，也不会自动传播到任意子 Task 或外部
  awaiter。
- pending `when_all` 仍遵循现有 Task 边界：子任务发布 continuation 后，外部提前销毁聚合
  帧属于无效使用。

## 剩余工作

1. `when_all` v1 没有剩余实现或交付工作。
2. 下一阶段从最新 `main` 新建分支，独立设计动态结构化任务作用域与取消传播。

## 推荐下一步

下一步从最新 `main` 创建新分支，先定义动态任务作用域的 close/join 生命周期、异常策略和
`std::stop_token` 传播边界；不要直接扩展已合并的 `when_all` 分支。
