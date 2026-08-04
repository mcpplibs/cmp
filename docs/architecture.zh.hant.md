# 架構

[English](architecture.md) · [简体中文](architecture.zh.md) · **繁體中文**

## 目前狀態

CMP 是一個 C++23 模組專案，已實作第一個執行期基礎型別。根模組匯出延遲啟動、單一消費者的
`mcpplibs::cmp::Task<T>` 及其 `Task<void>` 特化，但尚未提供排程器或根任務執行 API。

儲存庫現有內容包括：

- 一份 mcpp 套件清單；
- 根模組 `mcpplibs.cmp` 及其 Task 實作；
- 涵蓋契約、生命週期、例外和對稱轉移的 gtest 測試；
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

`gtest = "1.15.2"` 是測試使用的開發相依。CMP 目前不追蹤 `mcpp.lock`，該檔案由
`.gitignore` 排除。

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
├── src/cmp.cppm
├── tests/cmp_test.cpp
└── mcpp.toml
```

## 建置與測試

`.xlings.json` 固定專案使用的 mcpp 版本。`mcpp build` 建置自動推斷的函式庫目標。
`mcpp test` 會找到 `tests/cmp_test.cpp`，連結 gtest 進入點，並驗證 Task 型別契約、延遲執行、
協程框架所有權、值與例外傳播、巢狀組合，以及不會增長呼叫堆疊的對稱轉移。

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

Task<int> answer() {
    co_return 42;
}

Task<void> print_answer() {
    auto value = co_await answer();
    std::println("Coroutine result: {}", value);
    co_return;
}
```

這個範例在根測試目標之外，單獨檢查路徑相依解析、模組使用和外部協程編譯。範例私有的
`InlineRunner` 啟動一個 eager 根協程，等待 `print_answer()` 並輸出 `Coroutine result: 42`。
這個輔助型別只適用於目前同步完成且沒有排程器的協程鏈，不屬於 CMP 公共 API。

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

被移動後的 Task 為空，不得再次等待；目前實作會在違反該契約時終止程序。目前沒有公開
`sync_wait`、detached 執行、排程器、執行緒親和保證、計時器、取消機制、非同步 I/O 後端、
自訂協程框架 allocator 或阻塞工作執行緒池，也沒有保留舊骨架模組的相容別名。

捕捉變數的協程 lambda 需要特別小心：立即呼叫一個暫時的捕捉 lambda，可能使延遲協程參考
已經銷毀的閉包。CMP 尚未提供延長該閉包生命週期的輔助函式。

CMP 名稱中的 `C` 與 Go 執行期中的 `G` 相呼應，但這只說明命名來源，不表示兩者語意
等價。標準 C++ 協程提供暫停和恢復機制，本身不包含排程器，也不會把阻塞操作自動變成
非同步操作。

## 可選的後續工作

以下方向可以分別設計和審查，目前都不是套件的既有約定：

1. 根任務驅動器、最小單執行緒排程器和明確的排程 awaiter；
2. 結構化任務作用域和並行匯合；
3. 計時器、喚醒路徑和取消；
4. 多工作執行緒排程和工作竊取；
5. 非同步 I/O 整合；
6. 處理無法避免之阻塞工作的專用執行緒池；
7. 結果適配器和可選的協程框架配置策略。

只有已實作的 API 確實需要新邊界時，才增加模組分割區或實作單元。

## 驗證

在儲存庫根目錄執行：

```text
mcpp build --cache=off
mcpp test --cache=off
cd examples/basic
mcpp run
```

預期結果是函式庫建置成功、八個 Task 測試通過，而且範例以狀態 0 結束。其中一個測試執行
一百萬次立即完成的 Task，用於檢查對稱轉移不會增長原生呼叫堆疊。目前 Windows LLVM
工具鏈不會產生 GNU depfile；如果模組介面包含的檔案發生變更，增量建置可能沿用舊的 BMI
或目的檔。完整複驗時使用 `--cache=off`。
