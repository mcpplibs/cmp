# 架构

[English](architecture.md) · **简体中文** · [繁體中文](architecture.zh.hant.md)

## 当前状态

CMP 目前是一个初始的 C++23 模块项目。包可以构建和导入，但根模块还没有公共声明，
协程运行时也尚未实现。

仓库现有内容包括：

- 一份 mcpp 包清单；
- 仅包含模块声明的根模块 `mcpplibs.cmp`；
- 一个 gtest 导入测试；
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
`cmp`，在 C++ 源码中导入 `mcpplibs.cmp`。`mcpplibs::cmp` 留作以后公共 C++ 声明
使用。

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
`mcpp test` 发现 `tests/cmp_test.cpp`，链接 gtest 入口，并验证另一个翻译单元可以导入
`mcpplibs.cmp`。

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
import mcpplibs.cmp;

int main() {
    return 0;
}
```

该示例在根测试目标之外，单独检查路径依赖解析和模块使用。

## 当前边界

根模块目前没有提供 `task`、promise 类型、调度器、定时器、取消机制、异步 I/O 后端或
阻塞任务线程池，也没有保留旧脚手架模块的兼容别名。

CMP 名称中的 `C` 与 Go 运行时中的 `G` 相呼应，但这只说明命名来源，不表示两者语义等价。
标准 C++ 协程提供挂起和恢复机制，本身不包含调度器，也不会把阻塞操作自动变成异步操作。

## 可选的后续工作

以下方向可以分别设计和评审，目前都不是包的既有约定：

1. 协程任务的所有权、完成和生命周期规则；
2. 单线程调度器和显式调度 awaiter；
3. 定时器、唤醒路径和取消；
4. 多工作线程调度和工作窃取；
5. 异步 I/O 集成；
6. 处理不可避免的阻塞工作的专用线程池。

只有已实现的 API 确实需要新的边界时，才增加模块分区或实现单元。

## 验证

在仓库根目录执行：

```text
mcpp build --cache=off
mcpp test --cache=off
cd examples/basic
mcpp run
```

预期结果是库构建成功、一个导入测试通过，并且示例以状态 0 退出。当前 Windows LLVM
工具链不会生成 GNU depfile；如果模块接口包含的文件发生变化，增量构建可能复用旧的 BMI
或目标文件。完整复验时使用 `--cache=off`。
