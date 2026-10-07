# ADR 0007 — 動畫一律播放，不連動 Windows「動畫效果」設定

日期：2026-02
狀態：已採用

## 背景與問題

Windows 11 的「設定 → 協助工具 → 視覺效果 → 動畫效果」關閉時，WebView2 只是把 CSS
媒體查詢 `prefers-reduced-motion` 回報為 `reduce`；CSS animation/transition 與
rAF 驅動的動畫本身**不會**被作業系統停用。此前 `console.css` 以該媒體查詢全域關閉
transitions/animations（僅豁免落點指示），導致 RoudaMix 的動畫行為隨使用者系統設定
改變：拖曳幽靈抬起有動畫、其他回饋卻瞬間跳變，視覺語言不一致。

## 決策

1. RoudaMix 是即時操作工具，動畫屬於**狀態語言**而非裝飾：落點指示、拖曳拿起／落定、
   啟用態切換都承載功能語意。一律照常播放，不連動 OS 動畫設定（使用者指定）。
2. 不提供 app 內「減少動畫」開關——避免第二套行為模式；動畫本身維持專業克制
   （90–180ms、位移 4–12px、以淡入淡出為主），無眩光、無大位移，長時間使用不干擾。
3. 全 UI 原始碼**禁止出現 `prefers-reduced-motion`**，由
   `ui/src/lib/motion-policy.test.ts` 靜態把關，防止未來回歸。

## 動畫系統

- Token 集中在 `ui/src/motion.css`：4 個時長（`--motion-dur-1/2/3/out` =
  90/140/180/120ms）＋ 3 條曲線（`--motion-ease-out/in/standard`）。
- 鐵律：只動 `transform`／`translate`／`scale`／`opacity` 與顏色三態
  （`color`／`background-color`／`border-color`），不觸發 layout。
- 浮層進出場：原生 `<dialog>` 與 popover 用 `@starting-style` +
  `transition-behavior: allow-discrete`（WebView2 evergreen 支援，不支援時退化為瞬切）；
  條件渲染面板用 Svelte transition（進場 `--motion-dur-3` ease-out、離場一律
  `--motion-dur-out` ease-in；tooltip 與桌面選單離場維持瞬間）。

## 後果

- 正面：所有使用者的動畫體驗一致且可預期；每幀只走合成器（拖曳呼吸指示由逐幀重繪
  box-shadow 改為 `::before` overlay 動 opacity）。
- 負面／接受：對極少數在 OS 關閉動畫的前庭敏感使用者，RoudaMix 仍會播放短暫微動畫；
  這是工具語意一致性的刻意取捨（本 ADR 記錄之）。
