# RoudaMix 宣傳網站

獨立的 Svelte 5 / Vite / TypeScript 網站；不引用桌面 UI、Tauri 或音訊引擎。使用專案現有 `src-tauri/icons/icon.png` 的副本作為品牌圖示。

## 本機開發與驗證

使用 Node.js 24，在 `website/` 執行：

```powershell
npm ci
npm run dev
```

依 Vite 顯示的連接埠開啟 `/RoudaMix/`；英文入口為 `/RoudaMix/en/`。

```powershell
npm run check
npm test
npx playwright install chromium
npm run test:browser
npm run build
npm run test:build
```

`test:browser` 會啟動 4387 埠的開發伺服器。`test:build` 另外以不提供 SPA fallback 的暫時 HTTP 伺服器，確認真正建置產物的雙語入口、重新整理、章節深連結及資源。截圖與失敗 trace 位於被 Git 忽略的 `test-results/`。

## 檔案分工

- `src/content.ts`：繁體中文與英文的七幕敘事、六步教學、FAQ。
- `src/story.ts`：純函式敘事狀態與動效參數。
- `src/workflowMotion.ts`：每個操作的連續進度、先後順序與結果門檻。
- `src/MotionPointer.svelte`：示意指標的移入、按下與點擊圓環，定位於實際示意控制項。
- `src/App.svelte`：進度、Lenis / ScrollTrigger、章節導覽、語言路徑、靜態閱讀模式。
- `src/Scene.svelte`：同一份音訊與同一個視窗的 DOM / SVG 示意。
- `src/ScenePlayback.svelte`、`src/heroMotion.ts`：進入畫面後的自動演示、首屏完整工作流預覽、暫停／重播與轉場。
- `src/audio.ts`、`src/AudioDemo.svelte`：共用時鐘的兩路音訊、播放生命週期與錯誤處理。
- `src/config.ts`：正式 Release 及音檔接入設定。
- `src/style.css`：品牌、排版、手機特寫、游標、減少動態模式。
- `src/workflow.css`：訊號粒子、電平、選單、操作指標與分流路徑的呈現。
- `index.html`、`en/index.html`：各自的語言、標題、description、Open Graph、canonical 與 hreflang；更新分享文案時須同步這兩份靜態入口。

## 分鏡與動效

捲動 progress 限制於 0–1，分為七個等長章節。預設進入畫面後會自動播放約 6.7 秒的操作演示，首屏直接預覽麥克風、外掛及兩路輸出的完整流程。章節按鈕以 0.65–1.25 秒平滑捲動到該幕 78% 位置，再從頭播放該幕。示意視窗有方向性移位／縮放轉場，文字依序進入；教學與下載區在進入畫面時浮現。

工具列提供播放／暫停、重播與「隨捲動」模式。手動滾輪／觸控可暫時接管示範進度，停止 450 ms 後接續播放；「隨捲動」模式則完全由捲動控制，適合逐幀查看及倒回。偏好存於本機 `roudamix-demo-mode`。示範結束後停在結果，波形與電平持續活動，直到暫停或離開畫面。八個視窗高度的捲動距離，加一個視窗高度的 sticky 場景；約一分鐘是閱讀目標。

| Hash | 場景 | 畫面結果 |
| --- | --- | --- |
| `#open` | 開啟 | 三路訊號依序匯入；同一視窗由 0.82 倍放大至 1 倍、旋轉回正 |
| `#input` | 設定 | 打開裝置清單、選擇 WASAPI，再加入麥克風並顯示波形 |
| `#plugin` | 外掛 | 清單展開，指標點選 Reverb，外掛落入效果鏈 |
| `#route` | 路由 | 先畫出耳機連線，再畫出 CABLE Input 連線，各自出現電平 |
| `#listen` | 分流 | 按下 M；青色路徑繞過 Reverb，紫色路徑繼續經過效果 |
| `#obs` | OBS | 畫出 VB-CABLE 連線、展開裝置選單、選 CABLE Output，再顯示電平 |
| `#voice` | Discord | 同一端點銜接 Discord，選擇 CABLE Output 後開始收音 |

桌面滾輪由 Lenis 平滑（duration 0.7 秒），章節按鈕則由 GSAP 驅動一次捲動，不疊加第二層延遲或強制吸附。選單、指標、外掛插入、連線繪製與 bypass 由同一個示範 phase 決定；訊號粒子與電平使用短循環。自動示範不產生聲音；離開場景或分頁進入背景即暫停。游標圓環表示整頁閱讀比例，下方細條表示該幕示範進度。手機使用原生觸控捲動、固定章節列及依可用高度縮放的操作區。尺寸切換時，尚未完成的導覽會優先到達原本指定的目的地。

詳見 [動畫分鏡與驗證](MOTION.md)。

