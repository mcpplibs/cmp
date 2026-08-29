# CMP 当前工作状态

## 项目概览

CMP 是使用 mcpp 构建的 C++23 Modules 协程运行时库，公开模块为 `mcpplibs.cmp`。当前已实现
懒启动唯一所有权 `Task<T>`、变参/vector `when_all()`、静止点 `TaskGroup`、一次性与可复用
事件、RAII `AsyncMutex`、带定时和取消的调用线程 `RunLoop`，以及固定大小的 CPU
`ThreadPool`。

`.xlings.json` 固定 mcpp 2026.8.11.2；当前工具链为 LLVM 22.1.8，测试依赖为
`compat.gtest` 1.15.2。`examples/basic` 是独立 path-dependency consumer。

## 当前目标与状态

第五阶段 ThreadPool v1 已完成本地实现、测试、示例、压测、三语文档同步及复审修复，当前可
进入用户审查及远程 CI 交付。本轮未执行任何 Git 或 GitHub 操作；提交、推送、PR 和 CI 均不
应被视为已经完成。

## 已完成工作

- 新增不可移动的固定大小 `ThreadPool` 和弱引用、可复制的 `ThreadPool::Scheduler`；根模块已
  导出该分区。
- `schedule()` 始终挂起并转移到任意 worker；`schedule(stop_token)` 支持 completion/cancel
  原子竞态获胜语义。
- 实现共享 FIFO、休眠 worker、逐项唤醒、锁外恢复、构造失败清理、关闭/入队线性化、排空并
  join，以及 worker 内自析构终止保护。
- 新增 15 项确定性 ThreadPool 测试，覆盖 API、FIFO、多 worker 唤醒、结构化组合、取消竞态、
  高容量/栈安全、关闭与过期 Scheduler。
- 复审后把默认 worker 数断言收紧为精确契约，并补齐同源与异源过期 Scheduler 的身份比较
  测试。
- `examples/basic` 增加协程内 worker 计算、显式回到 RunLoop 并打印
  `Worker pool result: 42` 的示例。
- 新增标准库限定的 `benchmarks/thread-pool` consumer 和五轮数据报告；每轮覆盖粗粒度 CPU、
  连续调度、worker 内 fan-out 和并发重调度。
- README、架构文档、第五阶段设计/计划及压测报告已同步到实现事实。
- `mcpp-style-ref` 不再重复维护易过期的分区清单，改以架构文档为当前事实来源；本轮涉及的
  混合换行文件也已统一。

## 重要决策

- v1 只公开 `ThreadPool`、`get_scheduler()`、`thread_count()` 和两个 `schedule()` 重载；不增加
  executor 基类、submit/detach、resize、优先级、亲和或公共 shutdown API。
- 默认 worker 数使用 `hardware_concurrency()` 并把未知的零归一为一；显式传零抛出
  `std::invalid_argument`。
- 队列是无界共享 `std::deque`。有界同步接纳可能让递归提交的全部 worker 互相等待，因此
  backpressure 必须另行设计。
- 取消项留在 FIFO，由 worker 消费并恢复；取消回调不扫描队列，也不在请求取消的线程恢复用户
  协程。
- 析构关闭接纳并排空已经接纳的 continuation；ThreadPool 只拥有执行线程，Task 生命周期仍由
  `Task`、`when_all()` 和 `TaskGroup` 管理。
- 本机数据证明粗粒度 CPU 并行有效，也证明微任务会争用共享队列；work stealing 只有在代表性
  workload 的 profiler 证明共享队列是主要瓶颈后，才进入单独设计。

## 修改 / 重要文件

- 核心：`src/thread_pool.cppm`、`src/cmp.cppm`
- 测试：`tests/thread_pool_test.cpp`
- 示例：`examples/basic/src/main.cpp`
- 压测：`benchmarks/thread-pool/`、`docs/benchmarks/2026-08-29-cmp-thread-pool.md`
- 方案：`docs/superpowers/specs/2026-08-29-cmp-phase5-thread-pool-v1-design.md`、
  `docs/superpowers/plans/2026-08-29-cmp-phase5-thread-pool-v1.md`
- 公共文档：三份 README 与三份 `docs/architecture*` 文档
- Agent 技能：`.agents/skills/mcpp-style-ref/SKILL.md`

## 验证情况

- `mcpp build --profile dev --strict --cache=off`：通过。
- `mcpp test --profile dev --strict --cache=off`：8 个二进制、105/105 通过。
- `mcpp build --profile release --strict --cache=off`：通过。
- `mcpp test --profile release --strict --cache=off`：8 个二进制、105/105 通过。
- Dev `thread_pool_test` 定向复验：15/15 通过。
- Release 关键竞态测试子集连续执行 100 轮：100/100 通过。
- `examples/basic` 的 `mcpp run`：通过，包含 `Worker pool result: 42`，退出码 0。
- `mcpp-style-ref` 通过 `quick_validate.py` 校验；架构文档链接有效。
- 项目文本文件换行复查未发现混合 CRLF/LF 文件。
- ThreadPool Release benchmark：构建通过；五轮记录共 8,410,240 次操作全部成功、非预期失败
  为 0；最终可执行性复验也全部 PASS。
- 五轮中位数显示粗粒度 CPU 任务从 1 到 8 worker 约 7.56 倍加速；完整原始数据见压测报告。
- 以上均为本机 Linux/WSL2 结果；本轮没有运行 GitHub 三平台 CI。

## 已知问题 / 风险

- 共享 FIFO 在大量微小 continuation 与较多 worker 时存在显著 mutex/通知争用；这是已记录的
  v1 性能上限，不是丢任务或执行失败。
- ThreadPool 不抢占；长计算或阻塞调用会占住一个 worker。异步 I/O 与专用 blocking pool 尚未
  实现。
- 析构会等待已接纳 continuation 返回；用户代码永久阻塞时析构也会永久等待。在自身 worker
  内析构属于硬生命周期错误并立即终止。
- 三平台兼容性尚需远程 CI 确认。
- 本机 mcpp 仍输出 SubOS 缺少 `subos_info` 的环境警告，但本轮所有构建和运行均成功。

## 剩余工作

1. 用户审查第五阶段实现与压测结论。
2. 获得明确授权后，才可执行对应范围的 commit、push、PR 或远程 CI 操作。
3. 第五阶段合并并通过三平台 CI 后，再开始第六阶段异步 I/O / blocking pool 设计；不要把该
   能力补进当前 ThreadPool。

## 推荐下一步

先审查 `src/thread_pool.cppm`、`tests/thread_pool_test.cpp` 和线程池压测报告。若认可当前共享
FIFO v1 边界，再明确授权所需的 Git/GitHub 步骤完成交付；合并后从第六阶段的 I/O 契约与平台
边界开始设计。
