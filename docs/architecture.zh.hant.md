# 架構

[English](architecture.md) · [简体中文](architecture.zh.md) · **繁體中文**

## 目前狀態

CMP 是一個具備小型協程執行核心的 C++23 模組專案。根模組匯出延遲啟動、單一消費者的
`mcpplibs::cmp::Task<T>`、結構化變參 `when_all()`、`RunLoop` 及其可複製的 `Scheduler`
控制代碼。`when_all()` 會啟動並持有多個 Task，直到全部結束。`RunLoop::run()` 是公開根
任務執行邊界，`Scheduler::schedule()` 用於明確地把暫停協程送回對應執行迴圈。
`schedule_after()` 和 `schedule_at()` 在沒有計時執行緒的情況下提供相對和絕對的
`steady_clock` 期限；接受 `std::stop_token` 的多載使這些等待可以協作式取消。

儲存庫現有內容包括：

- 一份 mcpp 套件清單；
- 根模組 `mcpplibs.cmp` 及 Task、RunLoop、`when_all` 模組分割區；
- 涵蓋契約、生命週期、例外、排程和執行緒行為的 gtest 測試；
- 一個透過路徑相依使用根套件的獨立範例；
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
│   ├── run_loop.cppm
│   └── when_all.cppm
├── tests/
│   ├── cmp_test.cpp
│   ├── run_loop_test.cpp
│   └── when_all_test.cpp
└── mcpp.toml
```

## 建置與測試

`.xlings.json` 固定專案使用的 mcpp 版本。`mcpp build` 建置自動推斷的函式庫目標。
`mcpp test` 會找到三個測試檔案，並為每個檔案連結 gtest 進入點。測試同時驗證 Task 所有權
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

範例程式使用與外部專案相同的匯入路徑：

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

這個範例在根測試目標之外，單獨檢查路徑相依解析、模組使用、外部協程編譯和公開根任務
驅動器。RunLoop 在主執行緒驅動 `print_answer()`；短單調時鐘計時器到期後，協程輸出
`Coroutine result: 42`。下一個根任務並行匯合兩個定時結果並輸出
`Concurrent result: 42`。最後一個協程從預先取消的定時等待捕捉 `OperationCancelled`，
並輸出 `Coroutine cancelled`。

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
- 支援 move-only 結果；
- 全部子任務結束後，依參數順序重新拋出第一個子任務例外；
- 原子「計數 + 哨兵」協定保證立即完成和跨執行緒完成只恢復一次；
- 父任務在最後一個子任務完成的執行緒恢復，不隱式增加執行緒親和。

`RunLoop` 和 `Scheduler` 遵循以下契約：

- RunLoop 既不能複製也不能移動；它的身分是所有關聯 Scheduler 的生命週期錨點；
- `run(Task<T>)` 消費一個根 Task，並在呼叫執行緒執行就緒 continuation；
- 回傳根任務結果，包括 move-only 結果；根任務例外會重新拋出；
- RunLoop 可以依序重複使用，但巢狀或並行呼叫 `run()` 會拋出 `std::logic_error`；
- `schedule()` 始終暫停，並把 continuation 追加到執行緒安全的 FIFO 就緒佇列；
- `schedule_after()` 在暫停時測量原生 `steady_clock` 時長，`schedule_at()` 接受絕對的
  單調時鐘時間點；
- 接受 token 的定時多載始終暫停；取消獲勝時拋出 `OperationCancelled`，較晚的停止要求
  不能取代已經進入就緒佇列的期限完成；
- 已到期的期限仍非同步排隊；未來期限進入最小 Timer 堆，到期只讓 continuation 具備進入
  FIFO 排程的資格；
- 停止回呼只改變計時器狀態並喚醒 RunLoop，不會 inline 恢復使用者協程；目前取消透過 O(n)
  掃描定位對應計時器；
- 其他執行緒可以入列，但只有正在執行 `run()` 的執行緒會消費佇列；
- RunLoop 銷毀後繼續使用其 Scheduler，或在其所屬 RunLoop 未運行時使用 Scheduler，皆會
  從 await 運算式拋出 `std::logic_error`；
- 根任務完成時若仍有單獨排隊的工作或待處理 Timer，RunLoop 會拒絕退出，因為本階段
  不支援 detached 所有權。

RunLoop 不擁有工作執行緒，也不提供自動執行緒親和。外部 awaiter 可以在其他執行緒恢復
Task；明確等待原 Scheduler 才會把 continuation 送回對應 RunLoop。如果 Task 暫停後沒有
安排其他執行緒或事件來源恢復它，`run()` 可能無限等待。阻塞函式仍會阻塞協程目前所在的
執行緒。

目前沒有公開自由函式 `sync_wait`、動態 TaskGroup、detached 執行、獨立 Timer 控制代碼、
非同步 I/O 後端、自訂協程框架 allocator 或阻塞工作執行緒池。取消只明確用於定時等待；
模組既不提供隱式取消傳播，也沒有普通 `schedule()` 的 token 多載，並且沒有保留舊骨架
模組的相容別名。

捕捉變數的協程 lambda 需要特別小心：立即呼叫一個暫時的捕捉 lambda，可能使延遲協程參考
已經銷毀的閉包。CMP 尚未提供延長該閉包生命週期的輔助函式。

CMP 名稱中的 `C` 與 Go 執行期中的 `G` 相呼應，但這只說明命名來源，不表示兩者語意
等價。標準 C++ 協程提供暫停和恢復機制，本身不包含排程器，也不會把阻塞操作自動變成
非同步操作。

## 可選的後續工作

以下方向可以分別設計和審查，目前都不是套件的既有約定：

1. 動態結構化任務作用域和取消傳播；
2. 更多結構化喚醒路徑；
3. 多工作執行緒排程和工作竊取；
4. 非同步 I/O 整合；
5. 處理無法避免之阻塞工作的專用執行緒池；
6. 結果適配器和可選的協程框架配置策略。

Task、RunLoop 與 `when_all` 已形成真實的公開邊界，因此分別位於模組分割區中。只有其他
已實作 API 確實需要新邊界時，才繼續增加模組分割區或實作單元。

## 驗證

在儲存庫根目錄執行：

```text
mcpp build --cache=off
mcpp test --cache=off
cd examples/basic
mcpp run
```

預期結果是函式庫建置成功、三個二進位檔中的 46 項測試全部通過，而且範例依序輸出
`Coroutine result: 42`、`Concurrent result: 42` 和 `Coroutine cancelled` 後以狀態 0
結束。測試分別執行一百萬次立即完成的 Task、十萬次立即雙 Task 匯合、十萬次明確排程、
十萬次立即 Timer 和十萬次預先取消的定時等待，用於檢查對稱轉移以及所有匯合或佇列路徑都
不會增長原生呼叫堆疊。目前 Windows LLVM 工具鏈不會產生 GNU depfile；如果模組介面包含的
檔案發生變更，增量建置可能沿用舊的 BMI 或目的檔。完整複驗時使用 `--cache=off`。
