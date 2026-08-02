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
> CMP 目前處於 **bootstrap 階段**。套件已經匯出根模組 `mcpplibs.cmp`，但尚未提供協程執行期 API。

CMP 計畫以標準無堆疊 C++ 協程建構現代協程執行期與函式庫。專案將以明確的 `co_await`
為主線，透過經過驗證的小步驟逐步探索排程、計時器、非同步 I/O、取消，以及阻塞工作的安全隔離。

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
- M:N 排程、work stealing、計時器、取消或非同步 I/O 已經實作。

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

範例會以成功狀態結束且不產生輸出。它只負責證明獨立 mcpp 套件能夠解析路徑相依並匯入
`mcpplibs.cmp`。

## 目前模組

```cpp
import mcpplibs.cmp;

int main() {
    return 0;
}
```

bootstrap 階段的模組刻意不包含公開宣告。未來公共 API 將使用 `mcpplibs::cmp` 命名空間。

## 儲存庫結構

```text
.
├── .xlings.json                   # 固定的專案工具環境
├── mcpp.toml                      # 套件識別與測試相依
├── src/cmp.cppm                   # 根模組介面
├── tests/cmp_test.cpp             # 匯入 smoke 測試
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
`[dependencies]`，測試專用相依放在 `[dev-dependencies]`。

## 路線圖

執行期工作將拆分為可以獨立審查的階段：

1. 套件識別與可匯入模組 bootstrap；
2. 協程 task 與生命週期語意；
3. 最小單執行緒排程器；
4. 計時器、取消和結構化喚醒路徑；
5. 多 worker 排程與 work stealing；
6. 非同步 I/O 整合和 blocking pool。

bootstrap 之後的順序只是方向，不代表這些能力已經實作。

## 參與貢獻

修改模組邊界前請先閱讀[架構說明](docs/architecture.zh.hant.md)。保持改動小而清楚，遵循
C++23 模組慣例，並以 `mcpp build`、`mcpp test`、獨立範例和 CI 結果作為實作事實。

## 授權條款

[Apache License 2.0](LICENSE)
