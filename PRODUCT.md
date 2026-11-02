# Product

<!-- impeccable:product-schema 1 -->

## Platform

web

## Users

主要使用者：遊戲實況主與 VTuber——開播前設定音訊、直播中以 OBS 擷取成品音的使用者。

確認服務的其他受眾：

- Podcast／配音創作者（錄音、訪談、配音等非直播的內容製作）。
- 遠距會議與語音聊天使用者（一般開麥、想改善音質的人）。
- 音樂表演者與唱見（唱歌、樂器直播，需要低延遲監聽）。

## Product Purpose

RoudaMix 是 Windows 音訊混音器與 VST3 外掛宿主：把麥克風、遊戲與語音帶進同一個工作區，即時接收、處理並路由多個音訊來源，同時服務低延遲監聽與同步串流輸出。每一來源可分別決定「自己聽見什麼」與「觀眾聽見什麼」。

成功 = 兩者並重：開播前快速完成音訊設定並穩定撐完整場（穩定與可預期優先），且能透過 VST3 效果鏈打造自己的聲音（表達同樣是產品承諾）。

## Positioning

監聽／串流雙路由 + Monitor Bypass + 延遲政策：同一來源的 VST3 效果鏈，在自己耳機走低延遲 dry 訊號，在串流走完整處理並以 Full PDC 對齊——相鄰產品（系統混音器、OBS 音訊濾波器、通用 VST 宿主）無法同時誠實宣稱這三件事。

## Operating Context

- 典型直播工作流：RoudaMix → VB-CABLE（CABLE Input）→ OBS「音訊輸入擷取」（CABLE Output）→ 直播平台。
- 音訊環境：WASAPI 系統音訊／App 軌擷取；ASIO 驅動與第三方 VST3 由使用者自行安裝。
- 發行形態：Windows x64 可攜版（zip）與安裝版（setup.exe）；可攜版依賴 WebView2 Evergreen Runtime；設定保存在 `data` 資料夾。
- Session 工作流：設定 → 場景／Session 儲存載入，`Ctrl+S` 快速儲存；既有名稱、自訂色、來源、路由、插件與 Session 資料不得因 UI 變更被覆寫。
- 官方網站（`website/`）：獨立 Svelte 站點，繁中＋英文雙語的宣傳與六步互動教學，部署於 GitHub Pages；是產品的 Persuade 表面，不引用桌面 UI 或音訊引擎。
- 術語權威：`CONTEXT.md` 的領域語言表（Plugin Latency、Monitor Bypass、PDC 等，含應避用的錯誤說法）；撰寫 UI 文案與文件時以此為準。
- 開發與驗證慣例：本地引擎測試前必先全量重建 engine；提示一律走 `data-tooltip`（`ui/src/lib/tooltip.ts`），純圖示控制項另備 `aria-label`。

## Capabilities and Constraints

- 軌道類型：Audio／App／FX 軌；每軌可勾選 MON（監聽）與 STR（串流）、側鏈、低延遲。
- VST3 外掛鏈：僅支援使用者自行安裝的 Windows x64 VST3；有掃描、插件機架、Latency/PDC 診斷（詳見 `docs/plugin-latency-pdc-design.md`）。
- 延遲行為：監聽走 Low Latency、串流預設 Full PDC；Monitor Bypass／Runtime-suspended／Runtime-degraded 等狀態依 `CONTEXT.md` 定義。
- 平台約束：僅 Windows x64；目前發行檔無 Authenticode 簽章（Windows 會顯示未知發行者）。
- 授權約束：程式碼為 GPL-3.0-only；GPL 不授予 RoudaMix 名稱或標誌的商標權。
- 未定案事實：無（本次訪談未留下待決產品事實）。

## Brand Commitments

- 名稱：RoudaMix；圖示：`src-tauri/icons/icon.png`（官網亦使用其副本）。
- 標語：「你的聲音，由你分流。」；副標：「為直播、創作與每一次開麥」。
- 語言：繁體中文優先，英文（`README.en.md`、官網 `/en/`）並行。
- 語氣：務實、具體、不誇大——說明做什麼、怎麼做，不做無依據的行銷宣稱。

## Evidence on Hand

- `README.md`／`README.en.md`：產品定位、特色、下載與快速入門（六步）。
- 官方網站與互動教學：https://liutouo.github.io/RoudaMix/ （原始碼 `website/`）。
- `CONTEXT.md`：領域術語權威表。
- `docs/`：ADR、console/container prototype 記錄、plugin latency/PDC 設計文件。
- 圖示資產：`src-tauri/icons/icon.png`。

必須誠實面對的缺席：目前沒有使用者見證、案例研究、基準數據或媒體報導；未來任何表面都不得虛構這類證據。發行檔未簽署之事實須如實揭露，不得隱匿。

## Product Principles

1. **兩路各自合適**：自己聽乾淨低延遲、觀眾聽完整處理——路由真相是產品本身，任何 UI 都不得模糊它。
2. **開播時間神聖**：從安裝到穩定開播的路徑要短；使用者既有的設定與 Session 永遠安全。
3. **誠實呈現延遲**：延遲按路徑如實表達，絕不宣稱不存在的「零延遲」；術語遵循 `CONTEXT.md`。
4. **專業能力、平易入口**：VST3、PDC、ASIO 這類專業機制，對非音訊工程師也要可理解、可操作。
5. **不虛構、不誇大**：沒有的證據不編造，限制（如未簽署）如實揭露。

## Accessibility & Inclusion

- 明確驗收目標：WCAG 2.1 AA（使用者指定，2026-02 確認）。
- 既有底線（repo 既有政策，持續有效）：提示一律用 `data-tooltip`、禁用原生 `title`；純圖示控制項必須有 `aria-label`；鍵盤操作（快速鍵、Enter／Space／雙擊還原）持續可用。
- 領域特性：音量錶與延遲狀態不得只靠顏色傳達（搭配文字／數值）；錯誤訊息須可讀、可行動。
