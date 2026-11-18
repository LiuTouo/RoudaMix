---
target: 官網 website/
total_score: 31
max_score: 40
na_heuristics: 
p0_count: 0
p1_count: 2
target_identity: "file:D:\\Administrator\\Documents\\GitHub\\RoudaMix\\website\\index.html"
target_fingerprint: "sha256:6146e62f0e7b7bfea7fdd7463fabc50d0e13c8b75b72968557c76bf4436d77e0"
target_path: "D:\\Administrator\\Documents\\GitHub\\RoudaMix\\website\\index.html"
timestamp: 2026-11-17T12-21-36Z
slug: website-index-html
---
# Critique — RoudaMix 官網（website/）

Method: ⚠️ DEGRADED: single-context（本 session 無 sub-agent 工具；A→B 順序執行）
Mode: Persuade（行銷＋六步互動教學）

## Design Health Score
| # | Heuristic | Score | Key Issue |
|---|-----------|-------|-----------|
| 1 | Visibility of System Status | 3 | 章節進度 aria-current、release 載入/失敗狀態齊全 |
| 2 | Match System / Real World | 4 | 術語對齊 CONTEXT.md、雙語、FAQ 誠實揭露延遲與限制 |
| 3 | User Control and Freedom | 3 | skip-link、章節直達、hash deep-link；scroll-jack 8 視區略長 |
| 4 | Consistency and Standards | 3 | 內部錨點用 ↗ 外連符號（hero CTA、nav 下載） |
| 5 | Error Prevention | 3 | 唯讀表面，release/audio 失敗皆有 fallback |
| 6 | Recognition Rather Than Recall | 3 | 章節導覽帶名稱；手機版名稱隱藏僅剩數字 |
| 7 | Flexibility and Efficiency | 3 | deep-link、鍵盤可達；回訪者仍需過 1 分鐘故事 |
| 8 | Aesthetic and Minimalist Design | 3 | 插圖密度高但目的明確；6–9px 標籤是雜訊 |
| 9 | Error Recovery | 3 | releaseFailure/audioError 文案可行動 |
| 10 | Help and Documentation | 3 | 六步快速入門＋FAQ 任務導向 |
| **Total** | | **31/40** | **Good** |

## Design Specificity Verdict
高度特定：同一個 mock DAW 視窗貫穿七 章，訊號路徑逐步累積——敘事結構就是產品真相（兩路各自合適）。通用語彙殘留：hero 漸層字＋網格背景（偵測器：gradient-text warning、codex-grid-background advisory）。

## What's Working
- 術語誠實：不虛構證據、FAQ 承認「會有延遲」「非 OBS 直接整合」。
- 語言切換保留章節 hash；雙語內容完整對等而非機翻。
- 插圖即教學：workspace→裝置→外掛→兩路→分流→OBS→Discord，記憶結構清楚。

## Priority Issues
- [P1] 無 prefers-reduced-motion：GSAP+Lenis 捲動劫持、blur 轉場，前庭障礙者無降級（ui 有 ADR 0007，官網沒有）。Fix：媒體查詢降為靜態章節。→ /impeccable audit
- [P1] 教學示意關鍵文字 6–9px：插圖內「＋ Audio 軌 → 選擇麥克風」「右鍵 → 監聽略過」在 390px 縮至 6–8px，行動裝置讀不到核心教學。Fix：行動版放大標註或抽成文字步驟。→ /impeccable adapt
- [P2] 內部錨點用 ↗ 外連符號：誤導「新分頁」預期。Fix：移除內部連結的 ↗。→ /impeccable clarify
- [P2] 手機 chapter-nav 目標緊迫（44px 寬擠 7px 數字）、brand 29px 高、語言鈕 38px 寬。→ /impeccable adapt
- [P3] gradient-text＋codex-grid 為通用 AI 語彙，品牌已足夠獨特。→ /impeccable quieter

## Persona Red Flags
- Casey（手機）：插圖 6px 字、章節名隱藏、↗ 誤導。
- Sam（a11y）：無 reduced-motion；focus ring 可見。
- Riley（邊界）：release API 失敗有 fallback；audio null 有預留文案。

## Minor Observations
- 章節數字 mono「05 / 07」對比 8.77 但 11px 偏小。
- window-dots 10px ratio 4.08（插圖裝飾，可接受）。

## Questions to Consider
- 回訪者需要再走一次 1 分鐘故事嗎？「跳至教學」是否該更顯眼？
- 插圖標籤能否在行動版退場、由文字承擔？
