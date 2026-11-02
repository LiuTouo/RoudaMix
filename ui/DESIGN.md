---
name: RoudaMix 桌面主控台
description: 夜航混音台——Windows 音訊工作區的深色精密儀表語彙
colors:
  carbon-panel: "#0D0F10"
  panel: "#121416"
  console-surface: "#181B1E"
  raised: "#202428"
  well-black: "#090B0D"
  hairline: "#292D31"
  divider: "#1E2225"
  ink: "#E2E5E8"
  ink-dim: "#9CA5AE"
  master-blue: "#6EA8FE"
  scope-green: "#55C2A3"
  meter-green: "#40C985"
  amber-warn: "#D9A441"
  clip-red: "#DD5A5A"
typography:
  title:
    fontFamily: "IBM Plex Sans TC, Segoe UI, system-ui, sans-serif"
    fontSize: "14px"
    fontWeight: 600
    letterSpacing: "0.3px"
  body:
    fontFamily: "IBM Plex Sans TC, Segoe UI, system-ui, sans-serif"
    fontSize: "12px"
    fontWeight: 400
    lineHeight: 1.45
  label:
    fontFamily: "IBM Plex Sans TC, Segoe UI, system-ui, sans-serif"
    fontSize: "11px"
    fontWeight: 400
  readout:
    fontFamily: "ui-monospace, Cascadia Mono, Consolas, monospace"
    fontSize: "11px"
    fontWeight: 400
    fontFeature: "tabular-nums"
rounded:
  control: "3px"
  strip: "8px"
  well: "12px"
spacing:
  xs: "4px"
  sm: "6px"
  md: "8px"
  lg: "12px"
  xl: "16px"
  board: "20px"
components:
  button-primary:
    backgroundColor: "{colors.master-blue}"
    textColor: "{colors.carbon-panel}"
    rounded: "{rounded.control}"
    padding: "4px 10px"
  button-primary-hover:
    backgroundColor: "{colors.ink}"
  button-ghost:
    backgroundColor: "transparent"
    textColor: "{colors.ink}"
    rounded: "{rounded.control}"
    padding: "4px 10px"
  button-ghost-hover:
    backgroundColor: "{colors.raised}"
  strip-card:
    backgroundColor: "{colors.raised}"
    rounded: "{rounded.strip}"
  group-well:
    backgroundColor: "{colors.well-black}"
    rounded: "{rounded.well}"
  dialog-surface:
    backgroundColor: "{colors.console-surface}"
    rounded: "{rounded.control}"
---

# Design System: RoudaMix 桌面主控台

## Overview

**Creative North Star: "夜航混音台"**

深夜錄音室裡那張燈光昏暗的硬體混音台:近黑的霧面面板、細如髮絲的框線分割、只有訊號在的地方才亮燈。UI 是冷靜精密的儀器,不是裝飾的舞台——聲音是主角,介面退後成面板。所有視覺決策都服務「開播中的人掃一眼就懂」:狀態用燈色說話,讀值用等寬數字對齊,操作在需要時才現形。

