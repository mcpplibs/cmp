# CMP

> **Coroutine Machine Process**——一個基於 C++23 的協程執行期專案。

[![C++23](https://img.shields.io/badge/C%2B%2B-23-blue.svg)](https://en.cppreference.com/w/cpp/23)
[![Module](https://img.shields.io/badge/module-mcpplibs.cmp-green.svg)](https://en.cppreference.com/w/cpp/language/modules)
[![License](https://img.shields.io/badge/license-Apache_2.0-blue.svg)](LICENSE)

[English](README.md) · [简体中文](README.zh.md) · **繁體中文**

[mcpp](https://github.com/mcpp-community/mcpp) · [架構](docs/architecture.zh.hant.md) ·
[v1 可開發性壓測](docs/benchmarks/2026-08-29-cmp-v1-readiness.md) ·
[執行緒池壓測](docs/benchmarks/2026-08-29-cmp-thread-pool.md) ·
[Issues](https://github.com/mcpplibs/cmp/issues)

[![ci-linux](https://github.com/mcpplibs/cmp/actions/workflows/ci-linux.yml/badge.svg?branch=main)](https://github.com/mcpplibs/cmp/actions/workflows/ci-linux.yml)
[![ci-macos](https://github.com/mcpplibs/cmp/actions/workflows/ci-macos.yml/badge.svg?branch=main)](https://github.com/mcpplibs/cmp/actions/workflows/ci-macos.yml)
[![ci-windows](https://github.com/mcpplibs/cmp/actions/workflows/ci-windows.yml/badge.svg?branch=main)](https://github.com/mcpplibs/cmp/actions/workflows/ci-windows.yml)

> [!IMPORTANT]
> CMP 已提供延遲啟動、單一消費者的 `Task<T>` / `Task<void>`、支援變參和 vector 的結構化
> `when_all()`、eager 結構化 `TaskGroup`、一次性與可複用事件、RAII `AsyncMutex`，以及在呼叫
> 執行緒運行、支援明確排程和單調時鐘定時排程的 `RunLoop`，以及固定大小的 CPU
> `ThreadPool`。Phase 6B/6C 新增明確的 `IoContext`、move-only `TcpStream` 和 move-only
> `TcpListener`，用於原生非同步的數值位址 TCP client 與 server。`run_blocking()` 可在專用的
> pool 實例上執行同步 callable，並透過
> 明確的返回 Scheduler 交付結果。兩種執行器的就緒排程、定時等待、可複用事件等待、TaskGroup
> 子任務和排隊中的阻塞 offload 可明確使用基於 `std::stop_token` 的協作式取消；TCP 操作採用
> 相同 token 模型。detached 執行和其他原生 I/O 類型尚未實作。

CMP 計畫以標準無堆疊 C++ 協程建構現代協程執行期與函式庫。明確的 `co_await` 模型現已
涵蓋固定與增量結構化並行、一次性事件通知、呼叫執行緒與多 worker 排程、單調時鐘計時器、
可取消等待和阻塞工作的結構化隔離，以及可攜式原生非同步 TCP client 與 listener 切片。

## 為什麼叫 CMP？

CMP 是 **Coroutine Machine Process** 的縮寫。專案計畫以 **C** 表示輕量協程執行單元；
它與 Go runtime 的 **G** 僅有命名和思維模型上的啟發關係，不表示未來的 CMP task
已經等同於 goroutine。

專案遵循以下原則：

- 使用 C++23 標準無堆疊協程和 C++ Modules；
- 透過 `co_await` 和專用 awaiter 明確表達暫停；
- 以附帶測試、可審查的小改動逐步建設執行期；
- 不把使用情境限定為伺服器程式；
- 始終以 mcpp 作為建構和套件管理的唯一事實來源。

## 執行期邊界

C++ 標準協程是語言機制，不是完整執行期。因此 CMP 不會宣稱：

- task 自動等同於 Go goroutine；
- 任意阻塞呼叫會自動成為非阻塞呼叫；
- 可以直接在訊號處理器中安全切換協程；
- task 會隱式遷移、work stealing 已啟用，或所有 I/O 類型都已非同步化。

這些能力必須分別設計和驗證。CMP 目前透過明確的專用 `ThreadPool` 實例和
`run_blocking()` 隔離同步工作。Phase 6B/6C 已提供原生非同步 TCP client 與 listener；DNS、
TLS、檔案 I/O 和協作式安全點仍需分別設計。

## 快速開始

安裝 [xlings](https://github.com/openxlings/xlings)，然後安裝 [`.xlings.json`](.xlings.json)
固定的 mcpp 版本：

```bash
xlings install
mcpp --version
mcpp build
mcpp test
```

執行獨立 consumer：

```bash
cd examples/basic
mcpp run
```

範例會印出 `Coroutine result: 42`、`Concurrent result: 42`、`Worker pool result: 42`、
`Blocking result: 42`、`Task group result: 42` 和 `Recursive group result: 3`，接著透過
`Event signalled` 與 `Reusable event cycles: 2`
展示一次性及可複用通知，再印出 `Mutex result: 42` 和 `Coroutine cancelled`；所有輸出都在
`Task<void>` 協程內部。

## 目前 API

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
using mcpplibs::cmp::TcpListener;
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

`Task` 採延遲啟動：呼叫 `answer()` 只建立處於暫停狀態的協程，在被 `co_await` 消費時才
開始執行。Task 只能移動、只有一個消費者且只能作為右值等待；它保存值或例外，在子協程與
continuation 之間直接轉移，並透過 RAII 銷毀未消費的協程框架。目前刻意不支援 `Task<T&>`、
複製、移動賦值和 detached 執行。

`when_all()` 同樣採延遲啟動。等待它時會由左至右啟動全部輸入 Task，不會等待較早暫停的輸入
完成，並持續持有所有子協程框架，直到全部輸入結束。結果依參數順序組成 `std::tuple`；
`Task<void>` 對應 `std::monostate`。若子任務失敗，仍會先等待全部子任務收尾，再依參數
順序重新拋出第一個例外。具名 Task 必須移動到 `when_all()` 中。執行期數量的同型別任務可以
作為 `std::vector<Task<T>>` 傳入，並依相同索引順序回傳結果 vector；具名輸入 vector 同樣
必須移動。

`TaskGroup::spawn()` 接管一個 `Task<void>` 並立即啟動它。僅能使用一次的 `join()` 會等待 group
到達靜止點；join 等待期間，仍在執行的子任務可以遞迴增加工作。活動計數歸零時接納永久關閉。
TaskGroup 必須在解構前完成 join。`get_stop_token()` 和 `request_stop()` 提供明確的標準取消
通道，`cancel_and_join()` 會先要求該通道再 join。token 不會自動注入。需要傳回子任務結果時
應使用 `when_all()`。

在未 set 的 `OneShotEvent` 上執行 `co_await event` 會無分配地暫停。第一次執行緒安全的 `set()`
會永久設定事件，並在 setter 執行緒恰好恢復每個已註冊等待者一次；後續等待 inline 繼續，後續
set 不做任何事。事件不可移動且必須比等待者活得更久；v1 不提供 reset 或隱式 Scheduler 轉移。

`AsyncManualResetEvent` 提供可複用的 `set()` / `reset()` 週期。`wait(stop_token)` 支援協作式
取消，pending 等待者依 FIFO 順序恢復，set 與 cancel 競態只有一個穩定獲勝方。協程會在呼叫
`set()` 或要求取消的執行緒恢復；需要 RunLoop 親和時應明確等待 Scheduler。事件必須比所有
等待者活得更久。

`auto guard = co_await mutex.lock_async()` 無分配地取得 `AsyncMutex`，並透過 RAII 釋放。競爭
等待者依 FIFO 順序取得所有權並在釋放執行緒恢復；所有權不綁定執行緒。mutex 必須比所有 Guard
和排隊等待者活得更久。v1 不提供手工 unlock、try-lock、取消或隱式 Scheduler 轉移。

定義協程的轉譯單元必須匯入 `std`，使編譯器能夠看到標準協程協定型別。CMP 私下匯入
`std`，不會向使用端重新匯出整個標準函式庫。

`RunLoop::run()` 消費一個根 Task，在呼叫執行緒執行就緒協程，回傳結果並重新拋出例外。
`Scheduler::schedule()` 始終暫停目前協程並把 continuation 放入佇列；它的
`std::stop_token` 多載可以取消該排隊等待。`schedule_after()` 等待相對的 `steady_clock`
時長，`schedule_at()` 等待絕對的單調時鐘時間點。期限到達只讓
任務具備執行資格，不會 inline 恢復。Scheduler 可以複製，但始終屬於建立它的 RunLoop。
支援依序多次呼叫 `run()`，巢狀或並行呼叫會被拒絕。已經被移動的 Task 不得再次等待。

所有接受 `std::stop_token` 的 Scheduler 多載仍會排隊，包括預先取消的等待。如果取消先於
消費獲勝，等待會拋出 `OperationCancelled`；較晚的停止要求不能取代已經獲勝的完成。停止回呼
只喚醒 RunLoop，使用者協程仍由執行 `run()` 的執行緒恢復。目前取消透過 O(n) 掃描定位佇列項。

`ThreadPool` 啟動固定數量的 CPU worker。其可複製 Scheduler 始終暫停，並可在任意 worker 上
恢復 continuation；需要返回呼叫執行緒時明確等待 `RunLoop::Scheduler`。
`schedule(stop_token)` 與 RunLoop 就緒排程採用相同的完成/取消勝者規則。v1 使用一個
work-conserving 共用 FIFO 和休眠 worker；解構關閉接納、排空全部已接納項並 join worker。
ThreadPool 不擁有上層 Task，阻塞呼叫仍會阻塞目前 worker。

`run_blocking(blockingWorkers, returnTo, operation, stopToken)` 在其延遲 Task 框架中持有同步
callable，將它排程到指定 ThreadPool 恰好執行一次，並在透過 `returnTo` 排程回來後交付值、
`void`、例外或排隊取消。應使用獨立的 ThreadPool 實例隔離阻塞工作，避免佔用延遲敏感的 CPU
worker。停止要求可以跳過尚未被 worker 領取的工作，但不能搶佔已經執行的同步呼叫；返回排程
刻意不可取消。這是基於執行緒的隔離，不是原生非阻塞 I/O。

原生 TCP 介面保持明確且精簡：

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

Task<TcpStream> accept_one(
    IoContext& io,
    RunLoop::Scheduler caller,
    std::uint16_t port,
    std::stop_token token = {}) {
    auto listener = co_await TcpListener::bind(
        io, caller, "127.0.0.1", port, token);
    co_return co_await listener.accept(caller, token);
}
```

一個不可移動的 `IoContext` 可供多個 stream 共用，並擁有一個私有 I/O driver。`TcpStream` 只能
移動；connect 只接受數值 IPv4/IPv6 文字，不會隱藏阻塞 DNS。read/write span 的底層儲存必須
保持到返回的 Task 完成。每種成功或錯誤都會嘗試明確返回 Scheduler。一個 read 和一個 write
可以並存；同方向第二個操作拋出 `std::logic_error`。預取消不會關閉仍可用的 stream，而已發起
的 `write_all()` 被取消後會關閉 stream。`close()` 執行緒安全、冪等；當 close 獲勝時，已接納
操作恰好一次以 `OperationCancelled` 完成。`TcpListener::bind()` 完成數值位址的
bind-and-listen，允許連接埠零，並由 `local_port()` 返回實際連接埠。同一 listener 只允許一個
pending accept；預取消或主動取消 accept 後 listener 仍可使用，close 會取消 pending accept。
已接受 stream 與 listener 生命週期獨立。Phase 6A 在執行緒池上隔離任意同步呼叫；Phase 6B/6C
提供原生非同步 TCP，不是通用非同步 I/O 層。

RunLoop 不是背景執行緒，也不會把阻塞程式碼自動變成非同步程式碼。如果 Task 暫停後沒有
安排未來的恢復動作，`run()` 可能一直等待。CMP 不提供隱式執行緒親和：外部 awaiter 在其他
執行緒恢復協程後，需要明確等待目標 Scheduler 才會返回對應 RunLoop。

## 儲存庫結構

```text
.
├── .xlings.json                   # 固定的專案工具環境
├── mcpp.toml                      # 套件識別、執行期與測試相依
├── src/cmp.cppm                   # 根模組介面
├── src/task.cppm                  # Task 模組分割區
├── src/cancellation.cppm          # 共用的協作式取消例外
├── src/run_loop.cppm              # RunLoop 與 Scheduler 分割區
├── src/thread_pool.cppm            # 固定大小的 CPU worker 排程器
├── src/blocking.cppm               # 結構化阻塞呼叫 offload
├── src/tcp.cppm                    # 原生非同步 TCP client/server 與 I/O context
├── src/when_all.cppm              # 結構化並行 Task 匯合
├── src/task_group.cppm             # eager 可變結構化 Task 作用域
├── src/one_shot_event.cppm         # 無分配一次性通知
├── src/async_manual_reset_event.cppm # 可複用、可取消通知
├── src/async_mutex.cppm            # FIFO 協程感知 RAII 互斥鎖
├── tests/cmp_test.cpp             # Task 契約和生命週期測試
├── tests/run_loop_test.cpp        # 排程、邊界和執行緒測試
├── tests/thread_pool_test.cpp      # worker、取消和關閉測試
├── tests/blocking_test.cpp         # 阻塞 offload、執行緒親和和取消測試
├── tests/tcp_test.cpp              # TCP 生命週期、取消、競態和負載測試
├── tests/when_all_test.cpp        # 匯合所有權、結果和競態測試
├── tests/task_group_test.cpp       # 可變作用域生命週期和競態測試
├── tests/one_shot_event_test.cpp   # 事件發布和競態測試
├── tests/async_manual_reset_event_test.cpp # 可複用事件競態測試
├── tests/async_mutex_test.cpp       # mutex 所有權和交接測試
├── examples/basic/                # 獨立的路徑相依 consumer
├── benchmarks/v1-readiness/       # 本機計算、檔案和回環網路基線
├── benchmarks/thread-pool/         # 本機多 worker 排程基線
├── docs/architecture.zh.hant.md   # 目前結構、邊界與演進方向
└── .github/workflows/             # Linux、macOS 和 Windows CI
```

儲存庫目前不發布 `mcpp new` 範本。等 CMP 形成值得展示的穩定執行期 API 後，再設計專用範本。

## 開發與驗證

本機驗證路徑：

```bash
mcpp build --profile dev --strict --cache=off
mcpp test --profile dev --strict --cache=off
mcpp build --profile release --strict --cache=off
mcpp test --profile release --strict --cache=off
cd examples/basic
mcpp run
mcpp build --profile release --strict --cache=off
```

CI 在 Linux、macOS 和 Windows 上執行相同的嚴格無快取 Dev/Release 函式庫門禁，同時執行
獨立範例及其嚴格無快取 Release 建構。mcpp 版本由 `.xlings.json` 固定，不應依賴無關的
全域 mcpp 安裝。

CMP 目前不追蹤 `mcpp.lock`，`.gitignore` 明確執行這項儲存庫約定。執行期相依放在
`[dependencies]`，gtest 明確宣告在 `[dev-dependencies.compat]` 中。
目前本機套件包含 10 個測試二進位檔、161 項測試。僅用於 POSIX 的 Release 壓測 consumer 及其
成功/失敗資料記錄在 [v1 可開發性壓測](docs/benchmarks/2026-08-29-cmp-v1-readiness.md)。
多 worker 正確性和效能資料記錄在[執行緒池壓測](docs/benchmarks/2026-08-29-cmp-thread-pool.md)。

## 路線圖

執行期工作拆分為可以獨立審查的階段：

1. 套件識別與可匯入模組 bootstrap——已完成；
2. 協程 task 與生命週期語意——已實作初始 `Task`；
3. 根任務驅動器和最小單執行緒排程器——已完成初始實作；
4. 單調時鐘 Timer v1、可取消就緒/定時等待、變參/vector 匯合、靜止點 TaskGroup、
   OneShotEvent、AsyncManualResetEvent 和 AsyncMutex——已實作並完成壓力驗證；
5. 固定大小的多 worker 排程——已實作並完成壓測；work stealing 仍需 profiling 證據；
6. 結構化阻塞 offload 與數值位址原生非同步 TCP client/listener——均已實作；client 由
   PR #10、listener 由 PR #11 在 Linux、macOS 和 Windows 上完成 CI 驗證。

剩餘順序只是方向，不代表列出的能力已經實作。

## 參與貢獻

修改模組邊界前請先閱讀[架構說明](docs/architecture.zh.hant.md)。保持改動小而清楚，遵循
C++23 模組慣例，並以 `mcpp build`、`mcpp test`、獨立範例和 CI 結果作為實作事實。

## 授權條款

[Apache License 2.0](LICENSE)
