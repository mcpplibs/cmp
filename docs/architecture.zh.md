# 架构

[English](architecture.md) · **简体中文** · [繁體中文](architecture.zh.hant.md)

## 当前状态

CMP 是一个 C++23 模块项目，已经实现首个运行时基础类型。根模块导出懒启动、单消费者的
`mcpplibs::cmp::Task<T>` 及其 `Task<void>` 特化，但尚未提供调度器或根任务执行 API。

仓库现有内容包括：

- 一份 mcpp 包清单；
- 根模块 `mcpplibs.cmp` 及其 Task 实现；
- 覆盖契约、生命周期、异常和对称转移的 gtest 测试；
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

`gtest = "1.15.2"` 是测试使用的开发依赖。CMP 当前不跟踪 `mcpp.lock`，该文件由
`.gitignore` 排除。

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
├── src/cmp.cppm
├── tests/cmp_test.cpp
└── mcpp.toml
```

## 构建与测试

`.xlings.json` 固定项目使用的 mcpp 版本。`mcpp build` 构建自动推断的库目标。
`mcpp test` 发现 `tests/cmp_test.cpp`，链接 gtest 入口，并验证 Task 类型契约、懒执行、
协程帧所有权、值与异常传播、嵌套组合以及不会增长调用栈的对称转移。

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

Task<int> answer() {
    co_return 42;
}

Task<void> print_answer() {
    auto value = co_await answer();
    std::println("Coroutine result: {}", value);
    co_return;
}
```

该示例在根测试目标之外，单独检查路径依赖解析、模块使用和外部协程编译。示例私有的
`InlineRunner` 启动一个 eager 根协程，等待 `print_answer()` 并输出 `Coroutine result: 42`。
这个辅助类型只适用于当前同步完成且没有调度器的协程链，不属于 CMP 公共 API。

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

被移动后的 Task 为空，不得再次等待；当前实现会在违反该契约时终止进程。目前没有公共
`sync_wait`、detached 执行、调度器、线程亲和保证、定时器、取消机制、异步 I/O 后端、
自定义协程帧 allocator 或阻塞任务线程池，也没有保留旧脚手架模块的兼容别名。

捕获变量的协程 lambda 需要特别小心：立即调用一个临时的捕获 lambda，可能使懒协程引用
已经销毁的闭包。CMP 尚未提供延长该闭包生命周期的辅助函数。

CMP 名称中的 `C` 与 Go 运行时中的 `G` 相呼应，但这只说明命名来源，不表示两者语义等价。
标准 C++ 协程提供挂起和恢复机制，本身不包含调度器，也不会把阻塞操作自动变成异步操作。

## 可选的后续工作

以下方向可以分别设计和评审，目前都不是包的既有约定：

1. 根任务驱动器、最小单线程调度器和显式调度 awaiter；
2. 结构化任务作用域和并发汇合；
3. 定时器、唤醒路径和取消；
4. 多工作线程调度和工作窃取；
5. 异步 I/O 集成；
6. 处理不可避免的阻塞工作的专用线程池；
7. 结果适配器和可选的协程帧分配策略。

只有已实现的 API 确实需要新的边界时，才增加模块分区或实现单元。

## 验证

在仓库根目录执行：

```text
mcpp build --cache=off
mcpp test --cache=off
cd examples/basic
mcpp run
```

预期结果是库构建成功、八个 Task 测试通过，并且示例以状态 0 退出。其中一个测试执行一百万次
立即完成的 Task，用于检查对称转移不会增长原生调用栈。当前 Windows LLVM 工具链不会生成
GNU depfile；如果模块接口包含的文件发生变化，增量构建可能复用旧的 BMI 或目标文件。
完整复验时使用 `--cache=off`。
