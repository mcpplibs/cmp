# CMP v1 可开发性与压力基线

**日期：** 2026-08-30

**结论：** Phase 6C TCP client/server 最终本地门禁的 5 轮 Release 压测全部通过；三类场景
均无非预期失败。

## 测试边界

本报告验证 CMP v1 的结构化生命周期、调度、跨线程唤醒和失败传播能够承载计算任务；文件
继续通过专用 CMP `ThreadPool` 与 `run_blocking()` 隔离，TCP loopback client/server 则使用
Phase 6B/6C `IoContext`、`TcpStream` 与 `TcpListener` 原生异步路径。

压测程序位于 `benchmarks/v1-readiness`，只直接使用标准库、CMP，以及构造未监听失败端口所需
的 POSIX socket；正常 TCP 服务端不再使用阻塞 socket。Asio 由 CMP 路径依赖传递，不在
benchmark 重复声明。该 benchmark 不进入跨平台 CI。任何计数不匹配或非预期失败都会让进程
以非零状态退出。

## 环境与复现

- 系统：WSL2 Linux 5.15.153.1，x86_64；
- 处理器：AMD Ryzen 7 7735H，8 核 16 线程；
- 内存：约 11.7 GiB；
- 临时目录文件系统：ext4；
- mcpp：2026.8.28.1；
- 编译器：LLVM 22.1.8；
- 阻塞 worker：4；
- 原生异步 I/O driver：1；
- 配置：Release，关闭 mcpp 构建缓存。

```bash
cd benchmarks/v1-readiness
mcpp build --profile release --strict --cache=off
./target/x86_64-linux-gnu/4b99bcba98e861c5/bin/cmp-v1-readiness
```

目标目录中的平台哈希可能随环境变化；如果路径不同，使用构建输出给出的可执行文件路径。

## 负载

- `compute`：50,000 个经 `Scheduler` 调度的 Task，每个成功任务执行 128 轮整数运算；每 50 个
  任务注入一个预期异常并在结构化子任务内捕获。
- `file_io`：4 个结构化 offload，各在专用 ThreadPool worker 上执行 250 次 64 KiB 临时文件
  写入、读取和内容校验，再执行 25 次缺失文件读取以验证预期失败路径。
- `network_loopback`：4 个 `TcpListener`/服务端 `TcpStream` 与 4 个客户端 `TcpStream` 共用
  一个 `IoContext`，各执行 5,000 次 256 字节回环请求/响应，不占用 blocking worker；随后对
  被占用但未监听的端口执行 25 次连接，并且只把 `connection_refused` 计为预期失败。

吞吐量按“成功操作 + 预期失败操作”的总操作数计算。

## 最终门禁五轮原始结果

| 轮次 | 场景 | 操作数 | 成功 | 预期失败 | 非预期失败 | 耗时 ms | ops/s | 状态 |
| ---: | --- | ---: | ---: | ---: | ---: | ---: | ---: | --- |
| 1 | compute | 50,000 | 49,000 | 1,000 | 0 | 25.998 | 1,923,205.6 | PASS |
| 1 | file_io | 1,100 | 1,000 | 100 | 0 | 183.207 | 6,004.1 | PASS |
| 1 | network_loopback | 20,100 | 20,000 | 100 | 0 | 2,239.147 | 8,976.6 | PASS |
| 2 | compute | 50,000 | 49,000 | 1,000 | 0 | 25.443 | 1,965,140.8 | PASS |
| 2 | file_io | 1,100 | 1,000 | 100 | 0 | 147.661 | 7,449.5 | PASS |
| 2 | network_loopback | 20,100 | 20,000 | 100 | 0 | 2,286.284 | 8,791.6 | PASS |
| 3 | compute | 50,000 | 49,000 | 1,000 | 0 | 25.246 | 1,980,522.4 | PASS |
| 3 | file_io | 1,100 | 1,000 | 100 | 0 | 149.355 | 7,365.0 | PASS |
| 3 | network_loopback | 20,100 | 20,000 | 100 | 0 | 2,297.859 | 8,747.3 | PASS |
| 4 | compute | 50,000 | 49,000 | 1,000 | 0 | 26.662 | 1,875,306.9 | PASS |
| 4 | file_io | 1,100 | 1,000 | 100 | 0 | 138.760 | 7,927.4 | PASS |
| 4 | network_loopback | 20,100 | 20,000 | 100 | 0 | 2,236.655 | 8,986.6 | PASS |
| 5 | compute | 50,000 | 49,000 | 1,000 | 0 | 24.359 | 2,052,612.2 | PASS |
| 5 | file_io | 1,100 | 1,000 | 100 | 0 | 131.145 | 8,387.6 | PASS |
| 5 | network_loopback | 20,100 | 20,000 | 100 | 0 | 2,169.108 | 9,266.5 | PASS |

## 汇总

| 场景 | 最短 / 中位 / 最长耗时 ms | 最高 / 中位 / 最低 ops/s | 五轮非预期失败 |
| --- | ---: | ---: | ---: |
| compute | 24.359 / 25.443 / 26.662 | 2,052,612.2 / 1,965,140.8 / 1,875,306.9 | 0 |
| file_io | 131.145 / 147.661 / 183.207 | 8,387.6 / 7,449.5 / 6,004.1 | 0 |
| network_loopback | 2,169.108 / 2,239.147 / 2,297.859 | 9,266.5 / 8,976.6 / 8,747.3 | 0 |

相同负载的 Phase 6A 阻塞 client 历史中位数为 2,627.611 ms / 7,649.5 ops/s；Phase 6B
“原生异步 client + 4 个阻塞 server”中位数为 2,078.127 ms / 9,672.2 ops/s；Phase 6C
client/server 都由单 driver 原生异步驱动后的中位数为 2,239.147 ms / 8,976.6 ops/s。Phase 6C
减少了 4 个阻塞 worker，但每轮请求增加了服务端 return-Scheduler 跳转，因此本机吞吐低于
Phase 6B；这些数据只作为回归证据，不是跨机器 SLA 或 CI 阈值。结果说明 v1 核心可同时承载
结构化协程、blocking offload 和单 driver 原生异步 TCP client/server；只有代表性负载确认
该单 driver 是瓶颈后，才设计多 driver 或其他调度优化。
