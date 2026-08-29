# 架构

[English](architecture.md) · **简体中文** · [繁體中文](architecture.zh.hant.md)

## 当前状态

CMP 是一个具备小型协程执行核心的 C++23 模块项目。根模块导出懒启动、单消费者的
`mcpplibs::cmp::Task<T>`、结构化变参/vector `when_all()`、eager `TaskGroup`、`RunLoop` 及其
可复制的 `Scheduler` 句柄、无分配 `OneShotEvent`、可复用 `AsyncManualResetEvent` 和 RAII
`AsyncMutex`。汇合原语会持有每个子任务直到结束，事件负责发布外部信号。
`RunLoop::run()` 是公共根任务执行边界，`Scheduler::schedule()` 用于显式地把挂起协程送回
对应运行循环。`schedule()`、`schedule_after()` 和 `schedule_at()` 都有接受
`std::stop_token` 的协作式取消重载；定时调度使用相对和绝对的 `steady_clock` 期限，且不创建
定时线程。

仓库现有内容包括：

- 一份 mcpp 包清单；
- 根模块 `mcpplibs.cmp` 及 Task、cancellation、RunLoop、join、event、mutex 模块分区；
- 覆盖契约、生命周期、异常、调度和线程行为的 gtest 测试；
- 一个通过路径依赖使用根包的独立示例；
- 一个验证计算、文件与回环网络集成的本地 POSIX 压测 consumer；
- Linux、macOS 和 Windows 三套 CI 工作流。

## 包和模块标识

```toml
[package]
namespace   = "mcpplibs"
name        = "cmp"
version     = "0.1.0"
standard    = "c++23"
description = "A C++23 coroutine runtime project built with mcpp and C++ Modules"
license     = "Apache-2.0"
repo        = "https://github.com/mcpplibs/cmp"
```

mcpp 包由 `mcpplibs` 和 `cmp` 共同标识。使用方在 `[dependencies.mcpplibs]` 中声明
`cmp`，在 C++ 源码中导入 `mcpplibs.cmp`。公共 C++ 声明使用 `mcpplibs::cmp` 命名空间。

根模块接口位于 `src/cmp.cppm`，符合 mcpp 默认的库根模块命名规则。仓库中没有
`src/main.cpp`，因此 mcpp 会推断出名为 `cmp` 的库目标，不需要额外配置 `[lib]` 或
`[targets.cmp]`。虽然 C++23 也是 mcpp 的默认标准，清单中仍然明确写出这一基线。

`compat.gtest = "1.15.2"` 是测试使用的显式命名空间开发依赖。CMP 当前不跟踪
`mcpp.lock`，该文件由 `.gitignore` 排除。

## 仓库结构

```text
.
├── .github/workflows/
│   ├── ci-linux.yml
│   ├── ci-macos.yml
│   └── ci-windows.yml
├── .xlings.json
├── docs/
│   ├── architecture.md
│   ├── architecture.zh.md
│   └── architecture.zh.hant.md
├── examples/basic/
│   ├── mcpp.toml
│   └── src/main.cpp
├── src/
│   ├── cmp.cppm
│   ├── task.cppm
│   ├── cancellation.cppm
│   ├── run_loop.cppm
│   ├── when_all.cppm
│   ├── task_group.cppm
│   ├── one_shot_event.cppm
│   ├── async_manual_reset_event.cppm
│   └── async_mutex.cppm
├── tests/
│   ├── cmp_test.cpp
│   ├── run_loop_test.cpp
│   ├── when_all_test.cpp
│   ├── task_group_test.cpp
│   ├── one_shot_event_test.cpp
│   ├── async_manual_reset_event_test.cpp
│   └── async_mutex_test.cpp
├── benchmarks/v1-readiness/
│   ├── mcpp.toml
│   └── src/main.cpp
└── mcpp.toml
```

## 构建与测试

`.xlings.json` 固定项目使用的 mcpp 版本。`mcpp build` 构建自动推断的库目标。
`mcpp test` 发现七个测试文件，并为每个文件链接 gtest 入口。90 项测试同时验证 Task 所有权和
对称转移、结构化汇合，以及根任务执行、普通与定时调度、异常传播、跨线程期限唤醒、无效
Scheduler、取消竞态、RunLoop 复用和不会增长调用栈的重复完成。

三套 CI 工作流都会安装项目工具、构建库、运行测试并执行 `examples/basic`。不同操作系统
的工具安装和运行环境不同，因此分别保留工作流文件。

CMP 当前不随包发布供 `mcpp new` 使用的项目模板。原脚手架中的 `basic` 和 `lib` 是通用
发布模板，并不反映 CMP 的实际用法。删除这些模板后，`tools/template_smoke.sh` 已经没有
可渲染和编译的对象，因此连同对应的 CI 步骤一起删除。`examples/basic` 继续保留，用来直接
验证包的使用方式。

