# CMP 当前工作状态

## 项目概览

CMP 是使用 mcpp 构建的 C++23 Modules 协程运行时库，公开模块为 `mcpplibs.cmp`。当前 v1
核心包括：

- 懒启动、唯一所有权的 `Task<T>`；
- 变参和 `std::vector` 结构化 `when_all()`；
- eager、可递归接纳至静止点的 `TaskGroup`；
- `OneShotEvent`、可取消的 `AsyncManualResetEvent`、RAII `AsyncMutex`；
- 单消费者 `RunLoop` 及可复制 `Scheduler`；
- 普通、相对期限、绝对期限调度，以及显式 `std::stop_token` 协作式取消。

`.xlings.json` 固定 mcpp 2026.8.11.2，工具链解析为 LLVM 22.1.8，测试依赖是
`compat.gtest` 1.15.2。`examples/basic` 是独立 path-dependency consumer。

## 当前目标与状态

用户要求补齐第四阶段，增加成功/失败/竞态测试，并对计算、文件 I/O、网络 I/O 做压力验证，
确认 CMP v1 可用于开发。

第四阶段实现、测试、示例和本机压测已经完成，交付前最终一致性检查已通过。Git 交付分支为
`feature/phase4-v1-readiness`；用户已明确授权提交并推送本阶段全部内容（包括压测），但没有
授权创建 PR 或合并。

## 本阶段实现

### TaskGroup 静止点 join

- `join()` 等待活动计数归零，不再在开始等待时立即关闭接纳。
- 已经属于 group 的活动子任务可在 join 等待期间继续 `spawn()`；静止点到达后永久关闭。
- 外部线程与最后完成瞬间竞态接纳时不保证哪方获胜；join 后接纳抛出 `std::logic_error`。
- 新增 lazy `cancel_and_join()`：真正被等待时先 `request_stop()`，再执行同一个单次 join。

### 更广泛的显式取消

- `OperationCancelled` 移到独立 `:cancellation` 分区。
- 新增 `Scheduler::schedule(std::stop_token)`；预取消也先排队，消费前取消获胜则抛出异常。
- ready 与 timer 共用取消状态和 stop callback 协议；终态在 RunLoop 状态锁下确定，避免双恢复。
- 普通调度和定时调度的取消查找仍为 O(n)，只有压测证明瓶颈后才升级数据结构。

### AsyncManualResetEvent

- 新增不可移动、无 waiter 分配的 `AsyncManualResetEvent`。
- 支持初始状态、`is_set()`、`set()`、`reset()`、`wait(stop_token)` 和 `co_await event`。
- pending 等待者按 FIFO 恢复；双向侵入队列支持 O(1) 取消移除。
- set/cancel 竞态只有一个终态；预取消确定由取消获胜。
- setter 或取消请求线程直接恢复等待者；thread-local trampoline 保证嵌套唤醒不增长原生栈。
- 带 pending 等待者析构会终止，事件必须比等待协程帧活得更久。

## 测试与压测

- 全量套件现为 7 个测试二进制、90 项测试。
- 新增/扩展测试覆盖正常、预取消、晚取消、跨线程、异常、拒绝、静止点、递归接纳、FIFO、
  publication、set/cancel 竞态、生命周期和原生栈安全。
- `AsyncManualResetEvent` 10 项套件在 Release 下连续 30 轮通过；每轮包含 1,000 次 set/cancel
  竞态、50,000 个等待者和 20,000 个嵌套信号。
- RunLoop 三项关键取消竞态在 Release 下连续 100 轮通过。
- TaskGroup 四项递归、并发和取消关键用例在 Release 下连续 100 轮通过。
- 最终文件状态下，Dev / Release `--strict --cache=off` 构建均通过；两个 profile 的全量
  测试均为 90/90，7 个测试二进制、0 失败。

独立 POSIX Release 压测位于 `benchmarks/v1-readiness`。五轮均为 PASS：

| 场景 | 每轮成功 | 每轮预期失败 | 每轮非预期失败 | 中位耗时 | 中位吞吐 |
| --- | ---: | ---: | ---: | ---: | ---: |
| compute | 49,000 | 1,000 | 0 | 28.172 ms | 1,774,824.8 ops/s |
| file_io | 1,000 | 100 | 0 | 185.222 ms | 5,938.8 ops/s |
| network_loopback | 20,000 | 100 | 0 | 2,455.883 ms | 8,184.4 ops/s |

原始五轮数据、环境、命令和边界见
`docs/benchmarks/2026-08-29-cmp-v1-readiness.md`。文件和网络工作由外部 `std::jthread`
适配，再通过事件和 Scheduler 返回 RunLoop；这验证 v1 互操作及生命周期，不宣称原生异步
I/O 性能。

## 示例输出

`examples/basic` 已运行成功，输出：

```text
Coroutine result: 42
Concurrent result: 42
Task group result: 42
Recursive group result: 3
Event signalled
Reusable event cycles: 2
Mutex result: 42
Coroutine cancelled
```

## 本阶段重要文件

- `src/cancellation.cppm`
- `src/run_loop.cppm`
- `src/task_group.cppm`
- `src/async_manual_reset_event.cppm`
- `src/cmp.cppm`
- `tests/run_loop_test.cpp`
- `tests/task_group_test.cpp`
- `tests/async_manual_reset_event_test.cpp`
- `examples/basic/src/main.cpp`
- `benchmarks/v1-readiness/mcpp.toml`
- `benchmarks/v1-readiness/src/main.cpp`
- `docs/benchmarks/2026-08-29-cmp-v1-readiness.md`
- `docs/superpowers/specs/2026-08-29-cmp-phase4-v1-completion-design.md`
- `docs/superpowers/plans/2026-08-29-cmp-phase4-v1-completion.md`
- 三份 README、三份架构说明和本文件。

## 已知边界与风险

- v1 没有原生异步 I/O、blocking pool、多 worker 调度、work stealing 或 detached 所有权。
- 阻塞调用仍会阻塞其所在执行线程；当前 I/O consumer 使用外部线程适配。
- 压测的 socket 实现仅支持 POSIX，不进入 Windows/macOS/Linux 通用 CI；库和根测试仍是跨平台目标。
- Scheduler 大量并发取消时 O(n) 查找可能成为瓶颈，目前没有数据要求升级。
- TaskGroup 保留 wrapper 帧至析构，空间复杂度 O(n)；外部接纳与最终静止点竞态不提供保证。
- AsyncManualResetEvent 在 set/cancel 调用线程恢复，不隐式保证 RunLoop 亲和；开发者需显式
  `co_await scheduler.schedule()` 返回目标循环。
- 本机 mcpp 仍提示 SubOS 缺少 `subos_info`；工具链解析和本次构建、测试、运行未受影响。
- 当前已完成本机验证，尚未进行 PR 三平台 CI 验证。

## 剩余工作

本地实现与验证没有剩余项。本阶段 Git 交付以 `feature/phase4-v1-readiness` 上的第四阶段
提交为边界；PR、合并和对应三平台 CI 仍需用户另行决定。

## 推荐下一步

确认功能分支远端同步后，由用户决定是否创建 PR 并进行三平台 CI。第四阶段合并后，再单独
设计第五阶段多 worker 调度，不把第六阶段 I/O 后端提前混入。