`prefers-reduced-motion: reduce` 改為七個可順序閱讀的靜態區塊。圖形化操作不是可操作的混音器；真實控制項均提供文字名稱，未使用原生 `title` 提示，也未新增個別 tooltip。

若 Windows 或瀏覽器要求減少動態，頁首會顯示靜態模式說明與「開啟動畫」。訪客可自行開啟本站動畫，不需改動作業系統；也可從頁尾切回靜態閱讀。`?motion=full`／`?motion=reduced` 可直接指定網站模式，優先於儲存的 `roudamix-motion-preference`，最後才採用系統設定。JS 與 CSS 共用 `<html data-motion>`，避免已選擇開啟、動畫卻仍被 CSS 媒體查詢關閉。語言切換會保留明確選擇。

## 接入試聽音檔

目前 `publication.audio = null`，公開頁不顯示播放按鈕、不請求音訊。測試動態產生短 PCM 訊號，只在測試 HTTP 攔截中使用，絕不打包或當作正式試聽。

取得兩份已完成、可公開使用的音檔後：

1. 確認兩檔從同一時間原點輸出，長度、前置靜音與取樣率一致，內容與文案的監聽略過混響／串流保留混響相符。建議短 WAV；解碼後會常駐記憶體，避免長檔。
2. 放到 `public/audio/monitor.wav` 與 `public/audio/stream.wav`。
3. 在 `src/config.ts` 設定：

```ts
audio: {
  monitor: '/RoudaMix/audio/monitor.wav',
  stream: '/RoudaMix/audio/stream.wav',
},
```

4. 更新「未就緒設定」測試，改驗證兩個實際公開資源及內容授權；保留沒有素材時的渲染情境測試。
5. 在耳機實聽驗收：首次必須手動播放、暫停續播保留位置、快速來回切換不跳段、60 ms 交叉淡化無明顯突音、播放結束可重播、載入失敗仍可閱讀。

兩檔由同一 AudioContext 在同一時間與 offset 啟動，只切換 GainNode；不以兩個 HTMLAudioElement 的獨立時鐘同步。兩檔時長差超過 20 ms 會拒絕播放，但無法自動判定內容是否真正對齊。離開試聽幕、離開可見區域、頁面進入背景或切換語言會暫停／釋放；捲動不倒放音訊。瀏覽器測試證實排程與狀態，不取代正式素材的聽感驗收。

## 接入正式 Release

目前 `publication.release = null`，顯示「Windows 版即將提供」。不使用現有測試 ZIP。

正式公開 Release 完成後，填入真實版本與完整 HTTPS 下載 URL：

```ts
release: {
  version: 'X.Y.Z',
  url: 'https://github.com/LiuTouo/RoudaMix/releases/download/vX.Y.Z/RoudaMix-X.Y.Z-windows-x64-portable.zip',
},
```

以上是接入格式，不能原樣當作下載網址。同步更新 `content.ts` 的準備中文案、需求與發行資訊，再以未登入瀏覽器確認附件可下載。若專案名稱或部署帳號更動，同步調整 Vite base、兩份 HTML metadata 與公開資源路徑。

## GitHub Pages

`.github/workflows/website.yml` 在網站 PR / main 推送時驗證；只上傳 `website/dist`。PR 與私人儲存庫不部署；公開儲存庫的 main 才進入 Pages deployment。workflow 不建立 Release、不打包桌面程式，也不變更儲存庫可見性。

正式啟用時，將主儲存庫公開，並在 GitHub Settings → Pages 將 Source 設為 GitHub Actions。再於 main 執行 Website workflow。目標路徑：`https://liutouo.github.io/RoudaMix/` 與 `/RoudaMix/en/`。實際部署尚須這些外部條件成立，建立 workflow 不代表已發布。

## 查證來源

操作名稱以 `ui/src/App.svelte`、`ui/src/lib/TrackStrip.svelte` 及 `packaging/release/README.txt` 為準。英文教學標出必要的實際中文選項，未假設桌面程式有英文介面。

- [Lenis 官方整合說明](https://github.com/darkroomengineering/lenis#gsap-scrolltrigger)（透過 Context7 查詢）
- [GSAP ScrollTrigger](https://gsap.com/docs/v3/Plugins/ScrollTrigger/)
- [Svelte 元件生命週期與 mount](https://svelte.dev/docs/svelte/svelte)
- [Vite 6 靜態部署](https://v6.vite.dev/guide/static-deploy)
- [VB-CABLE 官方說明](https://vb-audio.com/Cable/)
- [OBS Audio Sources](https://obsproject.com/kb/audio-sources)
- [連續物件敘事方向參考](https://unitedcarriers.com/)；未複製其網頁素材或程式碼，也未聲稱逐幀還原。