`.gitignore` 排除 mcpp 生成物、编译数据库、项目工具环境、编译器和编辑器缓存，以及仓库内
的本地项目状态。这些内容由工具生成或只在本机使用，不属于发布包。

## 独立示例

`examples/basic` 有自己的清单，并通过路径依赖引用仓库根目录：

```toml
[package]
name     = "cmp-basic"
standard = "c++23"

[dependencies.mcpplibs]
cmp = { path = "../.." }
```

下面的精简 API 示例使用与外部项目相同的导入路径：

```cpp
import std;
import mcpplibs.cmp;

using mcpplibs::cmp::Task;
using mcpplibs::cmp::RunLoop;
using mcpplibs::cmp::AsyncMutex;
using mcpplibs::cmp::OperationCancelled;
using mcpplibs::cmp::OneShotEvent;
using mcpplibs::cmp::TaskGroup;
using mcpplibs::cmp::when_all;

using namespace std::chrono_literals;

Task<int> answer() {
    co_return 42;
}

Task<void> print_answer(RunLoop::Scheduler scheduler) {
    co_await scheduler.schedule_after(10ms);
    auto value = co_await answer();
    std::println("Coroutine result: {}", value);
    co_return;
}

Task<int> delayed_value(
    RunLoop::Scheduler scheduler,
    std::chrono::milliseconds delay,
    int value) {
    co_await scheduler.schedule_after(delay);
    co_return value;
}

Task<void> print_concurrent_results(RunLoop::Scheduler scheduler) {
    std::vector<Task<int>> tasks {};
    tasks.emplace_back(delayed_value(scheduler, 10ms, 20));
    tasks.emplace_back(delayed_value(scheduler, 1ms, 22));
    auto values = co_await when_all(std::move(tasks));
    std::println("Concurrent result: {}", values[0] + values[1]);
    co_return;
}

Task<void> add_delayed(
    RunLoop::Scheduler scheduler,
    std::chrono::milliseconds delay,
    int value,
    int& total) {
    co_await scheduler.schedule_after(delay);
    total += value;
    co_return;
}

Task<void> print_task_group(RunLoop::Scheduler scheduler) {
    int total { 0 };
    TaskGroup group {};
    group.spawn(add_delayed(scheduler, 10ms, 20, total));
    group.spawn(add_delayed(scheduler, 1ms, 22, total));
    co_await group.join();
    std::println("Task group result: {}", total);
    co_return;
}

Task<void> set_event(RunLoop::Scheduler scheduler, OneShotEvent& event) {
    co_await scheduler.schedule();
    event.set();
    co_return;
}

Task<void> print_event(RunLoop::Scheduler scheduler) {
    OneShotEvent event {};
    TaskGroup group {};
    group.spawn(set_event(scheduler, event));
    co_await event;
    co_await group.join();
    std::println("Event signalled");
    co_return;
}

Task<void> add_locked(
    RunLoop::Scheduler scheduler,
    AsyncMutex& mutex,
    int value,
    int& total) {
    co_await scheduler.schedule();
    auto guard = co_await mutex.lock_async();
    total += value;
    co_return;
}

Task<void> print_mutex(RunLoop::Scheduler scheduler) {
    int total { 0 };
    AsyncMutex mutex {};
    TaskGroup group {};
    group.spawn(add_locked(scheduler, mutex, 20, total));
    group.spawn(add_locked(scheduler, mutex, 22, total));
    co_await group.join();
    std::println("Mutex result: {}", total);
    co_return;
}

Task<void> print_cancellation(RunLoop::Scheduler scheduler, std::stop_token token) {
    try {
        co_await scheduler.schedule_after(1s, token);
    } catch (const OperationCancelled&) {
        std::println("Coroutine cancelled");
    }
    co_return;
}

int main() {
    RunLoop loop {};
    loop.run(print_answer(loop.get_scheduler()));
    loop.run(print_concurrent_results(loop.get_scheduler()));
    loop.run(print_task_group(loop.get_scheduler()));
    loop.run(print_event(loop.get_scheduler()));
    loop.run(print_mutex(loop.get_scheduler()));

    std::stop_source source {};
    source.request_stop();
    loop.run(print_cancellation(loop.get_scheduler(), source.get_token()));
}
```

该示例在根测试目标之外，单独检查路径依赖解析、模块使用、外部协程编译和公共根任务驱动器。
RunLoop 依次输出 `Coroutine result: 42`、`Concurrent result: 42` 和 `Task group result: 42`；
递归增长的 group 输出 `Recursive group result: 3`。其他协程演示一次性及两轮可复用事件并
输出 `Event signalled`、`Reusable event cycles: 2`，两个受保护 Task 输出
`Mutex result: 42`。最后一个结构化 group 使用 `cancel_and_join()`，从可取消就绪调度捕获
`OperationCancelled` 并输出 `Coroutine cancelled`。