這是一套**專業密度**的系統:12px 基準字級、24–28px 列高、32px 工作列,深色但對比充分(ink #E2E5E8 on carbon-panel #0D0F10)。暖色是稀有資源——琥珀與紅只屬於警告與危險,藍是焦點與主控,綠是訊號健康。浮層(對話框、抽屜、capsule)不投影、不浮起,僅以較亮一階的面板色與細框與底層區隔;真正的陰影只留給「被手拿起來」的東西(拖曳幽靈)與貼邊的抽屜。

**Key Characteristics:**
- 方角為預設:全域 border-radius 重置為 0,圓角只出現在明確賦予的地方
- 髮絲框線 1px hairline(#292D31)劃分一切,不用色塊堆疊層級
- 面板靠色階抬升:carbon-panel → panel → console-surface → raised,四階走完深度
- 數值一律 mono + tabular-nums(dB、%、延遲、計數)
- 動畫專業克制:90/140/180ms 階梯,只動 transform/translate/scale/opacity 與三態顏色
- 互動前安靜:幽靈圖示鈕無底色無框,hover 才浮出 raised 底

## Colors

色板是「暗房裡的儀表燈」:中性面皆帶極微的冷藍灰傾向,彩色只作為狀態燈存在,任一畫面上彩色面積遠低於一成。

### Primary
- **主控藍 Master Blue**(#6EA8FE):焦點、主操作、可互動的確認色。focus-visible 外框、下拉底線、tabs 作用中、primary 按鈕、拖曳虛線落點。它是面板上唯一的「往前」色。
- **示波綠 Scope Green**(#55C2A3):啟用與健康——推桿電源燈、checkbox 勾選、MON/STR 送出開關、低延遲指示。名稱取自示波器的穩定波形。

### Secondary
- **琥珀警示 Amber**(#D9A441):警告層——插件 bypass 燈(--rack-bg #332A1B 底)、placeholder 插件左緣、meter 進入警告段、rebind 邊框。僅用於「需要注意但未故障」;工作列的負載/XRUN/plugin fail 計數屬此層(警示發生中但引擎已自處)。
- **剪波紅 Clip Red**(#DD5A5A):危險與故障——meter 剪波、danger 動作 hover(--hot-bg #382022 底 --hot-ink #F0ABAB 字)、capsule 錯誤框、per-track/設定錯誤文字。永不用於裝飾。

### Neutral
- **碳黑面板 Carbon Panel**(#0D0F10):視窗底、最深層。所有深度的起點。
- **面板 Panel**(#121416):第一階抬升(舊 tokens --bg-panel;現行多被下層角色吸收)。
- **主控面 Console Surface**(#181B1E):浮層本體(對話框、tooltip、右鍵選單、capsule)與 hover 深面(--flat-2;--flat-3 已併入此值)。
- **抬升面 Raised**(#202428):軌道卡底色與一切 hover 底色(--flat-4)。
- **凹槽黑 Well Black**(#090B0D;token `--well`/`--well-edge` #080A0C):Input/Output 群組凹槽底,比視窗底更深,製造「嵌進面板」的下陷感。
- **髮絲線 Hairline**(#292D31):全 UI 邊框與分割線。
- **暗分割 Divider**(#1E2225):同群組內的次級分割(機架 footer、插件列間)。
- **墨 Ink**(#E2E5E8):主文字。
- **暗墨 Ink Dim**(#9CA5AE):標籤、次要文字、圖示;也是拖曳 ruler 刻度與推桿軌的基色(#8A93A3 同族)。

### Named Rules
**狀態三色分離 Rule。** 啟用(active/ok)、警告(warn)、危險(err)三種狀態各有專屬燈色,絕不混用:切換類的「on」不是警告,危險操作只允許 hover 時染紅。暖色永不出現在非警示語境。
**稀有彩度 Rule。** 中性灰階承擔 90% 以上的畫面;彩色只在語義成立處亮燈。要加強 hierarchy 時提高色階或字重,不是加顏色。

## Typography

**字族:** IBM Plex Sans TC(本機打包 400/500–600/700,OFL;fallback Segoe UI → system-ui),以 `--font-ui` token 貫穿全 UI(主控台、浮層、tooltip 同源);使用者可在「設定 → 通用 → 介面字型」改選本機已安裝字族(存 `settings.json` 的 `uiFontFamily`),字族名須過引號/反斜線安全檢查(settings.rs `opt_font` 與 `uiFont.ts` `sanitizeFontFamily` 同規則)。**讀值:** ui-monospace / Cascadia Mono / Consolas。

**Character:** 人文粗矩形的 IBM Plex 帶工程文件的克制;等寬字保留給一切會變動的數值,像設備上的 LCD 讀出。中文為主、尺寸小巧,靠字重與明暗而不是字級跳動分層。

### Hierarchy
- **Title**(600, 14px, +0.3px letter-spacing):群組標題(Inputs/Outputs、對話框標題)。全系統最大字級——這是刻意儀表化。
- **Body**(400, 12px, 1.45):主控台基準字級;選單、插件名、一般內容。
- **Label**(400, 11px):軌道小標籤、 MON/STR、技術附註、tooltip。
- **Readout**(mono, 10–11px, tabular-nums):dB、增益 %、延遲 ms、軌數計數、路徑資訊。一律 tabular-nums 對齊。

### Named Rules
**讀值上錶 Rule。** 凡是會跳動的數字必須 mono + tabular-nums;正文與標籤永不用等寬。

## Layout

單一工作區、無路由:頂部 32px 工作列(狀態燈 + telemetry 讀數),下方 main 16px 內距,主體是 Inputs/Outputs 兩欄 board grid(minmax(256px,2fr) / minmax(256px,1fr),gap 20px),每欄是一個 12px 圓角凹槽井(well),內容垂直排列軌道。每條軌道是 230px 寬的固定卡(strip,8px 圓角),以 16px lane pitch 水平虛擬化排列——間距是拖曳命中測試的契約,不可改。軌道卡下半固定為「左推桿/錶 + 右全高 VST 機架」;機架高度可拖曳並存於 localStorage。回應式僅一檔:≤1000px 時工作列換行。視窗最小高 640px。

密度契約:列高 24–28px、工作列 32px、群組頭 48px;間距單位 4/6/8/12/16/20。

## Elevation & Depth

**平是常態,亮階代陰影。** 深度由四階面板色(carbon-panel → panel → console-surface → raised)加上髮絲線達成;浮層明文 `box-shadow: none`。凹陷(井)是唯一用 inset shadow 表達的方向:群組井 `inset 0 3px 9px #000A` 加一絲上緣受光 `0 1px 0 #FFFFFF0D`。

### Shadow Vocabulary
- **凹槽井**(`inset 0 3px 9px #000A, 0 1px 0 #FFFFFF0D`):Input/Output 群組容器,嵌進面板的下陷感。
- **拖曳幽靈**(`0 18px 36px rgba(0,0,0,.45), 0 6px 12px rgba(0,0,0,.3)` + accent 細框 + scale 1.05):僅「被拿起」的軌道卡允許浮起投影。
- **延遲抽屜**(`-12px 0 28px #0008`):貼右緣的 drawer,陰影表達與主體的分層。
- **Tooltip**(`0 4px 14px rgb(0 0 0 / .5)`):唯一的小陰影,因為它懸浮在一切之上(body 層)。

### Named Rules
**拿起才浮起 Rule。** 靜止的 UI 永不投影;陰影只屬於拖曳中的物件、貼邊抽屜與 tooltip。

## Shapes

全域方角:`#app .audio-console * { border-radius: 0 }` 是整個系統的基準宣言——之後每一個圓角都是明確決定:控制項 3px、軌道卡 8px、群組井 12px。早期樣式裡的藥丸(11px)已被此重置覆蓋,現行語彙是直角硬邊。線條一律 1px hairline;兩種例外刻度:tooltip 與同族浮層的**左緣 2px accent**(頻道焦點燈)、placeholder 插件的**左緣 2px warn**。圓形只屬於訊號燈:電源 dot(6px)、radio(7px)、狀態點。指示器幾何:下拉選單以內嵌 SVG chevron 取代原生外觀,select 底線 1px 只在 focus/展開時染藍。

## Components

共用語彙:邊框 0 或 1px hairline;hover 一律 raised 底 + 90ms standard 曲線;focus 一律 1px accent outline(offset -1,色條內縮 2px);disabled 一律 opacity .4。動畫一律播放,不連動 OS「減少動態」設定(ADR 0007),只動 transform/translate/scale/opacity 與三態顏色;時長階梯 90ms(狀態)/140ms(小浮層)/180ms(對話框、軌道進場)/120ms(一律離場)。

### Buttons
- **Shape:** 直角或 3px;高度 24–28px,無原生框。
- **Ghost(預設):** 透明底,hover 浮現 raised;圖示鈕(關閉、齒輪)平時僅暗墨示意——「靜音、互動才現形」。
- **Primary:** 主控藍底 + 碳黑字;hover 轉墨色(#E2E5E8)。
- **Toggle.on:** mute 亮紅(#382022 底/#F0ABAB 字/err 框);rack bypass 亮琥珀(#332A1B 底/warn 字);一般啟用態走示波綠 12% 混合底。
- **Danger:** 平時如常,enabled:hover 才染紅。

### Tabs
純文字,1px 底線:作用中 = 主控藍字 + 藍底線 + raised 底;無框無圓角。

### Selects / Fields
原生 control 去外觀:透明底、無框,僅 1px 透明底線;hover 浮 raised;focus 底線染主控藍。改名輸入框同語彙(accent 底線 + console-surface 底)。**此語彙涵蓋對話框與抽屜內的一切 select 與文字輸入框**(下拉自帶內嵌 chevron;文字輸入框 rest 態以 1px 髮絲底線示意可輸入)。

### Fader(推桿)
20×30px 金屬質感推桿頭:`linear-gradient(180deg,#89929B 0 calc(50% - 1px), #252A2F 中線, #89929B …)`——上半亮下半暗、中央 1px 暗刻線;4px 無框軌;左側 7px 刻度尺(repeating-linear-gradient,12 格)。Unity(0 dB)固定在行程上 2/3 處,以暗墨短線標示。focus 時推桿頭框轉主控藍。

### Meter(電平錶)
Canvas 繪製,雙柱(L/R):正常 #40C985、警告 #D9A441、剪波 #DD5A5A(剪波保持 hold,直到下一次量測清除)、刻度 #8A93A3、錶底 #181B1E、柱間 6% 白分格。屬於狀態燈系統:顏色永不另作裝飾用途。

### 軌道卡 Strip
230px 固定寬、8px 圓角、raised 底、1px hairline。頭部 32px:遮罩著色的軌道圖示(自訂色 + 兩層低透明同色光暈)、刪除鈕;底部整條識別色帶(colorbar)承載軌名——色帶以 inline matteColor 填滿,名稱直接疊其上,hover 時加 15% 黑霧。拖曳中原卡 0.22 透明 + accent 虛線框;其他卡以 transform 滑動預覽落點(160ms,僅拖曳中啟用 transition)。

### 浮層(對話框 / 抽屜 / Popover / 右鍵選單 / Capsule)
統一 console-surface 底 + 1px hairline + 3px 圓角 + **無投影**;內部再以 raised 表 hover、以 hairline 分段。進場 140–180ms 淡入微上浮(@starting-style;不支援時瞬切退化),離場一律 120ms 加速退場。右鍵選單本體走 Svelte 過場,外殼透明。

### Tooltip(全域 `.app-tooltip`)
body 層、方角、console-surface 底、1px hairline、**左緣 2px 主控藍刻度(頻道焦點燈)**、11px/1.5、tabular-nums、淡入 140ms、隱藏瞬間。一律使用 `data-tooltip`,禁用原生 `title`。

## Do's and Don'ts

### Do:
- **Do** 用四階面板色表達深度:carbon-panel(#0D0F10)→ panel(#121416)→ console-surface(#181B1E)→ raised(#202428)。
- **Do** 所有變動數值用 mono + tabular-nums(11px)。
- **Do** hover 底色一律 raised(#202428),過場 90ms standard。
- **Do** 狀態燈各守其色:啟用/健康 = 示波綠(#55C2A3),警示 = 琥珀(#D9A441),故障/剪波 = 紅(#DD5A5A),焦點/主控 = 藍(#6EA8FE)。
- **Do** 純圖示控制項附 `aria-label`,提示一律 `data-tooltip`(全域 tooltip 層呈現)。
- **Do** 拖曳回饋用 transform 滑動預覽;拿起感交給幽靈圖層(scale 1.05 + 深陰影,不傾斜)。

### Don't:
- **Don't** 給靜止面板加投影或大圓角;浮層明文無影,圓角需明確賦予(預設方角)。
- **Don't** 以原生 `title` 屬性做提示;一律走 `data-tooltip`。
- **Don't** 以 reduced-motion 媒體查詢閘門任何動畫(ADR 0007:一律播放);動畫只動 transform/translate/scale/opacity 與三態顏色。
- **Don't** 只靠顏色傳達狀態——剪波有 hold、燈有位置語彙,必要時配文字(WCAG 2.1 AA 亦要求)。
- **Don't** 更動 230px strip 寬與 16px lane pitch——虛擬化與拖曳命中測試依賴它們。
- **Don't** 在暖色語境外使用琥珀/紅;切換類的「on」不是警告。
