# RoudaMix · C 極簡操作（已採用）

## 目前正式介面

使用者已選定 C，正式 `App` 預設使用此設計，不需要 prototype 參數或外觀開關：

- 軌道名稱只顯示於**底部整條識別色帶**，保留改名、換色與增益百分比。
- 上方三列保留來源／裝置、送出、MON/STR／側鏈／低延遲；M/B 與最後一列並排。
- 軌道刪除按鈕／系統輸出角色留在來源列右側；刪除仍須確認。
- 移除重複的頂部軌名、技術 ID、裝飾性區域標示、STEREO 與 FX source 長句。
- 保留**左側推桿／音頻錶、右側全高 VST 機架**；插件清單獨立捲動。
- 現有名稱、自訂色、來源、路由、插件與 Session 資料不因換 UI 被覆寫。

正式開發模式：

```powershell
npm run tauri -- dev
```

## 實作位置與邊界

| 路徑 | 用途 |
|---|---|
| `ui/src/console.css` | 正式 C 布局與 tokens，唯一的外觀來源；保留 250px strip / 8px lane pitch。 |
| `ui/src/App.svelte` | 正式入口布局、原始狀態列與新增軌道控制；移除裝飾標題及分區 prop。 |
| `ui/src/lib/TrackStrip.svelte` | 原控制項與 handlers；短標籤直接為 Svelte 文字，不使用 CSS 偽元素替代。 |
| `ui/src/lib/MeterCanvas.svelte` | 保持此前確認的 meter 行為與色彩，本輪未修改。 |
| `ui/src/ConsolePrototype.svelte` | DEV 模擬資料工具，直接掛同一個 `<App />`。 |
| `ui/src/console-prototype.css` | 僅 DEV 包裝與工具列，沒有正式介面的外觀覆寫。 |

已移除 A/B/C 專用覆寫樣式及切換器；選定的設計不依賴預覽程式。來源、路由、gain、mute、bypass、側鏈、低延遲、插件及排序的音訊 handlers 不變。

機架高度在正式版維持 localStorage 儲存／重載還原；隔離預覽不保存高度。Enter／Space／雙擊可恢復全高。所有提示維持 `data-tooltip`，圖示控制項保留 `aria-label`。

## C 的修改前基準與正式 build 比對

實作前已保存 `.scratch/header-C-approved/`：

- C 的 1600×1000、1280×800、800×800、390×800 四種畫面。
- Routing、plugin picker、settings 三種浮層。
- 元件座標、尺寸、字型、邊框與顏色的 `reference.json`。
- 修改前的 App、TrackStrip、正式／原型樣式、預覽 wrapper 與切換器來源。

基準只隱藏 DEV 工具列、讓 App 使用完整 viewport，沒有修改 App 的元件樣式；滑鼠停在非控制項處，避免 tooltip 計時造成不同截圖。

`.scratch/header-C-production-compare.mjs` **直接載入 `ui/dist` 的正式 JS／CSS**，使用 Tauri 官方 `mockIPC` / `mockWindows` 在測試邊界供應相同 fixture，不改寫正式 bundle。

| 檢查 | 結果 |
|---|---|
| 四種尺寸的元件幾何／量測樣式 | 全部一致。 |
| 1600 / 800 / 390px 主畫面、routing、plugin picker | 最新比對為零像素差異。 |
| 1280px 與 settings | 最大單一色彩通道差異 1/255，屬反鋸齒／透明合成容差。 |
| 七張截圖 | 均無超過 1/255 容差的像素；不遮蔽任何 App 區域。 |

正式截圖及報告：`.scratch/header-C-production/1600.png`、`.scratch/header-C-production/report.json`。

## 操作與建置驗證

- 正式 bundle：來源／輸出裝置、App 解除／重綁、路由、側鏈、低延遲、M/B、Monitor Bypass、gain、改名、換色、插件搜尋／加入／確認移除、軌道拖曳均通過。
- 正式 bundle：機架高度拖曳、儲存、reload 還原及鍵盤重設均通過。
- 正式 bundle：1600×1000 下機架可見 **26 個插件列**；30 個插件可完整捲動與鍵盤存取。
- 1280 / 800 / 390px 無 document 水平溢出；畫面外推桿可透過鍵盤焦點存取。
- 共用 UI smoke 另驗證插件拖放／context menu、刪除確認、設定各頁與 reduced motion。
- 77 Node + 89 Vitest tests 通過；`svelte-check` 0 errors / 0 warnings；production build 與 DEV mock 邊界檢查通過。
- GitNexus 的 Svelte／跨語言呼叫解析仍有限；先做 impact、再補查實際引用。不能將零受影響 process 當作沒有影響，驗證以來源與可執行檢查為準。

**限制：**以上是同資料下的正式前端視覺／操作測試，不是實體 ASIO/WASAPI 裝置或原生 VST editor 的硬體驗證；本輪未修改音訊引擎及 IPC 協定。

## 保留的隔離資料預覽

```sh
npm --prefix ui run prototype:console
```

<http://127.0.0.1:5190/?prototype=console&variant=C>

與正式介面共用元件／樣式；只有模擬資料及 DEV 工具列不同。僅 `serve + console-prototype` 專用模式替換 native APIs，production build 不包含 mock、fixtures 或預覽工具列。普通 dev 的 prototype 入口仍 fail-closed。舊 variant URL 統一轉到 C。

## 歷史方案

`.scratch/header-study/` 保留 A/B/C 比較圖及當時的驗證紀錄；三案切換腳本只適用於歷史探索階段，不再對目前正式介面執行。更早的 native 整合基準在 `.scratch/console-approved-reference/`，`console-original*.png` 則是已淘汰的整套原版實驗。
