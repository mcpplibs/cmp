# CMP 当前工作状态

## 项目概览

CMP 是使用 mcpp 构建的 C++23 Modules 协程运行时库，公开模块为 `mcpplibs.cmp`。当前已实现
懒启动单消费者 `Task<T>`、变参/vector `when_all()`、静止点 `TaskGroup`、一次性与可复用
事件、RAII `AsyncMutex`、带定时和取消的调用线程 `RunLoop`、固定大小的 `ThreadPool`，以及
用于隔离同步调用的 `run_blocking()`。Phase 6B 已加入拥有单一私有 I/O driver 的
`IoContext`，以及数值地址 TCP client 的 connect/read/write、取消和关闭生命周期。

`.xlings.json` 固定 mcpp 2026.8.11.2；当前工具链为 LLVM 22.1.8，运行时依赖为
`chriskohlhoff.asio` 1.38.1，测试依赖为 `compat.gtest` 1.15.2。`examples/basic` 是独立
path-dependency consumer。

## 当前目标与状态

Phase 6A blocking offload v1 与 Phase 6B 第 1–5 项已作为本地提交完成，最新为 Step 5
`a73c113`，均尚未推送。Phase 6B Plan 第 6 项 close/cancellation/shutdown 竞态已实现并通过
本地门禁；下一步是第 7 项并发负载与跨平台门禁准备。

## 已完成工作

- 新增 `mcpplibs.cmp:blocking` 分区并由根模块导出一个公共函数模板：
  `run_blocking(blockingWorkers, returnTo, operation, stopToken)`。
- helper 按值持有 callable，支持 `void`、可移动值、move-only callable/result 和异常传播；
  callable 在指定 ThreadPool 上执行一次，结果只在显式返回 Scheduler 上发布。
- 复用现有 `ThreadPool` 的调度、取消、队列和关闭契约；没有新增 `BlockingPool`、隐藏全局
  executor、future、类型擦除、依赖或 manifest 配置。
- 新增 11 项确定性测试，覆盖懒启动、worker/返回线程亲和、返回另一 ThreadPool、值/void、
  move-only、异常、预取消/排队取消/晚取消、取消竞态、过期与 inactive Scheduler、RunLoop
  响应性，以及 5,000 个并发 offload 的 exactly-once 行为。
- `examples/basic` 新增独立 blocking ThreadPool，并在协程中打印 `Blocking result: 42`。
- `benchmarks/v1-readiness` 的文件与回环网络场景已从手写 `jthread` adapter 迁移到一个专用
  CMP ThreadPool 和 `run_blocking()`；负载及成功/失败硬检查保持不变。
- 三份 README、三份架构文档、Phase 6A Design/Plan 和 v1 readiness 数据报告已同步到实现
  事实。
- 完成 Phase 6B 依赖核查：mcpp-index 当前提供 `chriskohlhoff.asio@1.38.1`，其 C++23 模块
  `asio` 覆盖 Linux、macOS 和 Windows，能够复用 epoll、kqueue 与 IOCP 后端。
- 新增并批准 Phase 6B TCP client v1 Design，确定首个原生异步 I/O 切片仅包含数值地址 TCP
  客户端、`IoContext`、move-only `TcpStream`、`read_some()`、`write_all()` 和显式返回
  Scheduler。
- Spec 已明确 buffer/handle 生命周期、单读单写并发、EOF/partial transfer、错误映射、
  stop/close/completion 竞态、exactly-once、context drain/join 及三平台确定性测试门槛。
- 新增 Phase 6B Implementation Plan，将实现拆为 dependency/backend gate、`IoContext`
  生命周期、单一 native-completion bridge、connect/read/write、取消/close/shutdown 竞态、
  跨平台负载、readiness 客户端迁移、文档和完整验证十个顺序步骤。
- 使用 mcpp 2026.8.11.2 精确加入 `[dependencies.chriskohlhoff] asio = "1.38.1"`。
- 新增 `mcpplibs.cmp:tcp` 分区，私有导入 `std`、`asio`、`:cancellation` 和 `:task`；根模块
  只重导出 `:tcp`，没有暴露 Asio 类型。
- 实现不可复制、不可移动的公共 `IoContext`：共享私有状态拥有一个 `asio::io_context`、
  persistent work guard、admission mutex/flag 和线性弱 socket registry；公开对象拥有一个
  `std::jthread` driver。
- `IoContext` shutdown 与普通 post 共用 admission 锁排序：先停止接纳，再在已接纳工作后关闭
  live socket、释放 guard、自然排空 handler 并 join；不调用 `io_context::stop()`，driver
  线程内自析构会终止而不是 self-join。
