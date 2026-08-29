# 架構

[English](architecture.md) · [简体中文](architecture.zh.md) · **繁體中文**

## 目前狀態

CMP 是一個具備小型協程執行核心的 C++23 模組專案。根模組匯出延遲啟動、單一消費者的
`mcpplibs::cmp::Task<T>`、結構化變參/vector `when_all()`、eager `TaskGroup`、`RunLoop` 及其
可複製的 `Scheduler` 控制代碼、無分配 `OneShotEvent`、可複用
`AsyncManualResetEvent` 和 RAII `AsyncMutex`。匯合原語會持有每個子任務直到結束，事件負責
發布外部訊號。`RunLoop::run()` 是公開根任務執行邊界，`Scheduler::schedule()` 用於明確地
把暫停協程送回對應執行迴圈。`schedule()`、`schedule_after()` 和 `schedule_at()` 都有接受
`std::stop_token` 的協作式取消多載；定時排程使用相對和絕對的 `steady_clock` 期限，且不建立
計時執行緒。

儲存庫現有內容包括：

- 一份 mcpp 套件清單；
- 根模組 `mcpplibs.cmp` 及 Task、cancellation、RunLoop、join、event、mutex 模組分割區；
- 涵蓋契約、生命週期、例外、排程和執行緒行為的 gtest 測試；
- 一個透過路徑相依使用根套件的獨立範例；
- 一個驗證計算、檔案與回環網路整合的本機 POSIX 壓測 consumer；
- Linux、macOS 和 Windows 三套 CI 工作流程。

## 套件和模組識別

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

mcpp 套件由 `mcpplibs` 和 `cmp` 共同識別。使用端在 `[dependencies.mcpplibs]` 中宣告
`cmp`，在 C++ 原始碼中匯入 `mcpplibs.cmp`。公開 C++ 宣告使用 `mcpplibs::cmp` 命名空間。

根模組介面位於 `src/cmp.cppm`，符合 mcpp 預設的函式庫根模組命名規則。儲存庫中沒有
`src/main.cpp`，因此 mcpp 會推斷出名為 `cmp` 的函式庫目標，不需要額外設定 `[lib]` 或
`[targets.cmp]`。雖然 C++23 也是 mcpp 的預設標準，清單中仍明確寫出這項基線。

`compat.gtest = "1.15.2"` 是測試使用的明確命名空間開發相依。CMP 目前不追蹤
`mcpp.lock`，該檔案由 `.gitignore` 排除。

## 儲存庫結構

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
│   ├── cancellation.cppm
│   ├── run_loop.cppm
│   ├── when_all.cppm
│   ├── task_group.cppm
│   ├── one_shot_event.cppm
│   ├── async_manual_reset_event.cppm
│   └── async_mutex.cppm
├── tests/
│   ├── cmp_test.cpp
│   ├── run_loop_test.cpp
│   ├── when_all_test.cpp
│   ├── task_group_test.cpp
│   ├── one_shot_event_test.cpp
│   ├── async_manual_reset_event_test.cpp
│   └── async_mutex_test.cpp
├── benchmarks/v1-readiness/
│   ├── mcpp.toml
│   └── src/main.cpp
└── mcpp.toml
```

## 建置與測試

`.xlings.json` 固定專案使用的 mcpp 版本。`mcpp build` 建置自動推斷的函式庫目標。
`mcpp test` 會找到七個測試檔案，並為每個檔案連結 gtest 進入點。90 項測試同時驗證 Task 所有權
和對稱轉移、結構化匯合，以及根任務執行、普通與定時排程、例外傳播、跨執行緒期限喚醒、
無效 Scheduler、取消競態、RunLoop 重複使用和不會增長呼叫堆疊的重複完成。

三套 CI 工作流程都會安裝專案工具、建置函式庫、執行測試並執行 `examples/basic`。不同
作業系統的工具安裝和執行環境不同，因此分別保留工作流程檔案。

CMP 目前不隨套件發布供 `mcpp new` 使用的專案範本。原始骨架中的 `basic` 和 `lib` 是
通用發布範本，並不反映 CMP 的實際用法。刪除這些範本後，`tools/template_smoke.sh` 已經
沒有可渲染和編譯的對象，因此連同對應的 CI 步驟一起刪除。`examples/basic` 繼續保留，
用來直接驗證套件的使用方式。

`.gitignore` 排除 mcpp 產生物、編譯資料庫、專案工具環境、編譯器和編輯器快取，以及
儲存庫內的本機專案狀態。這些內容由工具產生或只在本機使用，不屬於發布套件。

## 獨立範例

`examples/basic` 有自己的清單，並透過路徑相依引用儲存庫根目錄：

```toml
[package]
name     = "cmp-basic"
standard = "c++23"

