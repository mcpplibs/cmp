# 架构

[English](architecture.md) · **简体中文** · [繁體中文](architecture.zh.hant.md)

## 当前状态

CMP 是一个具备小型协程执行核心的 C++23 模块项目。根模块导出懒启动、单消费者的
`mcpplibs::cmp::Task<T>`、结构化变参 `when_all()`、`RunLoop` 及其可复制的 `Scheduler`
句柄。`when_all()` 会启动并持有多个 Task，直到全部结束。`RunLoop::run()` 是公共根任务
执行边界，`Scheduler::schedule()` 用于显式地把挂起协程送回对应运行循环。
`schedule_after()` 和 `schedule_at()` 在没有定时线程的情况下提供相对和绝对的
`steady_clock` 期限；接受 `std::stop_token` 的重载使这些等待可以协作式取消。

仓库现有内容包括：

- 一份 mcpp 包清单；
- 根模块 `mcpplibs.cmp` 及 Task、RunLoop、`when_all` 模块分区；
- 覆盖契约、生命周期、异常、调度和线程行为的 gtest 测试；
- 一个通过路径依赖使用根包的独立示例；
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
│   ├── run_loop.cppm
│   └── when_all.cppm
├── tests/
│   ├── cmp_test.cpp
│   ├── run_loop_test.cpp
│   └── when_all_test.cpp
└── mcpp.toml
```

## 构建与测试

`.xlings.json` 固定项目使用的 mcpp 版本。`mcpp build` 构建自动推断的库目标。
`mcpp test` 发现三个测试文件，并为每个文件链接 gtest 入口。测试同时验证 Task 所有权和
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

示例程序使用与外部项目相同的导入路径：

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
    auto [first, second] = co_await when_all(
        delayed_value(scheduler, 10ms, 20),
        delayed_value(scheduler, 1ms, 22));
    std::println("Concurrent result: {}", first + second);
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

该示例在根测试目标之外，单独检查路径依赖解析、模块使用、外部协程编译和公共根任务驱动器。
RunLoop 在主线程驱动 `print_answer()`；短单调时钟定时器到期后，协程输出
`Coroutine result: 42`。下一个根任务并发汇合两个定时结果并输出 `Concurrent result: 42`。
最后一个协程从预先取消的定时等待捕获 `OperationCancelled`，并输出
`Coroutine cancelled`。

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
- 支持 move-only 结果；
- 全部子任务结束后，按参数顺序重新抛出第一个子任务异常；
- 原子“计数 + 哨兵”协议保证立即完成和跨线程完成只恢复一次；
- 父任务在最后一个子任务完成的线程恢复，不隐式增加线程亲和。

`RunLoop` 和 `Scheduler` 遵循以下契约：

- RunLoop 既不能复制也不能移动；它的身份是所有关联 Scheduler 的生命周期锚点；
- `run(Task<T>)` 消费一个根 Task，并在调用线程执行就绪 continuation；
- 返回根任务结果，包括 move-only 结果；根任务异常会重新抛出；
- RunLoop 可以顺序复用，但嵌套或并发调用 `run()` 会抛出 `std::logic_error`；
- `schedule()` 始终挂起，并把 continuation 追加到线程安全的 FIFO 就绪队列；
- `schedule_after()` 在挂起时测量原生 `steady_clock` 时长，`schedule_at()` 接受绝对的
  单调时钟时间点；
- 接受 token 的定时重载始终挂起；取消获胜时抛出 `OperationCancelled`，较晚的停止请求
  不能替换已经进入就绪队列的期限完成；
- 已到期的期限仍异步排队；未来期限进入最小 Timer 堆，到期只让 continuation 具备进入
  FIFO 调度的资格；
- 停止回调只改变定时器状态并唤醒 RunLoop，不会 inline 恢复用户协程；当前取消通过 O(n)
  扫描定位对应定时器；
- 其他线程可以入队，但只有正在执行 `run()` 的线程会消费队列；
- RunLoop 销毁后继续使用其 Scheduler，或在其所属 RunLoop 未运行时使用 Scheduler，都会
  从 await 表达式抛出 `std::logic_error`；
- 根任务完成时如果仍存在单独排队的工作或待处理 Timer，RunLoop 会拒绝退出，因为本阶段
  不支持 detached 所有权。

RunLoop 不拥有工作线程，也不提供自动线程亲和。外部 awaiter 可以在其他线程恢复 Task；
显式等待原 Scheduler 才会把 continuation 送回对应 RunLoop。如果 Task 挂起后没有安排
其他线程或事件源恢复它，`run()` 可能无限等待。阻塞函数仍会阻塞协程当前所在的线程。

目前没有公共自由函数 `sync_wait`、动态 TaskGroup、detached 执行、独立 Timer 句柄、异步
I/O 后端、自定义协程帧 allocator 或阻塞任务线程池。取消只显式用于定时等待；模块既不
提供隐式取消传播，也没有普通 `schedule()` 的 token 重载，并且没有保留旧脚手架模块的
兼容别名。

捕获变量的协程 lambda 需要特别小心：立即调用一个临时的捕获 lambda，可能使懒协程引用
已经销毁的闭包。CMP 尚未提供延长该闭包生命周期的辅助函数。

CMP 名称中的 `C` 与 Go 运行时中的 `G` 相呼应，但这只说明命名来源，不表示两者语义等价。
标准 C++ 协程提供挂起和恢复机制，本身不包含调度器，也不会把阻塞操作自动变成异步操作。

## 可选的后续工作

以下方向可以分别设计和评审，目前都不是包的既有约定：

1. 动态结构化任务作用域和取消传播；
2. 更多结构化唤醒路径；
3. 多工作线程调度和工作窃取；
4. 异步 I/O 集成；
5. 处理不可避免的阻塞工作的专用线程池；
6. 结果适配器和可选的协程帧分配策略。

Task、RunLoop 与 `when_all` 已经形成真实的公共边界，因此分别位于模块分区中。只有其他
已实现 API 确实需要新边界时，才继续增加模块分区或实现单元。

## 验证

在仓库根目录执行：

```text
mcpp build --cache=off
mcpp test --cache=off
cd examples/basic
mcpp run
```

预期结果是库构建成功、三个二进制中的 46 项测试全部通过，并且示例依次输出
`Coroutine result: 42`、`Concurrent result: 42` 和 `Coroutine cancelled` 后以状态 0
退出。测试分别执行一百万次立即完成的 Task、十万次立即双 Task 汇合、十万次显式调度、
十万次立即 Timer 和十万次预先取消的定时等待，用于检查对称转移以及所有汇合或队列路径都
不会增长原生调用栈。当前 Windows LLVM 工具链不会生成 GNU depfile；如果模块接口包含的
文件发生变化，增量构建可能复用旧的 BMI 或目标文件。完整复验时使用 `--cache=off`。