- 新增 `tests/tcp_test.cpp`，以编译期断言锁定 `IoContext` 的默认构造及精确 copy/move traits，
  并以运行测试验证构造、driver 启动、析构关闭和 join 不挂起。
- 实现唯一的私有 `NativeOperationAwaiter`：一个共享 operation state 保存 continuation、Asio
  cancellation signal、原始 error/count、取消来源、同步 initiation 异常和 stop callback。
- completion handler 以一参数/两参数重载把 connect 与 read/write 统一为
  `std::error_code + transferred bytes`；native initiation 成功后，只有该 handler 可以恢复
  私有协程，stop/close/shutdown 只能请求 native completion。
- stop callback 只持有 operation/context 弱引用，并把 stop-token cancellation 排到 I/O 线程
  后再记录来源、发出 `cancellation_type::all`；context 已停止接纳时由 shutdown close 路径
  完成 operation。
- Step 3 曾加入内部具体模板实例作为 bridge 编译门禁，Step 4 接入真实 connect initiation 后已
  删除；没有为私有 bridge 暴露测试 API。
- 新增不可默认构造、不可复制、可 `noexcept` 移动构造且不可移动赋值的 `TcpStream`；公开
  `connect()` 是非协程包装器，在返回 lazy Task 前复制地址、Scheduler、token 与 context 弱句柄。
- `connect()` 被 context 接纳后才在 I/O 线程检查预取消、解析 numeric IPv4/IPv6、创建并注册
  socket、发起 `async_connect()`；预取消不解析地址或打开 socket，系统错误保留原 error code。
- native operation state 新增私有生命周期锚点，保证临时 connect socket 存活到 handler；成功
  后先经显式 return Scheduler 再发布 move-only stream，return Scheduler 失败由内部 RAII 关闭。
- `tests/tcp_test.cpp` 扩展为单一同步 Asio loopback fixture，覆盖 laziness/临时地址所有权、IPv4、
  可用时 IPv6、拒绝连接、预取消、RunLoop/ThreadPool 亲和及 inactive/expired Scheduler。
- 新增 lazy `read_some()` 与 `write_all()` 非协程包装器；buffer span、Scheduler、token 与 socket
  state 均在调用时按值捕获，实际 buffer 仍按 API 契约借用到 Task 完成。
- socket state 使用每方向一个 atomic admission flag，允许一读一写并行并拒绝同方向重叠；flag
  保持到 return-Scheduler 跳转结束，跳转成功或失败均释放。
- read 实现 empty、partial、bytes-before-error、一次 retained fatal error 与 sticky EOF；远端 EOF
  不关闭写方向，后续整条流关闭又不会被旧 sticky EOF 掩盖。
- write 使用 Asio `async_write()` 完成全 buffer；已发起 write 的取消或错误关闭 stream，成功计数
  不等于 buffer 大小时视为内部不变量破坏。
- 当前 libc++ `std` 模块不提供 C++23 `std::scope_exit`；按实测改为在 return-Scheduler 成功与
  异常两条路径显式释放 admission，没有新增 guard abstraction 或依赖。
- 同一 loopback fixture 增加小型同步 protocol callback、partial/EOF/duplex gate 与
  `reuse_address`，没有增加 public server 或固定 sleep。
- 新增线程安全、幂等、非阻塞且 `noexcept` 的 `TcpStream::close()` 与 `is_open()`；析构复用
  同一关闭请求，move 后仅新 handle 保有关闭所有权。
- socket 以弱引用记录 connect/read/write operation；stop、显式 close、write cancellation 与
  context shutdown 在 I/O 线程记录首个本地取消来源，再由唯一 native handler 恢复协程。
- 显式关闭请求使用原子标记覆盖公开校验到 native initiation 的窄窗口；已接纳操作在 close
  获胜时稳定得到 `OperationCancelled`，而无关 socket error 仍保留 `system_error`。
- TCP 测试新增 active/pre-cancellation、关闭幂等、move、write cancellation、两类完成竞态、
  context drain/join 和 surviving handle 边界，当前 `tcp_test` 为 23 项。

## 重要决策

- 独立的普通 `ThreadPool` 实例就是 blocking pool；不维护行为相同的第二种线程池类型。
- `run_blocking()` 是懒 `Task`，callable 和两个 Scheduler 都按值进入协程帧，避免临时对象
  悬空。
- 第一次调度接受可选 `std::stop_token`；取消只能跳过尚未被 worker claim 的 callable。
  已经开始的同步调用不能被抢占。