[dependencies.mcpplibs]
cmp = { path = "../.." }
```

下面的精簡 API 範例使用與外部專案相同的匯入路徑：

```cpp
import std;
import mcpplibs.cmp;

using mcpplibs::cmp::Task;
using mcpplibs::cmp::RunLoop;
using mcpplibs::cmp::AsyncMutex;
using mcpplibs::cmp::OperationCancelled;
using mcpplibs::cmp::OneShotEvent;
using mcpplibs::cmp::TaskGroup;
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
    RunLoop loop {};
    loop.run(print_answer(loop.get_scheduler()));
    loop.run(print_concurrent_results(loop.get_scheduler()));
    loop.run(print_task_group(loop.get_scheduler()));
    loop.run(print_event(loop.get_scheduler()));
    loop.run(print_mutex(loop.get_scheduler()));

    std::stop_source source {};
    source.request_stop();
    loop.run(print_cancellation(loop.get_scheduler(), source.get_token()));
}
```

這個範例在根測試目標之外，單獨檢查路徑相依解析、模組使用、外部協程編譯和公開根任務
驅動器。RunLoop 依序輸出 `Coroutine result: 42`、`Concurrent result: 42` 和
`Task group result: 42`；遞迴增長的 group 輸出 `Recursive group result: 3`。其他協程展示
一次性及兩輪可複用事件並輸出 `Event signalled`、`Reusable event cycles: 2`，兩個受保護
Task 輸出 `Mutex result: 42`。最後一個結構化 group 使用 `cancel_and_join()`，從可取消就緒
排程捕捉 `OperationCancelled` 並輸出 `Coroutine cancelled`。

任何定義協程的轉譯單元都要自行匯入 `std`，使 `std::coroutine_traits` 和標準協程協定型別
參與編譯。CMP 模組私下匯入 `std`，而不是向使用端重新匯出整個標準函式庫。

## 目前邊界

`Task<T>` 和 `Task<void>` 遵循以下契約：

- 建構時延遲啟動，協程本體在 Task 被等待時開始執行；
- 所有權唯一：Task 可以移動建構，但不能複製或移動賦值；
- `operator co_await()` 僅用於右值，並在等待時消費協程控制代碼；
- 協程框架保存一個值或 `std::exception_ptr`；
- 子協程完成後直接轉移到 continuation，避免遞迴呼叫 `resume()`；
- 未消費的 Task 銷毀自己的框架，消費它的 awaiter 銷毀已完成的框架；
- 拒絕參考和陣列結果型別。

被移動後的 Task 為空，不得再次等待；目前實作會在違反該契約時終止程序。

`when_all()` 遵循以下契約：

- 聚合 Task 延遲啟動，並持有每個輸入 Task；
- 等待時由左至右啟動輸入，不等待較早暫停的輸入完成；
- 所有子任務都到達終態後才恢復父任務；
- 結果依參數順序組成 `std::tuple`，其中 `Task<void>` 對應 `std::monostate`；
- `std::vector<Task<T>>` 輸入依索引順序產生相同數量的結果 vector；
- 支援 move-only 結果；
- 全部子任務結束後，依參數順序重新拋出第一個子任務例外；
- 原子「計數 + 哨兵」協定保證立即完成和跨執行緒完成只恢復一次；
- 父任務在最後一個子任務完成的執行緒恢復，不隱式增加執行緒親和。

`TaskGroup` 遵循以下契約：

- `spawn(Task<void>)` 接管所有權並在傳回前 inline 啟動子任務；
- 並行接納會序列化；等待僅能使用一次的 `join()` 期間，仍在執行的子任務可以遞迴接納工作；
- 活動計數歸零時接納永久關閉；外部接納與最後一個完成競態時不保證哪方獲勝；
- join 恢復前，每個已接納子任務都必須到達終態；
- 全部子任務完成後，依接納順序重新拋出第一個例外；
- group 不可移動，解構時必須未使用或已經 join，否則終止程序；
- `get_stop_token()` 和 `request_stop()` 提供明確的標準取消通道，但不會向 Task 注入 token；
- `cancel_and_join()` 會在真正被等待時要求該通道，然後執行相同的 join；
- 最後一個子任務在其完成執行緒恢復 join，不隱式增加排程器親和。

`OneShotEvent` 遵循以下契約：

- 預設事件未 set，可由多個協程無分配地等待；
- 第一次 `set()` 永久設定事件，並恰好恢復所有已註冊等待者一次；
- 註冊與 set 競態時，等待者要麼進入恢復鏈結串列，要麼觀察到 set 狀態；
- set 前的寫入透過 acquire-release 順序發布給等待者；
- 等待者依未指定順序在 setter 執行緒 inline 恢復；
- 事件不可移動且必須比所有等待者活得更久；帶 pending 等待者解構會終止程序；
- 不提供 reset、可取消註銷、值或隱式 Scheduler 轉移。

`AsyncManualResetEvent` 遵循以下契約：

- 預設事件為 unset；`set()` 依 FIFO 恢復 pending 等待者，並讓後續等待保持 ready；
- `reset()` 只影響未來等待，已被 set 選中的等待者仍會完成；
- `wait(stop_token)` 以 O(1) 移除一個取消的 pending 等待者，預先取消的等待確定由取消獲勝；
- set 與取消競態恰有一個獲勝方，等待者只恢復一次；
- 等待者在 setter 或取消要求執行緒恢復，不隱式轉移 Scheduler；
- thread-local dispatch trampoline 保證巢狀訊號鏈不增長原生呼叫堆疊；
- 事件不可移動且必須比所有等待者活得更久；帶 pending 等待者解構會終止程序。

`AsyncMutex` 遵循以下契約：

- `lock_async()` 傳回無分配操作，等待結果是 move-only RAII Guard；
- 立即取得者 inline 繼續，競爭等待者依 FIFO 順序暫停；
- Guard 解構恰好交接一次所有權，並在該執行緒恢復下一個等待者；
- 所有權不綁定執行緒，內部狀態鎖不會在恢復使用者協程時持有；
- mutex 必須比所有 Guard 和等待者活得更久；持有或排隊時解構會終止；
- 不提供手工 unlock、try-lock、取消、遞迴或隱式 Scheduler 轉移。

`RunLoop` 和 `Scheduler` 遵循以下契約：

- RunLoop 既不能複製也不能移動；它的身分是所有關聯 Scheduler 的生命週期錨點；
- `run(Task<T>)` 消費一個根 Task，並在呼叫執行緒執行就緒 continuation；
- 回傳根任務結果，包括 move-only 結果；根任務例外會重新拋出；
- RunLoop 可以依序重複使用，但巢狀或並行呼叫 `run()` 會拋出 `std::logic_error`；
- `schedule()` 始終暫停，並把 continuation 追加到執行緒安全的 FIFO 就緒佇列；接受 token 的
  多載可以在消費前取消該排隊等待；
- `schedule_after()` 在暫停時測量原生 `steady_clock` 時長，`schedule_at()` 接受絕對的
  單調時鐘時間點；
- 所有接受 token 的多載仍會排隊，包括預先取消的等待；取消獲勝時拋出
  `OperationCancelled`，較晚的停止要求不能取代已經獲勝的完成；
- 已到期的期限仍非同步排隊；未來期限進入最小 Timer 堆，到期只讓 continuation 具備進入
  FIFO 排程的資格；
- 停止回呼只改變就緒項/計時器狀態並喚醒 RunLoop，不會 inline 恢復使用者協程；目前取消
  透過 O(n) 掃描定位佇列項；
- 其他執行緒可以入列，但只有正在執行 `run()` 的執行緒會消費佇列；
- RunLoop 銷毀後繼續使用其 Scheduler，或在其所屬 RunLoop 未運行時使用 Scheduler，皆會
  從 await 運算式拋出 `std::logic_error`；
- 根任務完成時若仍有單獨排隊的工作或待處理 Timer，RunLoop 會拒絕退出，因為本階段
  不支援 detached 所有權。

RunLoop 不擁有工作執行緒，也不提供自動執行緒親和。外部 awaiter 可以在其他執行緒恢復
Task；明確等待原 Scheduler 才會把 continuation 送回對應 RunLoop。如果 Task 暫停後沒有
安排其他執行緒或事件來源恢復它，`run()` 可能無限等待。阻塞函式仍會阻塞協程目前所在的
執行緒。

目前沒有公開自由函式 `sync_wait`、detached 執行、獨立 Timer 控制代碼、非同步 I/O 後端、
自訂協程框架 allocator 或阻塞工作執行緒池。取消仍是明確的：Scheduler 等待和
`AsyncManualResetEvent` 接受 token，TaskGroup 可持有共享 stop 通道，但模組不提供隱式
傳播，並且沒有保留舊骨架模組的相容別名。

捕捉變數的協程 lambda 需要特別小心：立即呼叫一個暫時的捕捉 lambda，可能使延遲協程參考
已經銷毀的閉包。CMP 尚未提供延長該閉包生命週期的輔助函式。

CMP 名稱中的 `C` 與 Go 執行期中的 `G` 相呼應，但這只說明命名來源，不表示兩者語意
等價。標準 C++ 協程提供暫停和恢復機制，本身不包含排程器，也不會把阻塞操作自動變成
非同步操作。

## 可選的後續工作

以下方向可以分別設計和審查，目前都不是套件的既有約定：

1. TaskGroup 結果控制代碼及更多支援取消的原語；
2. channel 和更多結構化喚醒路徑；
3. 多工作執行緒排程和工作竊取；
4. 非同步 I/O 整合；
5. 處理無法避免之阻塞工作的專用執行緒池；
6. 結果適配器和可選的協程框架配置策略。

Task、cancellation、RunLoop、`when_all`、TaskGroup、OneShotEvent、AsyncManualResetEvent 與
AsyncMutex 已形成真實的公開邊界，因此分別位於模組分割區中。只有其他已實作 API 確實需要
新邊界時，才繼續增加模組分割區或實作單元。

## 驗證

在儲存庫根目錄執行：

```text
mcpp build --profile dev --strict --cache=off
mcpp test --profile dev --strict --cache=off
mcpp build --profile release --strict --cache=off
mcpp test --profile release --strict --cache=off
cd examples/basic
mcpp run
```

預期結果是函式庫建置成功、七個二進位檔中的 90 項測試全部通過，而且範例依序輸出
`Coroutine result: 42`、`Concurrent result: 42`、`Task group result: 42`、
`Recursive group result: 3`、`Event signalled`、`Reusable event cycles: 2`、
`Mutex result: 42` 和 `Coroutine cancelled` 後以狀態 0 結束。測試保留原有高容量堆疊安全
檢查，並增加兩萬次 TaskGroup 遞迴接納、十萬次預取消就緒排程、五萬個 manual event 等待者、
兩萬次巢狀可複用事件訊號及 set/cancel 競態；第四階段重點競態套件已連續執行多輪 Release
測試。計算、臨時檔案和回環網路的成功/失敗計數及吞吐記錄在
[v1 可開發性壓測](benchmarks/2026-08-29-cmp-v1-readiness.md)。目前 Windows LLVM 工具鏈
不會產生 GNU depfile；如果模組介面包含的檔案發生變更，增量建置可能沿用舊的 BMI 或
目的檔。完整複驗時使用 `--cache=off`。
