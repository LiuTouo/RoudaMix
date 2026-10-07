# 幾何扁平 UI 原型

此分支僅用於外觀決策，尚未選定正式樣式。沿用 `prototype/vst-editor-styles` 的預覽入口、比較列與 Tauri 模擬層，以目前主程式的元件呈現。

從專案根目錄執行 `npm --prefix ui run prototype`，開啟 http://127.0.0.1:5188/?variant=A 。網址可用 `original`、`A`、`B`、`C`；底部比較列可用按鈕或取得焦點後的左右方向鍵切換，支援收合。

- A：零圓角色塊；B：2px 圓角及細框；C：連續底色與水平分隔。配置與尺寸不變。
- 背景 `#111418` → 面板 `#1d232b` → 功能區 `#28313c` → 控制／浮層 `#354151` → 浮層內控制 `#424f60`。
- 操作皆為記憶體模擬，重新整理重設。儲存、下載、註冊快捷鍵及 VST 原生編輯器不操作真實系統。
- 使用主程式入口操作設定、插件、路由、側鏈及延遲面板；比較列另提供警示與關閉情境。改動軌道後模擬關閉，選「關閉程式」可看未儲存提示。
- 全域 tooltip 仍由 `app.css` 維護，只有原型設定樣式變數；正常 `dev` / `build` 使用原入口與既有外觀。

驗收：`npm --prefix ui run check`、`npm --prefix ui test`、`npm --prefix ui run build`。預覽伺服器啟動後，以已安裝的 Playwright 執行 `node ui/src/prototype/check.mjs`；非專案依賴可用 `PLAYWRIGHT_MODULE` 指向其模組 URL，`CHROMIUM_PATH` 指定既有 Chromium 執行檔。無須新增正式依賴。

瀏覽器驗收會輸出 `.scratch/geometric-flat-ui/gallery.html`、各畫面对照截圖與 `report.json`，涵蓋三種尺寸、各主要浮層、狀態保留、文字對比、提示及正式產物隔離。等使用者選定後，再於正式分支實作選定方向；本分支保留作為原型依據。
