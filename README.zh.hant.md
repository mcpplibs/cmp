# CMP

> **Coroutine Machine Process**——一個基於 C++23 的協程執行期專案。

[![C++23](https://img.shields.io/badge/C%2B%2B-23-blue.svg)](https://en.cppreference.com/w/cpp/23)
[![Module](https://img.shields.io/badge/module-mcpplibs.cmp-green.svg)](https://en.cppreference.com/w/cpp/language/modules)
[![License](https://img.shields.io/badge/license-Apache_2.0-blue.svg)](LICENSE)

[English](README.md) · [简体中文](README.zh.md) · **繁體中文**

[mcpp](https://github.com/mcpp-community/mcpp) · [架構](docs/architecture.zh.hant.md) ·
[Issues](https://github.com/mcpplibs/cmp/issues)

[![ci-linux](https://github.com/mcpplibs/cmp/actions/workflows/ci-linux.yml/badge.svg?branch=main)](https://github.com/mcpplibs/cmp/actions/workflows/ci-linux.yml)
[![ci-macos](https://github.com/mcpplibs/cmp/actions/workflows/ci-macos.yml/badge.svg?branch=main)](https://github.com/mcpplibs/cmp/actions/workflows/ci-macos.yml)
[![ci-windows](https://github.com/mcpplibs/cmp/actions/workflows/ci-windows.yml/badge.svg?branch=main)](https://github.com/mcpplibs/cmp/actions/workflows/ci-windows.yml)

> [!IMPORTANT]
> CMP 已提供延遲啟動、單一消費者的 `Task<T>` / `Task<void>`，以及在呼叫執行緒運行、
> 支援明確排程和單調時鐘定時排程的 `RunLoop`。取消、非同步 I/O 和 detached 執行尚未實作。

CMP 計畫以標準無堆疊 C++ 協程建構現代協程執行期與函式庫。明確的 `co_await` 模型現已
涵蓋排程和單調時鐘計時器，並將透過經過驗證的小步驟繼續探索非同步 I/O、取消，以及阻塞
工作的安全隔離。

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
- M:N 排程、work stealing、取消或非同步 I/O 已經實作。

這些能力必須分別設計和驗證。預期方向是明確的非同步 I/O awaiter、專用 blocking pool
以及協作式安全點。

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

範例會先等待一個短 RunLoop 計時器，再從 `Task<void>` 協程內部印出
`Coroutine result: 42`，然後以成功狀態結束。它負責證明獨立 mcpp 套件能夠解析路徑相依、
匯入 `mcpplibs.cmp`、組合 Task 並透過公開 RunLoop 使用定時排程。

## 目前 API

```cpp
import std;
import mcpplibs.cmp;

using mcpplibs::cmp::Task;
using mcpplibs::cmp::RunLoop;

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

int main() {
    RunLoop loop {};
    loop.run(print_answer(loop.get_scheduler()));
}
```

`Task` 採延遲啟動：呼叫 `answer()` 只建立處於暫停狀態的協程，在被 `co_await` 消費時才
開始執行。Task 只能移動、只有一個消費者且只能作為右值等待；它保存值或例外，在子協程與
continuation 之間直接轉移，並透過 RAII 銷毀未消費的協程框架。目前刻意不支援 `Task<T&>`、
複製、移動賦值和 detached 執行。

定義協程的轉譯單元必須匯入 `std`，使編譯器能夠看到標準協程協定型別。CMP 私下匯入
`std`，不會向使用端重新匯出整個標準函式庫。

`RunLoop::run()` 消費一個根 Task，在呼叫執行緒執行就緒協程，回傳結果並重新拋出例外。
`Scheduler::schedule()` 始終暫停目前協程並把 continuation 放入佇列；`schedule_after()`
等待相對的 `steady_clock` 時長，`schedule_at()` 等待絕對的單調時鐘時間點。期限到達只讓
任務具備執行資格，不會 inline 恢復。Scheduler 可以複製，但始終屬於建立它的 RunLoop。
支援依序多次呼叫 `run()`，巢狀或並行呼叫會被拒絕。已經被移動的 Task 不得再次等待。

RunLoop 不是背景執行緒，也不會把阻塞程式碼自動變成非同步程式碼。如果 Task 暫停後沒有
安排未來的恢復動作，`run()` 可能一直等待。CMP 不提供隱式執行緒親和：外部 awaiter 在其他
執行緒恢復協程後，需要明確等待目標 Scheduler 才會返回對應 RunLoop。

## 儲存庫結構

```text
.
├── .xlings.json                   # 固定的專案工具環境
├── mcpp.toml                      # 套件識別與測試相依
├── src/cmp.cppm                   # 根模組介面
├── src/task.cppm                  # Task 模組分割區
├── src/run_loop.cppm              # RunLoop 與 Scheduler 分割區
├── tests/cmp_test.cpp             # Task 契約和生命週期測試
├── tests/run_loop_test.cpp        # 排程、邊界和執行緒測試
├── examples/basic/                # 獨立的路徑相依 consumer
├── docs/architecture.zh.hant.md   # 目前結構、邊界與演進方向
└── .github/workflows/             # Linux、macOS 和 Windows CI
```

儲存庫目前不發布 `mcpp new` 範本。等 CMP 形成值得展示的穩定執行期 API 後，再設計專用範本。

## 開發與驗證

本機驗證路徑：

```bash
mcpp build --cache=off
mcpp test --cache=off
cd examples/basic && mcpp run
```

CI 在 Linux、macOS 和 Windows 上執行等價的建構、測試與獨立範例流程。mcpp 版本由
`.xlings.json` 固定，不應依賴無關的全域 mcpp 安裝。

CMP 目前不追蹤 `mcpp.lock`，`.gitignore` 明確執行這項儲存庫約定。執行期相依放在
`[dependencies]`，gtest 明確宣告在 `[dev-dependencies.compat]` 中。

## 路線圖

執行期工作拆分為可以獨立審查的階段：

1. 套件識別與可匯入模組 bootstrap——已完成；
2. 協程 task 與生命週期語意——已實作初始 `Task`；
3. 根任務驅動器和最小單執行緒排程器——已完成初始實作；
4. 單調時鐘 Timer v1——已實作；取消和結構化喚醒路徑仍待開發；
5. 多 worker 排程與 work stealing；
6. 非同步 I/O 整合和 blocking pool。

剩餘順序只是方向，不代表列出的能力已經實作。

## 參與貢獻

修改模組邊界前請先閱讀[架構說明](docs/architecture.zh.hant.md)。保持改動小而清楚，遵循
C++23 模組慣例，並以 `mcpp build`、`mcpp test`、獨立範例和 CI 結果作為實作事實。

## 授權條款

[Apache License 2.0](LICENSE)
