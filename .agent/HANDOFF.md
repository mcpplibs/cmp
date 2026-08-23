# CMP 当前工作状态

## 项目概览

CMP 是使用 mcpp 构建的 C++23 Modules 协程运行时库。当前公开核心包括惰性、唯一所有权的
`Task<T>`，单消费者 `RunLoop`，以及可复制的 `RunLoop::Scheduler`。Scheduler 支持普通
`schedule()`、相对期限 `schedule_after()` 和绝对期限 `schedule_at()`；所有 continuation
均由调用 `RunLoop::run()` 的线程恢复。

项目使用 `mcpplibs.cmp` 模块；`.xlings.json` 固定 mcpp 2026.8.11.2，`mcpp.toml` 固定
LLVM 22.1.8，测试依赖为 `compat.gtest` 1.15.2。实现位于 `src/`，契约测试位于 `tests/`，
`examples/basic` 是独立的 path-dependency 消费示例。

## 当前目标与状态

Timer v1 已实现并完成本地 Dev、Release 和重复并发验证。本次交付提交位于
`feature/timer-v1`，并推送至 `origin` 的同名分支。用户没有授权创建或修改 PR；Linux、
macOS 和 Windows CI 结果仍待确认。

## 已完成工作

- 为 Scheduler 导出 `Clock`、`Duration`、`TimePoint`，新增 `schedule_after()` 与
  `schedule_at()`。
- 在 `RunLoopState` 中加入标准库最小 Timer 堆、溢出安全的相对期限计算和新最早期限通知。
- `drive()` 使用非谓词 `wait_until()`，每轮最多把一个到期 Timer 追加到现有 FIFO，并始终
  在互斥量外恢复协程。
- 已到期和非正期限仍会挂起并直接进入 FIFO；最大时长不会回绕成立即任务。
- Timer 加入现有 outstanding-work、失败清理和 RunLoop 顺序复用契约。
- 新增正常期限、立即期限、同期限、跨线程唤醒、无效 Scheduler、溢出清理和十万次立即
  Timer 测试。
- 独立示例会在协程中等待 `10ms` 后打印；英文、简体中文和繁体中文 README/架构文档已同步。

## 重要决策

- 固定使用 `std::chrono::steady_clock`；相对期限在 `await_suspend()` 时测量。
- 正延迟先比较 `TimePoint::max() - delay`，确认安全后才执行加法。
- 所有 timed await 都真正挂起；期限到达只表示具备运行资格，不承诺精确恢复时间。
- 未到期 Timer 使用 `std::priority_queue`，不增加后台线程、平台 Timer、依赖或公开 Timer
  对象。
- 仅新最早期限需要打断既有 timed wait；等待前复制堆顶期限，避免解锁后堆重排导致引用
  失效。
- 每轮最多提升一个到期 Timer，限制持锁时间，并通过追加到 FIFO 平衡 ready/timer 公平性。
- Timer 仍是非所有权调度引用；取消和 exactly-once 竞态留到独立设计。
- `std::priority_queue` 无法高效删除任意 Timer；取消阶段必须选择惰性失效或更换数据结构。

## 修改 / 重要文件

- `src/run_loop.cppm`：Timer 堆、驱动循环和公开 timed Scheduler API。
- `tests/run_loop_test.cpp`：Timer v1 契约与边界测试。
- `examples/basic/src/main.cpp`：协程内定时打印示例。
- `README.md`、`README.zh.md`、`README.zh.hant.md`：三语公开说明。
- `docs/architecture.md`、`docs/architecture.zh.md`、`docs/architecture.zh.hant.md`：三语架构契约。
- `docs/superpowers/specs/2026-08-12-cmp-timer-v1-design.md`：已实现设计。
- `docs/superpowers/plans/2026-08-12-cmp-timer-v1.md`：已完成实施计划和本地结果。

## 验证情况

- 基线：修改实现前 22 项测试通过。
- 红灯：新增测试因缺少 timed API 按预期编译失败。
- Dev：`mcpp build` 通过，29/29 测试通过。
- Release：`mcpp build` 通过，29/29 测试通过。
- 稳定性：Release RunLoop 的 21 项测试额外连续运行 10 次，全部通过。
- 示例：`examples/basic` 构建运行通过，输出 `Coroutine result: 42`。
- mcpp 每次运行都会提示现有 SubOS 缺少 `subos_info`；不影响本次编译、测试或示例结果。
- 本次 Git 授权仅包括提交和推送；没有创建或修改 PR，也没有执行其他 GitHub 写操作。
- 三平台 CI 尚未在当前工作状态中确认。

## 已知问题 / 风险

- 只有当前 Linux WSL2 环境完成本地验证，macOS/Windows 结果必须以 CI 为准。
- 并发测试使用真实 `steady_clock` 和宽松存活上界；目前重复测试稳定，但 CI 仍需确认。
- Timer v1 不支持取消；外部销毁已经发布的协程帧仍由所有者负责。

## 剩余工作

1. 后续获得相应授权后，确认 Linux、macOS 和 Windows CI 结果。
2. CI 通过后，只有用户明确授权才可创建或修改 PR。
3. Timer v1 合并后，再单独设计取消与结构化唤醒路径。

## 推荐下一步

先确认三平台 CI；若全部通过，再由用户决定是否授权 PR。不要从本次提交和推送授权推导 PR
或合并权限。