- 返回调度不接受取消 token，确保值、异常或取消结果不会滞留在 blocking worker。
- 返回 Scheduler 失败时直接传播其异常；失效的完成上下文本身无法被 helper 修复。
- Phase 6A 仍是每个运行中同步调用占用一个系统线程的隔离方案，不宣称原生非阻塞 I/O。
- 队列沿用 ThreadPool 的无界共享 FIFO；背压、超时、强制中断和 worker replacement 均未在
  没有实测需求前增加。
- Phase 6B 先做 TCP 而非文件：现有 Asio package 的 socket 能统一覆盖三平台，file backend
  不能提供同等平台面。
- C++23 标准库没有网络 API，因此 6B 选择精确固定 `chriskohlhoff.asio@1.38.1`，私有导入，
  不向 CMP 公共 API 暴露 Asio 类型；不自研三套 OS backend。
- `IoContext` v1 固定一个私有 driver thread，不暴露通用 post/run/线程数接口；应用结果始终
  经调用者指定 Scheduler 返回。
- socket state 只弱引用 context；共享 context state 由公开 `IoContext` 与 driver 持有，
  socket 不能延长公开 driver 生命周期。v1 registry 使用 O(n) 弱引用扫描，只有实测成本显著
  时才替换。
- native initiation 成功前的同步异常可以直接恢复私有协程；成功后恢复权只属于 Asio handler。
  stop callback 不从请求线程 emit、访问 socket 或恢复 coroutine。
- 显式 close 立即发布逻辑关闭状态，再把取消与 native close 排到 I/O 线程；资源失效检查先于
  pre-cancellation，远端 EOF 不关闭本地写方向。
- `TcpStream` 允许一项 pending read 与一项 pending write；同方向重叠直接拒绝，不增加隐式
  排队。主动 close、context shutdown 与已发起 write 的取消均有明确关闭语义。
- v1 不含 DNS、server、TLS、UDP、文件 I/O、timeout、socket option 或隐式 executor；这些都
  没有在首个可验证切片前预建。
- 新测试只在 `tests/tcp_test.cpp` 内用同步 Asio loopback fixture；不为测试增加公共 server。
- 现有三平台 CI 会自动发现新测试，不预先修改 workflow；`examples/basic` 保持短小且不引入
  live network，仍作为外部 path consumer 回归运行。
- readiness 只替换 TCP client；同步 loopback server 继续通过 Phase 6A `run_blocking()`
  运行，compute/file workload 与硬计数保持不变。

## 修改 / 重要文件

- 当前实现：`mcpp.toml`、`src/tcp.cppm`、`src/cmp.cppm`
- Phase 6A 核心：`src/blocking.cppm`
- 测试：`tests/blocking_test.cpp`、`tests/tcp_test.cpp`
- 示例：`examples/basic/src/main.cpp`
- 压测：`benchmarks/v1-readiness/src/main.cpp`、
  `docs/benchmarks/2026-08-29-cmp-v1-readiness.md`
- 方案：`docs/superpowers/specs/2026-08-29-cmp-phase6a-blocking-offload-v1-design.md`、
  `docs/superpowers/plans/2026-08-29-cmp-phase6a-blocking-offload-v1.md`
- 已批准设计：
  `docs/superpowers/specs/2026-08-29-cmp-phase6b-tcp-client-v1-design.md`
- 当前实施计划：
  `docs/superpowers/plans/2026-08-30-cmp-phase6b-tcp-client-v1.md`
- 公共文档：`README.md`、`README.zh.md`、`README.zh.hant.md`、`docs/architecture.md`、
  `docs/architecture.zh.md`、`docs/architecture.zh.hant.md`

## 验证情况

- Phase 6A baseline 的 `mcpp build --profile dev --strict --cache=off`：通过。
- Phase 6A baseline 的 `mcpp test --profile dev --strict --cache=off`：9 个二进制、116/116
  通过。
- Phase 6A baseline 的 `mcpp build --profile release --strict --cache=off`：通过。
- Phase 6A baseline 的 `mcpp test --profile release --strict --cache=off`：9 个二进制、
  116/116 通过。
- Dev 定向 `blocking_test`：11/11 通过。
- Release `CancellationRaceInvokesAtMostOnce` 连续执行 100 轮：100/100 通过。
- `examples/basic` 的 `mcpp run`：通过，包含 `Blocking result: 42`，退出码 0。
- `benchmarks/v1-readiness` Release strict 构建通过；迁移后执行 5 轮，compute、file_io 和
  network_loopback 每轮均 PASS，五轮非预期失败总数为 0。