任何定义协程的翻译单元都要自行导入 `std`，使 `std::coroutine_traits` 和标准协程协议类型
参与编译。CMP 模块私有导入 `std`，而不是向使用方重新导出整个标准库。

## 当前边界

`Task<T>` 和 `Task<void>` 遵循以下契约：

- 构造时懒启动，协程体在 Task 被等待时开始执行；
- 所有权唯一：Task 可以移动构造，但不能复制或移动赋值；
- `operator co_await()` 仅用于右值，并在等待时消费协程句柄；
- 协程帧保存一个值或 `std::exception_ptr`；
- 子协程完成后直接转移到 continuation，避免递归调用 `resume()`；
- 未消费的 Task 销毁自己的帧，消费它的 awaiter 销毁已经完成的帧；
- 拒绝引用和数组结果类型。

被移动后的 Task 为空，不得再次等待；当前实现会在违反该契约时终止进程。

`when_all()` 遵循以下契约：

- 聚合 Task 懒启动，并持有每个输入 Task；
- 等待时从左到右启动输入，不等待较早挂起的输入完成；
- 所有子任务都到达终态后才恢复父任务；
- 结果按参数顺序组成 `std::tuple`，其中 `Task<void>` 映射为 `std::monostate`；
- `std::vector<Task<T>>` 输入按索引顺序产生相同数量的结果 vector；
- 支持 move-only 结果；
- 全部子任务结束后，按参数顺序重新抛出第一个子任务异常；
- 原子“计数 + 哨兵”协议保证立即完成和跨线程完成只恢复一次；
- 父任务在最后一个子任务完成的线程恢复，不隐式增加线程亲和。

`TaskGroup` 遵循以下契约：

- `spawn(Task<void>)` 接管所有权并在返回前 inline 启动子任务；
- 并发接纳会串行化；等待单次使用的 `join()` 期间，仍在运行的子任务可以递归接纳工作；
- 活动计数归零时接纳永久关闭；外部接纳与最后一个完成竞态时不保证哪方获胜；
- join 恢复前，每个已接纳子任务都必须到达终态；
- 全部子任务完成后，按接纳顺序重新抛出第一个异常；
- group 不可移动，析构时必须未使用或已经 join，否则终止进程；
- `get_stop_token()` 和 `request_stop()` 提供显式标准取消通道，但不会向 Task 注入 token；
- `cancel_and_join()` 会在真正被等待时请求该通道，然后执行相同的 join；
- 最后一个子任务在其完成线程恢复 join，不隐式增加调度器亲和。

`OneShotEvent` 遵循以下契约：

- 默认事件未 set，可由多个协程无分配地等待；
- 第一次 `set()` 永久设置事件，并恰好恢复所有已注册等待者一次；
- 注册与 set 竞态时，等待者要么进入恢复链表，要么观察到 set 状态；
- set 前的写入通过 acquire-release 顺序发布给等待者；
- 等待者按未指定顺序在 setter 线程 inline 恢复；
- 事件不可移动且必须比所有等待者活得更久；带 pending 等待者析构会终止进程；
- 不提供 reset、可取消注销、值或隐式 Scheduler 转移。

`AsyncManualResetEvent` 遵循以下契约：

- 默认事件为 unset；`set()` 按 FIFO 恢复 pending 等待者，并让后续等待保持 ready；
- `reset()` 只影响未来等待，已经被 set 选中的等待者仍会完成；
- `wait(stop_token)` 以 O(1) 移除一个取消的 pending 等待者，预先取消的等待确定由取消获胜；
- set 与取消竞态恰有一个获胜方，等待者只恢复一次；
- 等待者在 setter 或取消请求线程恢复，不隐式转移 Scheduler；
- thread-local dispatch trampoline 保证嵌套信号链不增长原生调用栈；
- 事件不可移动且必须比所有等待者活得更久；带 pending 等待者析构会终止进程。

`AsyncMutex` 遵循以下契约：

- `lock_async()` 返回无分配操作，等待结果是 move-only RAII Guard；
- 立即取得者 inline 继续，竞争等待者按 FIFO 顺序挂起；
- Guard 析构恰好交接一次所有权，并在该线程恢复下一个等待者；
- 所有权不绑定线程，内部状态锁不会在恢复用户协程时持有；
- mutex 必须比所有 Guard 和等待者活得更久；持有或排队时析构会终止；
- 不提供手工 unlock、try-lock、取消、递归或隐式 Scheduler 转移。

`RunLoop` 和 `Scheduler` 遵循以下契约：

