# CMP v1 可开发性与压力基线

**日期：** 2026-08-29

**结论：** Phase 6B TCP client 迁移后的本机 5 轮 Release 压测全部通过；三类场景均无
非预期失败。

## 测试边界

本报告验证 CMP v1 的结构化生命周期、调度、跨线程唤醒和失败传播能够承载计算任务；文件
和 TCP loopback server 继续通过专用 CMP `ThreadPool` 与 `run_blocking()` 隔离，TCP client
则使用 Phase 6B `IoContext`/`TcpStream` 原生异步路径。

压测程序位于 `benchmarks/v1-readiness`，只直接使用标准库、CMP 和服务端所需的 POSIX 回环
socket；Asio 由 CMP 路径依赖传递，不在 benchmark 重复声明。该 benchmark 不进入跨平台 CI。
任何计数不匹配或非预期失败都会让进程以非零状态退出。

## 环境与复现

- 系统：WSL2 Linux 5.15.153.1，x86_64；
- 处理器：AMD Ryzen 7 7735H，8 核 16 线程；
- 内存：约 11.7 GiB；
- 临时目录文件系统：ext4；
- mcpp：2026.8.11.2；
- 编译器：LLVM 22.1.8；
- 阻塞 worker：4；
- 原生异步 I/O driver：1；
- 配置：Release，关闭 mcpp 构建缓存。

```bash
cd benchmarks/v1-readiness
mcpp build --profile release --strict --cache=off
./target/x86_64-linux-gnu/937ff8fc1b673bcb/bin/cmp-v1-readiness
```

目标目录中的平台哈希可能随环境变化；如果路径不同，使用构建输出给出的可执行文件路径。

## 负载

- `compute`：50,000 个经 `Scheduler` 调度的 Task，每个成功任务执行 128 轮整数运算；每 50 个
  任务注入一个预期异常并在结构化子任务内捕获。
- `file_io`：4 个结构化 offload，各在专用 ThreadPool worker 上执行 250 次 64 KiB 临时文件
  写入、读取和内容校验，再执行 25 次缺失文件读取以验证预期失败路径。
- `network_loopback`：4 个同步 echo server 各占用一个专用 ThreadPool worker；4 个
  `TcpStream` 共用一个 `IoContext`，各执行 5,000 次 256 字节回环请求/响应，再对被占用但未
  监听的端口执行 25 次连接，并且只把 `connection_refused` 计为预期失败。

吞吐量按“成功操作 + 预期失败操作”的总操作数计算。

## 五轮原始结果

| 轮次 | 场景 | 操作数 | 成功 | 预期失败 | 非预期失败 | 耗时 ms | ops/s | 状态 |
| ---: | --- | ---: | ---: | ---: | ---: | ---: | ---: | --- |
| 1 | compute | 50,000 | 49,000 | 1,000 | 0 | 32.879 | 1,520,728.9 | PASS |
| 1 | file_io | 1,100 | 1,000 | 100 | 0 | 176.548 | 6,230.6 | PASS |
| 1 | network_loopback | 20,100 | 20,000 | 100 | 0 | 2,016.219 | 9,969.2 | PASS |
| 2 | compute | 50,000 | 49,000 | 1,000 | 0 | 27.174 | 1,839,973.8 | PASS |
| 2 | file_io | 1,100 | 1,000 | 100 | 0 | 167.446 | 6,569.3 | PASS |
| 2 | network_loopback | 20,100 | 20,000 | 100 | 0 | 2,071.085 | 9,705.1 | PASS |
| 3 | compute | 50,000 | 49,000 | 1,000 | 0 | 29.339 | 1,704,210.2 | PASS |
| 3 | file_io | 1,100 | 1,000 | 100 | 0 | 189.751 | 5,797.1 | PASS |
| 3 | network_loopback | 20,100 | 20,000 | 100 | 0 | 2,026.998 | 9,916.1 | PASS |
| 4 | compute | 50,000 | 49,000 | 1,000 | 0 | 28.291 | 1,767,337.2 | PASS |
| 4 | file_io | 1,100 | 1,000 | 100 | 0 | 970.697 | 1,133.2 | PASS |
| 4 | network_loopback | 20,100 | 20,000 | 100 | 0 | 2,353.919 | 8,538.9 | PASS |
| 5 | compute | 50,000 | 49,000 | 1,000 | 0 | 30.551 | 1,636,609.6 | PASS |
| 5 | file_io | 1,100 | 1,000 | 100 | 0 | 457.299 | 2,405.4 | PASS |
| 5 | network_loopback | 20,100 | 20,000 | 100 | 0 | 2,503.267 | 8,029.5 | PASS |

## 汇总

| 场景 | 最短 / 中位 / 最长耗时 ms | 最高 / 中位 / 最低 ops/s | 五轮非预期失败 |
| --- | ---: | ---: | ---: |
| compute | 27.174 / 29.339 / 32.879 | 1,839,973.8 / 1,704,210.2 / 1,520,728.9 | 0 |
| file_io | 167.446 / 189.751 / 970.697 | 6,569.3 / 5,797.1 / 1,133.2 | 0 |
| network_loopback | 2,016.219 / 2,071.085 / 2,503.267 | 9,969.2 / 9,705.1 / 8,029.5 | 0 |

相同负载的 Phase 6A 阻塞 client 历史中位数为 2,627.611 ms / 7,649.5 ops/s；Phase 6B 原生
异步 client 的本轮中位数为 2,071.085 ms / 9,705.1 ops/s。该差异只作为本机回归证据，不是
跨机器 SLA 或 CI 阈值。结果说明 v1 核心可以同时承载结构化协程、blocking offload 和
单 driver 原生异步 TCP；work stealing 仍需由独立的代表性负载证明必要性。
