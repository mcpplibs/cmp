# CMP

> **Coroutine Machine Process**——一个基于 C++23 的协程运行时项目。

[![C++23](https://img.shields.io/badge/C%2B%2B-23-blue.svg)](https://en.cppreference.com/w/cpp/23)
[![Module](https://img.shields.io/badge/module-mcpplibs.cmp-green.svg)](https://en.cppreference.com/w/cpp/language/modules)
[![License](https://img.shields.io/badge/license-Apache_2.0-blue.svg)](LICENSE)

[English](README.md) · **简体中文** · [繁體中文](README.zh.hant.md)

[mcpp](https://github.com/mcpp-community/mcpp) · [架构](docs/architecture.zh.md) ·
[v1 可开发性压测](docs/benchmarks/2026-08-29-cmp-v1-readiness.md) ·
[线程池压测](docs/benchmarks/2026-08-29-cmp-thread-pool.md) ·
[Issues](https://github.com/mcpplibs/cmp/issues)

[![ci-linux](https://github.com/mcpplibs/cmp/actions/workflows/ci-linux.yml/badge.svg?branch=main)](https://github.com/mcpplibs/cmp/actions/workflows/ci-linux.yml)
[![ci-macos](https://github.com/mcpplibs/cmp/actions/workflows/ci-macos.yml/badge.svg?branch=main)](https://github.com/mcpplibs/cmp/actions/workflows/ci-macos.yml)
[![ci-windows](https://github.com/mcpplibs/cmp/actions/workflows/ci-windows.yml/badge.svg?branch=main)](https://github.com/mcpplibs/cmp/actions/workflows/ci-windows.yml)

> [!IMPORTANT]
> CMP 已提供懒启动、单消费者的 `Task<T>` / `Task<void>`、支持变参和 vector 的结构化
> `when_all()`、eager 结构化 `TaskGroup`、一次性与可复用事件、RAII `AsyncMutex`，以及在调用
> 线程运行、支持显式调度和单调时钟定时调度的 `RunLoop`，以及固定大小的 CPU `ThreadPool`。
> Phase 6B 新增显式 `IoContext` 和 move-only `TcpStream`，用于原生异步的数值地址 TCP client。
> `run_blocking()` 可在专用的 pool 实例上执行同步 callable，并通过显式的返回 Scheduler
> 交付结果。两种执行器的就绪调度、定时等待、可复用事件等待、TaskGroup 子任务和排队中的
> 阻塞 offload 可显式使用基于 `std::stop_token` 的协作式取消；TCP 操作采用同一 token 模型。
> detached 执行和其他原生 I/O 类型尚未实现。

CMP 计划基于标准无栈 C++ 协程构建现代协程运行时和库。显式 `co_await` 模型现已覆盖固定与
增量结构化并发、一次性事件通知、调用线程与多 worker 调度、单调时钟定时器、可取消等待和
阻塞工作的结构化隔离，以及一个可移植的原生异步 TCP client 切片。

## 为什么叫 CMP？

CMP 是 **Coroutine Machine Process** 的缩写。项目计划用 **C** 表示轻量协程执行单元；
它与 Go runtime 的 **G** 只存在命名和思维模型上的启发关系，并不表示未来的 CMP task
已经等同于 goroutine。

项目遵循以下原则：

- 使用 C++23 标准无栈协程和 C++ Modules；
- 通过 `co_await` 和专用 awaiter 显式表达挂起；
- 以带测试、可审查的小改动逐步建设运行时；
- 不把使用场景限定为服务器程序；
- 始终以 mcpp 作为构建和包管理的唯一事实来源。

## 运行时边界

C++ 标准协程是语言机制，不是完整运行时。因此 CMP 不会宣称：

- task 自动等同于 Go goroutine；
- 任意阻塞调用会自动变成非阻塞调用；
- 可以直接在信号处理器中安全切换协程；
- task 会隐式迁移、work stealing 已启用，或所有 I/O 类型都已异步化。

这些能力必须分别设计和验证。CMP 目前通过显式的专用 `ThreadPool` 实例和
`run_blocking()` 隔离同步工作。Phase 6B 已提供原生异步 TCP client；DNS、监听 socket、TLS、
文件 I/O 和协作式安全点仍需分别设计。

## 快速开始

安装 [xlings](https://github.com/openxlings/xlings)，然后安装 [`.xlings.json`](.xlings.json)
固定的 mcpp 版本：

```bash
xlings install
mcpp --version
mcpp build
mcpp test
```

运行独立 consumer：

```bash
cd examples/basic
mcpp run
```

示例会打印 `Coroutine result: 42`、`Concurrent result: 42`、`Worker pool result: 42`、
`Blocking result: 42`、`Task group result: 42` 和 `Recursive group result: 3`，然后通过
`Event signalled` 与 `Reusable event cycles: 2`
演示一次性及可复用通知，再打印 `Mutex result: 42` 和 `Coroutine cancelled`；所有输出都在
`Task<void>` 协程内部。

## 当前 API

```cpp
import std;
import mcpplibs.cmp;

using mcpplibs::cmp::Task;
using mcpplibs::cmp::RunLoop;
using mcpplibs::cmp::AsyncMutex;
using mcpplibs::cmp::OperationCancelled;
using mcpplibs::cmp::OneShotEvent;
using mcpplibs::cmp::TaskGroup;
using mcpplibs::cmp::ThreadPool;
using mcpplibs::cmp::IoContext;
using mcpplibs::cmp::TcpStream;
using mcpplibs::cmp::run_blocking;
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

Task<int> calculate_on_workers(
    ThreadPool::Scheduler workers,
    RunLoop::Scheduler caller) {
    co_await workers.schedule();
    const int result = 21 * 2;
    co_await caller.schedule();
    co_return result;
}

Task<void> print_worker_result(
    ThreadPool::Scheduler workers,
    RunLoop::Scheduler caller) {
    const auto result = co_await calculate_on_workers(workers, caller);
    std::println("Worker pool result: {}", result);
    co_return;
}

Task<void> print_blocking_result(
    ThreadPool::Scheduler blockingWorkers,
    RunLoop::Scheduler caller) {
    const auto result = co_await run_blocking(
        blockingWorkers,
        caller,
        [] {
            std::this_thread::sleep_for(1ms);
            return 42;
        });
    std::println("Blocking result: {}", result);
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
    ThreadPool workers { 2 };
    ThreadPool blockingWorkers { 2 };
    RunLoop loop {};
    loop.run(print_answer(loop.get_scheduler()));
    loop.run(print_concurrent_results(loop.get_scheduler()));
    loop.run(print_worker_result(
        workers.get_scheduler(),
        loop.get_scheduler()));
    loop.run(print_blocking_result(
        blockingWorkers.get_scheduler(),
        loop.get_scheduler()));
    loop.run(print_task_group(loop.get_scheduler()));
    loop.run(print_event(loop.get_scheduler()));
    loop.run(print_mutex(loop.get_scheduler()));

    std::stop_source source {};
    source.request_stop();
    loop.run(print_cancellation(loop.get_scheduler(), source.get_token()));
}
```

`Task` 采用懒启动：调用 `answer()` 只创建处于挂起状态的协程，在被 `co_await` 消费时才
开始执行。Task 只能移动、只有一个消费者且只能作为右值等待；它保存值或异常，在子协程与
continuation 之间直接转移，并通过 RAII 销毁未消费的协程帧。当前刻意不支持 `Task<T&>`、
复制、移动赋值和 detached 执行。

`when_all()` 同样采用懒启动。等待它时会从左到右启动全部输入 Task，不会等待较早挂起的输入
完成，并持续持有所有子协程帧，直到全部输入结束。结果按参数顺序组成 `std::tuple`；
`Task<void>` 对应 `std::monostate`。如果子任务失败，仍会先等待全部子任务收尾，再按参数
顺序重新抛出第一个异常。命名 Task 必须移动到 `when_all()` 中。运行时数量的同类型任务可以
作为 `std::vector<Task<T>>` 传入，并按相同索引顺序返回结果 vector；命名输入 vector 同样
必须移动。

`TaskGroup::spawn()` 接管一个 `Task<void>` 并立即启动它。单次使用的 `join()` 会等待 group
到达静止点；join 等待期间，仍在运行的子任务可以递归增加工作。活动计数归零时接纳永久关闭。
TaskGroup 必须在析构前完成 join。`get_stop_token()` 和 `request_stop()` 提供显式标准取消通道，
`cancel_and_join()` 会先请求该通道再 join。token 不会自动注入。需要返回子任务结果时应使用
`when_all()`。

在未 set 的 `OneShotEvent` 上执行 `co_await event` 会无分配地挂起。第一次线程安全的 `set()`
会永久设置事件，并在 setter 线程恰好恢复每个已注册等待者一次；后续等待 inline 继续，后续
set 不做任何事。事件不可移动且必须比等待者活得更久；v1 不提供 reset 或隐式 Scheduler 转移。

`AsyncManualResetEvent` 提供可复用的 `set()` / `reset()` 周期。`wait(stop_token)` 支持协作式
取消，pending 等待者按 FIFO 顺序恢复，set 与 cancel 竞态只有一个稳定获胜方。协程会在调用
`set()` 或请求取消的线程恢复；需要 RunLoop 亲和时应显式等待 Scheduler。事件必须比所有等待者
活得更久。

`auto guard = co_await mutex.lock_async()` 无分配地取得 `AsyncMutex`，并通过 RAII 释放。竞争等待者
按 FIFO 顺序取得所有权并在释放线程恢复；所有权不绑定线程。mutex 必须比所有 Guard 和排队
等待者活得更久。v1 不提供手工 unlock、try-lock、取消或隐式 Scheduler 转移。

定义协程的翻译单元必须导入 `std`，使编译器能够看到标准协程协议类型。CMP 私有导入
`std`，不会向使用方重新导出整个标准库。

`RunLoop::run()` 消费一个根 Task，在调用线程执行就绪协程，返回结果并重新抛出异常。
`Scheduler::schedule()` 始终挂起当前协程并把 continuation 放入队列；它的
`std::stop_token` 重载可以取消该排队等待。`schedule_after()` 等待相对的 `steady_clock`
时长，`schedule_at()` 等待绝对的单调时钟时间点。期限到达只让
任务具备运行资格，不会 inline 恢复。Scheduler 可以复制，但始终属于创建它的 RunLoop。
支持顺序多次调用 `run()`，嵌套或并发调用会被拒绝。已经被移动的 Task 不得再次等待。

所有接受 `std::stop_token` 的 Scheduler 重载仍会排队，包括预先取消的等待。如果取消先于
消费获胜，等待会抛出 `OperationCancelled`；较晚的停止请求不能替换已经获胜的完成。停止回调
只唤醒 RunLoop，用户协程仍由执行 `run()` 的线程恢复。当前取消通过 O(n) 扫描定位队列项。

`ThreadPool` 启动固定数量的 CPU worker。其可复制 Scheduler 始终挂起，并可在任意 worker 上
恢复 continuation；需要回到调用线程时显式等待 `RunLoop::Scheduler`。
`schedule(stop_token)` 与 RunLoop 就绪调度采用相同的完成/取消胜者规则。v1 使用一个
work-conserving 共享 FIFO 和休眠 worker；析构关闭接纳、排空全部已接纳项并 join worker。
ThreadPool 不拥有上层 Task，阻塞调用仍会阻塞当前 worker。

`run_blocking(blockingWorkers, returnTo, operation, stopToken)` 在其懒 Task 帧中持有同步
callable，将它调度到指定 ThreadPool 恰好执行一次，并在通过 `returnTo` 调度回来后交付值、
`void`、异常或排队取消。应使用独立的 ThreadPool 实例隔离阻塞工作，避免占用延迟敏感的 CPU
worker。停止请求可以跳过尚未被 worker 领取的工作，但不能抢占已经运行的同步调用；返回调度
刻意不可取消。这是基于线程的隔离，不是原生非阻塞 I/O。

原生 TCP 接口保持显式且精简：

```cpp
Task<std::size_t> exchange(
    IoContext& io,
    RunLoop::Scheduler caller,
    std::string_view numericAddress,
    std::uint16_t port,
    std::span<const std::byte> request,
    std::span<std::byte> reply,
    std::stop_token token = {}) {
    auto stream = co_await TcpStream::connect(
        io, caller, numericAddress, port, token);
    co_await stream.write_all(caller, request, token);
    co_return co_await stream.read_some(caller, reply, token);
}
```

一个不可移动的 `IoContext` 可供多个 stream 共享，并拥有一个私有 I/O driver。`TcpStream` 只能
移动；connect 只接受数值 IPv4/IPv6 文本，不会隐藏阻塞 DNS。read/write span 的底层存储必须
保持到返回的 Task 完成。每种成功或错误都会尝试显式返回 Scheduler。一个 read 和一个 write
可以并存；同方向第二个操作抛出 `std::logic_error`。预取消不会关闭仍可用的 stream，而已发起
的 `write_all()` 被取消后会关闭 stream。`close()` 线程安全、幂等；当 close 获胜时，已接纳
操作恰好一次以 `OperationCancelled` 完成。Phase 6A 在线程池上隔离任意同步调用；Phase 6B
只提供原生异步 TCP，不是通用异步 I/O 层。

RunLoop 不是后台线程，也不会把阻塞代码自动变成异步代码。如果 Task 挂起后没有安排未来的
恢复动作，`run()` 可能一直等待。CMP 不提供隐式线程亲和：外部 awaiter 在其他线程恢复协程
后，需要显式等待目标 Scheduler 才会返回对应 RunLoop。

## 仓库结构

```text
.
├── .xlings.json              # 固定的项目工具环境
├── mcpp.toml                 # 包身份、运行时与测试依赖
├── src/cmp.cppm              # 根模块接口
├── src/task.cppm             # Task 模块分区
├── src/cancellation.cppm     # 共享的协作式取消异常
├── src/run_loop.cppm         # RunLoop 与 Scheduler 分区
├── src/thread_pool.cppm      # 固定大小的 CPU worker 调度器
├── src/blocking.cppm         # 结构化阻塞调用 offload
├── src/tcp.cppm              # 原生异步 TCP client 与 I/O context
├── src/when_all.cppm         # 结构化并发 Task 汇合
├── src/task_group.cppm       # eager 可变结构化 Task 作用域
├── src/one_shot_event.cppm   # 无分配一次性通知
├── src/async_manual_reset_event.cppm # 可复用、可取消通知
├── src/async_mutex.cppm      # FIFO 协程感知 RAII 互斥锁
├── tests/cmp_test.cpp        # Task 契约和生命周期测试
├── tests/run_loop_test.cpp   # 调度、边界和线程测试
├── tests/thread_pool_test.cpp # worker、取消和关闭测试
├── tests/blocking_test.cpp   # 阻塞 offload、线程亲和和取消测试
├── tests/tcp_test.cpp        # TCP 生命周期、取消、竞态和负载测试
├── tests/when_all_test.cpp   # 汇合所有权、结果和竞态测试
├── tests/task_group_test.cpp # 可变作用域生命周期和竞态测试
├── tests/one_shot_event_test.cpp # 事件发布和竞态测试
├── tests/async_manual_reset_event_test.cpp # 可复用事件竞态测试
├── tests/async_mutex_test.cpp # mutex 所有权和交接测试
├── examples/basic/           # 独立的路径依赖 consumer
├── benchmarks/v1-readiness/  # 本地计算、文件和回环网络基线
├── benchmarks/thread-pool/   # 本地多 worker 调度基线
├── docs/architecture.zh.md   # 当前结构、边界和演进方向
└── .github/workflows/        # Linux、macOS 和 Windows CI
```

仓库目前不发布 `mcpp new` 模板。等 CMP 形成值得演示的稳定运行时 API 后，再设计专用模板。

## 开发与验证

本地验证路径：

```bash
mcpp build --profile dev --strict --cache=off
mcpp test --profile dev --strict --cache=off
mcpp build --profile release --strict --cache=off
mcpp test --profile release --strict --cache=off
cd examples/basic && mcpp run
```

CI 在 Linux、macOS 和 Windows 上执行等价的构建、测试和独立示例流程。mcpp 版本由
`.xlings.json` 固定，不应依赖无关的全局 mcpp 安装。

CMP 当前不跟踪 `mcpp.lock`，`.gitignore` 明确执行这一仓库约定。运行时依赖放在
`[dependencies]`，gtest 明确声明在 `[dev-dependencies.compat]` 中。
当前本地套件包含 10 个测试二进制、140 项测试。仅用于 POSIX 的 Release 压测 consumer 及其
成功/失败数据记录在 [v1 可开发性压测](docs/benchmarks/2026-08-29-cmp-v1-readiness.md)。
多 worker 正确性和性能数据记录在[线程池压测](docs/benchmarks/2026-08-29-cmp-thread-pool.md)。

## 路线图

运行时工作拆分为可以独立审查的阶段：

1. 包身份和可导入模块 bootstrap——已完成；
2. 协程 task 与生命周期语义——已实现初始 `Task`；
3. 根任务驱动器和最小单线程调度器——已完成初始实现；
4. 单调时钟 Timer v1、可取消就绪/定时等待、变参/vector 汇合、静止点 TaskGroup、
   OneShotEvent、AsyncManualResetEvent 和 AsyncMutex——已实现并完成压力验证；
5. 固定大小的多 worker 调度——已实现并完成压测；work stealing 仍需 profiling 证据；
6. 结构化阻塞 offload——已实现；数值地址原生异步 TCP client——已完成本地实现与验证，
   远程三平台 CI 仍待确认。

剩余顺序只是方向，不代表列出的能力已经实现。

## 参与贡献

修改模块边界前请先阅读[架构说明](docs/architecture.zh.md)。保持改动小而清晰，遵循 C++23
模块约定，并以 `mcpp build`、`mcpp test`、独立示例和 CI 结果作为实现事实。

## 许可证

[Apache License 2.0](LICENSE)
