# 架構

[English](architecture.md) · [简体中文](architecture.zh.md) · **繁體中文**

## 目前狀態

CMP 目前是一個初始的 C++23 模組專案。套件可以建置和匯入，但根模組還沒有公開宣告，
協程執行期也尚未實作。

儲存庫現有內容包括：

- 一份 mcpp 套件清單；
- 只包含模組宣告的根模組 `mcpplibs.cmp`；
- 一個 gtest 匯入測試；
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
`cmp`，在 C++ 原始碼中匯入 `mcpplibs.cmp`。`mcpplibs::cmp` 保留給日後的公開 C++
宣告使用。

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
`mcpp test` 會找到 `tests/cmp_test.cpp`，連結 gtest 進入點，並驗證另一個轉譯單元可以
匯入 `mcpplibs.cmp`。

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
import mcpplibs.cmp;

int main() {
    return 0;
}
```

這個範例在根測試目標之外，單獨檢查路徑相依解析和模組使用。

## 目前邊界

根模組目前沒有提供 `task`、promise 型別、排程器、計時器、取消機制、非同步 I/O 後端或
阻塞工作執行緒池，也沒有保留舊骨架模組的相容別名。

CMP 名稱中的 `C` 與 Go 執行期中的 `G` 相呼應，但這只說明命名來源，不表示兩者語意
等價。標準 C++ 協程提供暫停和恢復機制，本身不包含排程器，也不會把阻塞操作自動變成
非同步操作。

## 可選的後續工作

以下方向可以分別設計和審查，目前都不是套件的既有約定：

1. 協程工作的所有權、完成和生命週期規則；
2. 單執行緒排程器和明確的排程 awaiter；
3. 計時器、喚醒路徑和取消；
4. 多工作執行緒排程和工作竊取；
5. 非同步 I/O 整合；
6. 處理無法避免之阻塞工作的專用執行緒池。

只有已實作的 API 確實需要新邊界時，才增加模組分割區或實作單元。

## 驗證

在儲存庫根目錄執行：

```text
mcpp build --cache=off
mcpp test --cache=off
cd examples/basic
mcpp run
```

預期結果是函式庫建置成功、一個匯入測試通過，而且範例以狀態 0 結束。目前 Windows
LLVM 工具鏈不會產生 GNU depfile；如果模組介面包含的檔案發生變更，增量建置可能沿用舊的
BMI 或目的檔。完整複驗時使用 `--cache=off`。