- 文件/网络每轮计数分别为 1,000/20,000 成功、100/100 预期失败、0 非预期失败；原始耗时和
  吞吐已写入 benchmark 报告。
- 当前 mcpp 仍输出 SubOS 缺少 `subos_info` 的既有环境提示，但所有构建和运行成功。
- 以上均为本机 Linux/WSL2 结果；尚未执行 GitHub 三平台 CI 或其他远程操作。
- Phase 6B backend gate：`mcpp build --profile dev --strict --cache=off` 通过；mcpp 下载并编译
  `chriskohlhoff.asio` 1.38.1，CMP 使用 LLVM 22.1.8 构建成功。
- Phase 6B `IoContext` focused gate：`mcpp test tcp_test --profile dev --strict --cache=off` 的
  1/1 测试通过。
- 加入 `IoContext` 后，`mcpp build --profile dev --strict --cache=off` 再次通过；
  `mcpp test --profile dev --strict --cache=off` 为 10 个二进制、117/117 通过。
- 加入 native bridge 后，`mcpp build --profile dev --strict --cache=off` 与
  `mcpp build --profile release --strict --cache=off` 均通过；Dev 与 Release 全量测试均为 10 个
  二进制、117/117 通过。
- connect 定向 `tcp_test` 在 Dev 与 Release 均为 8/8 通过；Release binary 重复执行 100 轮，
  800/800 用例通过。
- 加入 connect 后，Dev 与 Release 的 strict cache-off 全量测试均为 10 个二进制、124/124 通过。
  当前未重复执行 example 或 benchmark；它们保留到 Phase 6B 完整 API 与 readiness 迁移门禁。
- read/write 定向 `tcp_test` 在 Dev 与 Release 均为 15/15 通过；Release 整套重复 10 轮为
  150/150，通过两个 overlap test 重复 50 轮为 100/100。
- 加入 read/write 后，Dev 与 Release 的 strict cache-off 全量测试均为 10 个二进制、131/131
  通过。
- 曾尝试 Release 整套重复 100 轮；前 24 轮 360/360 通过，第 25 轮因本机临时端口范围仅
  `60700–61000` 而出现 `Address already in use`。fixture 随后增加 `reuse_address`，上述 10 轮
  整套与 50 轮 overlap 复验通过；该次资源耗尽不计为功能通过，也未隐藏。
- close/cancellation/shutdown 定向 `tcp_test` 在 Dev 与 Release 均为 23/23 通过；两套 strict
  cache-off 全量测试均为 10 个二进制、139/139 通过。
- Release 两项完成竞态连续执行 5 轮共 10/10 测试通过，覆盖 100 次 close/completion 与
  500 次 stop/completion 竞态，没有丢失或重复完成。

## 已知问题 / 风险

- 运行中的同步调用不可抢占；永久阻塞会永久占用 worker，并使等待它的 Task 和 pool 析构
  无法完成。callable 如需协作取消，必须自行捕获并检查 token。
- ThreadPool 使用无界共享 FIFO；持续生产快于消费时，应用需要限制自己的结构化 in-flight
  数量。
- 返回 Scheduler 的 owner 必须持续存活；RunLoop Scheduler 还必须处于 active `run()` 中。
- `run_blocking()` 是线程隔离，不是 epoll、io_uring、kqueue 或 IOCP 等原生异步 I/O。
- 三平台兼容性仍需远程 CI 确认。
- `chriskohlhoff.asio@1.38.1` 只在本机 Linux/WSL2 + LLVM 22.1.8 完成模块编译；macOS 与
  Windows 仍需后续远程 CI 验证。
- Phase 6B 本地已有 backend、`IoContext`、native bridge、connect/read/write、EOF/overlap、
  pending cancellation、close/is_open 与 shutdown 竞态；并发负载、readiness TCP client 迁移、
  文档同步和最终本地矩阵仍待后续步骤。
- 单 I/O driver 是 v1 的刻意简化；只有 benchmark 证明它是瓶颈后才设计多 driver/strand。

## 剩余工作

1. 按 Phase 6B Plan 第 7 项增加有界的多客户端并发负载测试，并完成两类竞态各至少 100 轮的
   Release 门禁。
2. 继续第 8–10 项：迁移 readiness TCP client、同步六份开发文档并执行完整本地验证矩阵。
3. 本地完成后仍需用户另行授权 push/PR，才能取得 Linux、macOS、Windows 远程 CI 结果。

## 推荐下一步

按 `2026-08-30-cmp-phase6b-tcp-client-v1.md` 开始第 7 项，在现有 loopback fixture 内加入
跨平台有界并发客户端门禁，不增加 public server、通用 executor 或新依赖。
