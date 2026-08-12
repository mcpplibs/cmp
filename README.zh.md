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
> CMP 已提供懒启动、单消费者的 `Task<T>` / `Task<void>`，以及在调用线程运行、支持显式
> 调度的 `RunLoop`。取消、定时器、异步 I/O 和 detached 执行尚未实现。

CMP 计划基于标准无栈 C++ 协程构建现代协程运行时和库。项目将以显式 `co_await` 为主线，
通过经过验证的小步骤逐步探索调度、定时器、异步 I/O、取消以及阻塞工作的安全隔离。

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
- M:N 调度、work stealing、定时器、取消或异步 I/O 已经实现。

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

示例会从经过调度的 `Task<void>` 协程内部打印 `Coroutine result: 42`，然后以成功状态退出。
它负责证明独立 mcpp 包能够解析路径依赖、导入 `mcpplibs.cmp`、组合 Task 并通过公共
RunLoop 驱动任务。

## 当前 API

```cpp
import std;
import mcpplibs.cmp;

using mcpplibs::cmp::Task;
using mcpplibs::cmp::RunLoop;

Task<int> answer() {
    co_return 42;
}

Task<void> print_answer(RunLoop::Scheduler scheduler) {
    co_await scheduler.schedule();
    auto value = co_await answer();
    std::println("Coroutine result: {}", value);
    co_return;
}

int main() {
    RunLoop loop {};
    loop.run(print_answer(loop.get_scheduler()));
}
```

`Task` 采用懒启动：调用 `answer()` 只创建处于挂起状态的协程，在被 `co_await` 消费时才
开始执行。Task 只能移动、只有一个消费者且只能作为右值等待；它保存值或异常，在子协程与
continuation 之间直接转移，并通过 RAII 销毁未消费的协程帧。当前刻意不支持 `Task<T&>`、
复制、移动赋值和 detached 执行。

定义协程的翻译单元必须导入 `std`，使编译器能够看到标准协程协议类型。CMP 私有导入
`std`，不会向使用方重新导出整个标准库。

`RunLoop::run()` 消费一个根 Task，在调用线程执行就绪协程，返回结果并重新抛出异常。
`Scheduler::schedule()` 始终挂起当前协程并把 continuation 放入队列。Scheduler 可以复制，
但始终属于创建它的 RunLoop。支持顺序多次调用 `run()`，嵌套或并发调用会被拒绝。已经被
移动的 Task 不得再次等待。

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
├── tests/cmp_test.cpp        # Task 契约和生命周期测试
├── tests/run_loop_test.cpp   # 调度、边界和线程测试
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
4. 定时器、取消和结构化唤醒路径；
5. 多 worker 调度与 work stealing；
6. 异步 I/O 集成和 blocking pool。

剩余顺序只是方向，不代表列出的能力已经实现。

## 参与贡献

修改模块边界前请先阅读[架构说明](docs/architecture.zh.md)。保持改动小而清晰，遵循 C++23
模块约定，并以 `mcpp build`、`mcpp test`、独立示例和 CI 结果作为实现事实。

## 许可证

[Apache License 2.0](LICENSE)
