---
target: 桌面 App UI ui/
total_score: 33
max_score: 40
na_heuristics: 
p0_count: 0
p1_count: 1
target_identity: "file:D:\\Administrator\\Documents\\GitHub\\RoudaMix\\ui\\src\\App.svelte"
target_fingerprint: "sha256:ed80a362b23f88f82888669d9c6fae2daf6cdd94acbaed7c9046b14e3c272791"
target_path: "D:\\Administrator\\Documents\\GitHub\\RoudaMix\\ui\\src\\App.svelte"
timestamp: 2026-11-17T12-21-36Z
slug: ui-src-app-svelte
closed: true
---
# Critique — RoudaMix 桌面主控台 UI（ui/）

Method: ⚠️ DEGRADED: single-context（本 session 無 sub-agent 工具；A→B 順序執行）
Mode: Operate（音訊工作區主控台）

## Design Health Score
| # | Heuristic | Score | Key Issue |
|---|-----------|-------|-----------|
| 1 | Visibility of System Status | 4 | 電平表、PDC 讀值、LOAD%、引擎狀態、plugin fails、session 遮罩 |
| 2 | Match System / Real World | 4 | MON/STR、低延遲、MB 標記，術語即 CONTEXT.md 領域語言 |
| 3 | User Control and Freedom | 3 | ConfirmDialog、dirty 追蹤、Ctrl+S；未見全域 undo |
| 4 | Consistency and Standards | 3 | DESIGN.md 漂移 78 項（38 字級/26 圓角/14 顏色） |
| 5 | Error Prevention | 3 | 破壞性操作確認、lastWorkingDevice 智慧預設 |
| 6 | Recognition Rather Than Recall | 3 | 37 處 data-tooltip、GuidedTour；14 純圖示鈕無 tooltip |
| 7 | Flexibility and Efficiency | 4 | hotkey、UI 縮放/字型、監聽熱鍵、Session 管理 |
| 8 | Aesthetic and Minimalist Design | 3 | 密度高但分組清楚（inputs/outputs wells） |
| 9 | Error Recovery | 3 | errors.ts 可讀代碼、role=alert |
| 10 | Help and Documentation | 3 | GuidedTour、LatencyDrawer 解釋 PDC |
| **Total** | | **33/40** | **Good** |

## Design Specificity Verdict
「夜航混音台」語彙成立：碳黑面板、hairline 分隔、tabular-nums 讀值。偵測器 81 項中 78 為 advisory 漂移——是 DESIGN.md 過時而非風格失控；DOM 即時掃描僅 1 項，實際渲染面乾淨。

## What's Working
- 無障礙命名 0 缺口（114 個互動元素全有 aria-label/文字）。
- 狀態可見性典範：PDC M/S 毫秒讀值直接放在標題列。
- GuidedTour＋hotkey＋tooltip 三層漸進。

## Priority Issues
- [P1] 互動目標低於 24px：checkbox 7×7、＋軌鈕 40×21、齒輪 26×26、PDC 列 17px 高（WCAG 2.5.8 AA）。Fix：padding/僞元素擴大熱區。→ /impeccable audit
- [P2] DESIGN.md 漂移 78 項：flat-theme 別名層加入後文件未更新，設計系統失去權威。→ /impeccable document
- [P2] 14 個純圖示鈕無 data-tooltip（視窗控制鈕等）：違反自家「提示一律 data-tooltip」政策（aria-label 有，滑鼠提示無）。→ /impeccable polish
- [P3] console-prototype dev 模式壞：mock 缺 emit export、vite 把 ui/design/*.html 掃成 entry。Fix：補 export＋排除 design/。（非 impeccable 命令）
- [P3] GuidedTour 8px solid 邊框 ×2、console.css side-tab 警告。→ /impeccable polish

## Persona Red Flags
- Alex（老手）：hotkey/縮放/Session 齊全；小目標增加誤觸成本。
- Sam（a11y）：命名完整、focus ring 全可見但 1px 細；7px checkbox 鍵盤空間極小。
- Riley（邊界）：session 還原遮罩、scan 取消事件、ABI mismatch 遙測皆有處理。

## Minor Observations
- 10–11px 字級密集（混音台慣例可辯護，但 10px 中文筆畫多）。
- 偵測器 design-system-color 多為 rgba 白系 hairline——屬文件未涵蓋而非雜色。

## Questions to Consider
- checkbox 7×7 是刻意像素密度還是遺漏？
- DESIGN.md 該重提取（document）還是把 flat-theme 併入既有詞彙？
