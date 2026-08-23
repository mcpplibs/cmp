# CMP

> **Coroutine Machine Process**——一个基于 C++23 的协程运行时项目。

[![C++23](https://img.shields.io/badge/C%2B%2B-23-blue.svg)](https://en.cppreference.com/w/cpp/23)
[![Module](https://img.shields.io/badge/module-mcpplibs.cmp-green.svg)](https://en.cppreference.com/w/cpp/language/modules)
[![License](https://img.shields.io/badge/license-Apache_2.0-blue.svg)](LICENSE)

[English](README.md) · **简体中文** · [繁體中文](README.zh.hant.md)

[mcpp](https://github.com/mcpp-community/mcpp) · [架构](docs/architecture.zh.md) ·
[Issues](https://github.com/mcpplibs/cmp/issues)

[![ci-linux](https://github.com/mcpplibs/cmp/actions/workflows/ci-linux.yml/badge.svg?branch=main)](https://github.com/mcpplibs/cmp/actions/workflows/ci-linux.yml)
[![ci-macos](https://github.com/mcpplibs/cmp/actions/workflows/ci-macos.yml/badge.svg?branch=main)](https://github.com/mcpplibs/cmp/actions/workflows/ci-macos.yml)
[![ci-windows](https://github.com/mcpplibs/cmp/actions/workflows/ci-windows.yml/badge.svg?branch=main)](https://github.com/mcpplibs/cmp/actions/workflows/ci-windows.yml)

> [!IMPORTANT]
> CMP 已提供懒启动、单消费者的 `Task<T>` / `Task<void>`、支持变参和 vector 的结构化
> `when_all()`，以及在调用线程运行、支持显式调度和单调时钟定时调度的 `RunLoop`。定时
> 等待已支持基于 `std::stop_token` 的协作式取消；异步 I/O 和 detached 执行尚未实现。

CMP 计划基于标准无栈 C++ 协程构建现代协程运行时和库。显式 `co_await` 模型现已覆盖结构化
并发汇合、调度、单调时钟定时器和可取消定时等待，并将通过经过验证的小步骤继续探索异步
I/O 以及阻塞工作的安全隔离。

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
- M:N 调度、work stealing、通用取消传播或异步 I/O 已经实现。

这些能力必须分别设计和验证。预期方向是显式异步 I/O awaiter、专用 blocking pool
以及协作式安全点。

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

示例会打印 `Coroutine result: 42`，汇合两个定时 Task 后打印 `Concurrent result: 42`，
再从预先取消的定时等待打印 `Coroutine cancelled`；所有输出都在 `Task<void>` 协程内部。
它负责证明独立 mcpp 包能够导入 `mcpplibs.cmp`、组合并汇合 Task，以及通过公共 RunLoop
使用定时调度和取消。

## 当前 API

```cpp
import std;
import mcpplibs.cmp;

using mcpplibs::cmp::Task;
using mcpplibs::cmp::RunLoop;
using mcpplibs::cmp::OperationCancelled;
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

定义协程的翻译单元必须导入 `std`，使编译器能够看到标准协程协议类型。CMP 私有导入
`std`，不会向使用方重新导出整个标准库。

`RunLoop::run()` 消费一个根 Task，在调用线程执行就绪协程，返回结果并重新抛出异常。
`Scheduler::schedule()` 始终挂起当前协程并把 continuation 放入队列；`schedule_after()`
等待相对的 `steady_clock` 时长，`schedule_at()` 等待绝对的单调时钟时间点。期限到达只让
任务具备运行资格，不会 inline 恢复。Scheduler 可以复制，但始终属于创建它的 RunLoop。
支持顺序多次调用 `run()`，嵌套或并发调用会被拒绝。已经被移动的 Task 不得再次等待。

接受 `std::stop_token` 的定时重载也始终挂起。如果取消先于期限获胜，等待会抛出
`OperationCancelled`；较晚的停止请求不能替换已经进入就绪队列的期限完成。停止回调只唤醒
RunLoop，用户协程仍由执行 `run()` 的线程恢复。当前取消通过 O(n) 扫描定位定时器。

RunLoop 不是后台线程，也不会把阻塞代码自动变成异步代码。如果 Task 挂起后没有安排未来的
恢复动作，`run()` 可能一直等待。CMP 不提供隐式线程亲和：外部 awaiter 在其他线程恢复协程
后，需要显式等待目标 Scheduler 才会返回对应 RunLoop。

## 仓库结构

```text
.
├── .xlings.json              # 固定的项目工具环境
├── mcpp.toml                 # 包身份和测试依赖
├── src/cmp.cppm              # 根模块接口
├── src/task.cppm             # Task 模块分区
├── src/run_loop.cppm         # RunLoop 与 Scheduler 分区
├── src/when_all.cppm         # 结构化并发 Task 汇合
├── tests/cmp_test.cpp        # Task 契约和生命周期测试
├── tests/run_loop_test.cpp   # 调度、边界和线程测试
├── tests/when_all_test.cpp   # 汇合所有权、结果和竞态测试
├── examples/basic/           # 独立的路径依赖 consumer
├── docs/architecture.zh.md   # 当前结构、边界和演进方向
└── .github/workflows/        # Linux、macOS 和 Windows CI
```

仓库目前不发布 `mcpp new` 模板。等 CMP 形成值得演示的稳定运行时 API 后，再设计专用模板。

## 开发与验证

本地验证路径：

```bash
mcpp build --cache=off
mcpp test --cache=off
cd examples/basic && mcpp run
```

CI 在 Linux、macOS 和 Windows 上执行等价的构建、测试和独立示例流程。mcpp 版本由
`.xlings.json` 固定，不应依赖无关的全局 mcpp 安装。

CMP 当前不跟踪 `mcpp.lock`，`.gitignore` 明确执行这一仓库约定。运行时依赖放在
`[dependencies]`，gtest 明确声明在 `[dev-dependencies.compat]` 中。

## 路线图

运行时工作拆分为可以独立审查的阶段：

1. 包身份和可导入模块 bootstrap——已完成；
2. 协程 task 与生命周期语义——已实现初始 `Task`；
3. 根任务驱动器和最小单线程调度器——已完成初始实现；
4. 单调时钟 Timer v1、可取消定时等待和变参/vector 结构化汇合——已实现；可变任务作用域、
   取消传播和更多唤醒路径仍待开发；
5. 多 worker 调度与 work stealing；
6. 异步 I/O 集成和 blocking pool。

剩余顺序只是方向，不代表列出的能力已经实现。

## 参与贡献

修改模块边界前请先阅读[架构说明](docs/architecture.zh.md)。保持改动小而清晰，遵循 C++23
模块约定，并以 `mcpp build`、`mcpp test`、独立示例和 CI 结果作为实现事实。

## 许可证

[Apache License 2.0](LICENSE)
