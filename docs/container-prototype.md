# Inputs / Outputs 容器原型（throwaway / DEV only）

設計問題：在保留 RoudaMix 的識別色帶、來源／路由、M/B、左側直立電平與推桿、右側 VST 資訊密度下，哪種 **Inputs / Outputs 大容器** 最容易理解？重點是暗色內凹底盤與容器標題，不是仿製金屬推桿。

## 決策：採用 A（2026-10-26）

使用者明確選定：「我要用A」。A 的左右獨立暗色內凹底盤最貼近原始參考需求：Inputs / Outputs 各自收納直立音軌，重點是容器，不是金屬推桿。

正式實作僅調整 `App.svelte` 現有 group 的標題／真實軌數與 `console.css` 的底盤、間距及捲動樣式；沿用真實 TrackStrip、資料、控制、效果器與路由，不匯入此原型或 mock。音軌寬 230px（原 250px，2026-10-26 緊緻化）、間距 16px、lane 原點與既有虛擬化／拖放事件保持不變；M/B 改為第三列流排版靠右，下拉列整行不再重疊。多軌在各底盤內捲動，極窄窗保留 board 水平捲動以到達兩個容器。

原型仍是 DEV-only 比較來源，未直接升為正式元件。未獲 commit／branch 操作授權，因此沒有建立 throwaway branch、commit、push 或刪除未保存的原型。

## 啟動與比較

從 repo root 執行（一行，使用現有 UI dependencies）：

```sh
npm --prefix ui run prototype:containers
```

- A 左右雙容器：http://127.0.0.1:5191/?prototype=containers&variant=A
- B 共用機架分區：http://127.0.0.1:5191/?prototype=containers&variant=B
- C 上下分區：http://127.0.0.1:5191/?prototype=containers&variant=C

底部浮動列可直接選 A/B/C 或用左右箭頭循環；非表單／按鈕／可捲動區域焦點下，鍵盤 ← / → 也可切換。URL 可分享與重新整理；缺少或無效的 variant 會歸一成 A。窄螢幕保留直立音軌，在各容器內橫向捲動；極窄螢幕 A/B 也會上下排列，主要視覺比較請用桌面寬度。

## 為何採最小隔離入口

正式 `App.svelte` 的掛載會註冊桌面事件與載入設定，`TrackStrip.svelte` 直接呼叫 engine mutation。為避免瀏覽器缺少 Tauri backend，或試用時操作真實音訊，本原型沿用 `main.ts` 既有 DEV-only `?prototype=` 分流，不掛載正式 App/TrackStrip。`?prototype=containers` 只載入 `ContainerPrototype.svelte`；既有正式入口、console / palette 原型與正式混音行為保持不變。

復用現有全域色票、字體、track icons、tooltip 層；所有容器樣式只作用於此原型。production build 不保留此入口及其元件。沒有新 dependency、backend alias、儲存或音訊呼叫。

## 可試用與邊界

- 四條 Inputs（人聲、音樂、桌面、FX）與兩條 Outputs（監聽、串流），三方案使用相同資料。
- 增益、M、B、監聽／串流傳送按鈕僅變更記憶體狀態；切換方案保留，重新整理或「重設模擬狀態」復原。
- 電平與 telemetry 是標示清楚的 mock 示意；不模擬音訊處理、路由加總或插件行為。
- 展開「檢視完整模擬狀態」可讀取當前方案與全部 mock track 資料。
- 不提供存檔、裝置切換、插件 GUI、真實音訊與檔案寫入。
- 已選定 A 的容器視覺，原型本身仍不提升為正式介面；沒有 commit / push。

## 驗證

```sh
npm --prefix ui run check
npm --prefix ui run build
npm --prefix ui test
```

新增 `ui/src/lib/vitest/containerPrototype.test.ts`：A/B/C 深連結與容器內容、無效 URL、循環切換、焦點保護、history 更新、本地增益／mute／bypass／路由、重設。

實跑既有本機 Chromium（headless）：1440×1000 與 390×844 的 A/B/C；確認兩組容器的方向、文件無橫向溢出、窄屏容器內捲動、range 鍵盤操作、跨方案狀態保留、刷新重設，沒有 console/page errors 或 Tauri invoke。截圖在本機 temp 的 `roudamix-containers-{A,B,C}.png` 及 `roudamix-containers-{A,B,C}-mobile.png`，未加入 repo。

上述截圖及 mock 互動檢查是原型驗證，不是正式 backend 整合驗證。使用者已選定 A；真實桌面 WebView、其他瀏覽器與實體音訊仍需另行驗證。

### 正式套用 A 的驗證

- `svelte-check`：0 errors / 0 warnings；production build 通過。
- UI tests：90 Node + 185 Vitest 通過，包含 tooltip-policy；新增 `console-containers.test.ts` 的兩個結構／幾何契約測試。
- 未 mock 的正式根頁可渲染兩個空底盤；一般瀏覽器沒有 Tauri backend，出現 `transformCallback` undefined，因此未完成真實 backend／音訊整合驗證。
- 額外使用既有隔離 console transport 掛載真實 App／TrackStrip，只驗證版面：27 inputs／14 outputs、230px strip／246px pitch、各 lane 捲至末軌、虛擬化、空輸入、800px／390px 窄窗，均通過且無 JS page errors。這是 **mock transport 下的正式元件 layout smoke**，不能替代上項整合驗證。
- 本機 temp 截圖以 `roudamix-production-A-` 開頭；`no-backend.png` 是未 mock 根頁，其餘 `layout-only.png`、`many-layout-only.png`、`800-layout-only.png`、`390-layout-only.png`、`empty-layout-only.png` 均為隔離 transport 下的版面證據。