- RunLoop 既不能复制也不能移动；它的身份是所有关联 Scheduler 的生命周期锚点；
- `run(Task<T>)` 消费一个根 Task，并在调用线程执行就绪 continuation；
- 返回根任务结果，包括 move-only 结果；根任务异常会重新抛出；
- RunLoop 可以顺序复用，但嵌套或并发调用 `run()` 会抛出 `std::logic_error`；
- `schedule()` 始终挂起，并把 continuation 追加到线程安全的 FIFO 就绪队列；接受 token 的
  重载可以在消费前取消该排队等待；
- `schedule_after()` 在挂起时测量原生 `steady_clock` 时长，`schedule_at()` 接受绝对的
  单调时钟时间点；
- 所有接受 token 的重载仍会排队，包括预先取消的等待；取消获胜时抛出
  `OperationCancelled`，较晚的停止请求不能替换已经获胜的完成；
- 已到期的期限仍异步排队；未来期限进入最小 Timer 堆，到期只让 continuation 具备进入
  FIFO 调度的资格；
- 停止回调只改变就绪项/定时器状态并唤醒 RunLoop，不会 inline 恢复用户协程；当前取消通过
  O(n) 扫描定位队列项；
- 其他线程可以入队，但只有正在执行 `run()` 的线程会消费队列；
- RunLoop 销毁后继续使用其 Scheduler，或在其所属 RunLoop 未运行时使用 Scheduler，都会
  从 await 表达式抛出 `std::logic_error`；
- 根任务完成时如果仍存在单独排队的工作或待处理 Timer，RunLoop 会拒绝退出，因为本阶段
  不支持 detached 所有权。

RunLoop 不拥有工作线程，也不提供自动线程亲和。外部 awaiter 可以在其他线程恢复 Task；
显式等待原 Scheduler 才会把 continuation 送回对应 RunLoop。如果 Task 挂起后没有安排
其他线程或事件源恢复它，`run()` 可能无限等待。阻塞函数仍会阻塞协程当前所在的线程。

目前没有公共自由函数 `sync_wait`、detached 执行、独立 Timer 句柄、异步 I/O 后端、自定义
协程帧 allocator 或阻塞任务线程池。取消仍是显式的：Scheduler 等待和
`AsyncManualResetEvent` 接受 token，TaskGroup 可持有共享 stop 通道，但模块不提供隐式传播，
并且没有保留旧脚手架模块的兼容别名。

捕获变量的协程 lambda 需要特别小心：立即调用一个临时的捕获 lambda，可能使懒协程引用
已经销毁的闭包。CMP 尚未提供延长该闭包生命周期的辅助函数。

CMP 名称中的 `C` 与 Go 运行时中的 `G` 相呼应，但这只说明命名来源，不表示两者语义等价。
标准 C++ 协程提供挂起和恢复机制，本身不包含调度器，也不会把阻塞操作自动变成异步操作。

## 可选的后续工作

以下方向可以分别设计和评审，目前都不是包的既有约定：

1. TaskGroup 结果句柄及更多支持取消的原语；
2. channel 和更多结构化唤醒路径；
3. 多工作线程调度和工作窃取；
4. 异步 I/O 集成；
5. 处理不可避免的阻塞工作的专用线程池；
6. 结果适配器和可选的协程帧分配策略。

Task、cancellation、RunLoop、`when_all`、TaskGroup、OneShotEvent、AsyncManualResetEvent 与
AsyncMutex 已经形成真实的公共边界，因此分别位于模块分区中。只有其他已实现 API 确实需要
新边界时，才继续增加模块分区或实现单元。

## 验证

在仓库根目录执行：

```text
mcpp build --profile dev --strict --cache=off
mcpp test --profile dev --strict --cache=off
mcpp build --profile release --strict --cache=off
mcpp test --profile release --strict --cache=off
cd examples/basic
mcpp run
```

预期结果是库构建成功、七个二进制中的 90 项测试全部通过，并且示例依次输出
`Coroutine result: 42`、`Concurrent result: 42`、`Task group result: 42`、
`Recursive group result: 3`、`Event signalled`、`Reusable event cycles: 2`、
`Mutex result: 42` 和 `Coroutine cancelled` 后以状态 0 退出。测试保留原有高容量栈安全检查，
并增加两万次 TaskGroup 递归接纳、十万次预取消就绪调度、五万个 manual event 等待者、两万次
嵌套可复用事件信号及 set/cancel 竞态；第四阶段重点竞态套件已连续执行多轮 Release 测试。
计算、临时文件和回环网络的成功/失败计数及吞吐记录在
[v1 可开发性压测](benchmarks/2026-08-29-cmp-v1-readiness.md)。当前 Windows LLVM 工具链不会
生成 GNU depfile；如果模块接口包含的文件发生变化，增量构建可能复用旧的 BMI 或目标文件。
完整复验时使用 `--cache=off`。
