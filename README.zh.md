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
> CMP 当前处于 **bootstrap 阶段**。包已经导出根模块 `mcpplibs.cmp`，但尚未提供协程运行时 API。

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

示例会以成功状态退出且不产生输出。它只负责证明独立 mcpp 包能够解析路径依赖并导入
`mcpplibs.cmp`。

## 当前模块

```cpp
import mcpplibs.cmp;

int main() {
    return 0;
}
```

bootstrap 阶段的模块刻意不包含公开声明。未来公共 API 将使用 `mcpplibs::cmp` 命名空间。

## 仓库结构

```text
.
├── .xlings.json              # 固定的项目工具环境
├── mcpp.toml                 # 包身份和测试依赖
├── src/cmp.cppm              # 根模块接口
├── tests/cmp_test.cpp        # 导入 smoke 测试
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
`[dependencies]`，测试专用依赖放在 `[dev-dependencies]`。

## 路线图

运行时工作将拆分为可以独立审查的阶段：

1. 包身份和可导入模块 bootstrap；
2. 协程 task 与生命周期语义；
3. 最小单线程调度器；
4. 定时器、取消和结构化唤醒路径；
5. 多 worker 调度与 work stealing；
6. 异步 I/O 集成和 blocking pool。

bootstrap 之后的顺序只是方向，不代表这些能力已经实现。

## 参与贡献

修改模块边界前请先阅读[架构说明](docs/architecture.zh.md)。保持改动小而清晰，遵循 C++23
模块约定，并以 `mcpp build`、`mcpp test`、独立示例和 CI 结果作为实现事实。

## 许可证

[Apache License 2.0](LICENSE)
