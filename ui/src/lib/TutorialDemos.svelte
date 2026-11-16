<script lang="ts">
  // 教學動畫:共用相位計時器(t 每 tick +1),各 demo 以 p = t % LOOP[demo] 分派狀態;
  // 游標 left/top 走 CSS transition 平滑滑動、按下以 .down 縮放,列表/面板以 .open
  // 淡入 —— 全部純 class 切換,動畫細節交給 CSS,無逐幀素材。
  let { demo, replayKey = 0 }: { demo: string; replayKey?: number } = $props();

  let t = $state(0);
  const TICK_MS = 620;
  const LOOP: Record<string, number> = {
    "pick-device": 9,
    "buffer-size": 9,
    "asio-panel": 12,
    "pick-wasapi": 9,
    "wasapi-note": 8,
    "add-track": 12,
    "set-output": 10,
    "add-plugin": 11,
    mix: 13,
    session: 14,
  };

  // 換步驟或按重播 → 相位歸零重演。
  $effect(() => {
    void demo;
    void replayKey;
    t = 0;
  });
  $effect(() => {
    const id = setInterval(() => (t += 1), TICK_MS);
    return () => clearInterval(id);
  });

  const p = $derived(t % (LOOP[demo] ?? 8));
  /** 在 [a,b) 相位區間內。 */
  const at = (a: number, b = a + 1) => p >= a && p < b;
</script>

<div class="demo" aria-hidden="true">
  {#if demo === "pick-device" || demo === "pick-wasapi"}
    {@const wasapi = demo === "pick-wasapi"}
    <div class="winbar"><i></i>設定 · 音訊裝置</div>
    <div class="row" style="left:20px; top:52px; width:340px;">
      <span class="lbl" style="width:70px;">音訊裝置</span>
      <div class="sel" class:on={at(1, 4)}>
        <div class="swap" style="min-width:130px;">
          <span class="sw" class:hide={p >= 4}>（請選擇）</span>
          <span class="sw over" class:show={p >= 4}>{wasapi ? "系統音訊 (WASAPI)" : "Studio Audio (ASIO)"}</span>
        </div>
        <span class="dim">▾</span>
      </div>
    </div>
    <div class="row" style="left:20px; top:156px;">
      <span class="lbl" style="width:70px;">狀態</span>
      <div class="swap" style="min-width:64px;">
        <span class="chip sw" class:hide={p >= 5}>未連線</span>
        <span class="chip ok sw over" class:show={p >= 5}>執行中</span>
      </div>
    </div>
    {#if !wasapi}
      <div class="row" style="left:20px; top:192px;">
        <span class="lbl" style="width:70px;">Buffer Size</span>
        <span class="mono dim">128</span>
      </div>
    {:else}
      <p class="cap" style="left:20px; top:192px;">無 ASIO 硬體也能使用</p>
    {/if}
    <div class="list" class:open={at(2, 4)} style="left:98px; top:72px;">
      {#if !wasapi}<div class="opt">Studio Audio (ASIO)</div>{/if}
      <div class="opt on">系統音訊 (WASAPI)</div>
      {#if !wasapi}<div class="opt">Mic Only (ASIO)</div>{/if}
    </div>
    <div class="cur" class:down={at(1) || at(3)} style="left:{p < 1 ? 60 : p < 2 ? 230 : p < 3 ? (wasapi ? 150 : 265) : 260}px; top:{p < 1 ? 200 : p < 2 ? 50 : p < 3 ? (wasapi ? 78 : 96) : 92}px;">
      <svg viewBox="0 0 24 24"><path d="M5 3l14 7-6.2 1.6L10 18.6z" /></svg>
    </div>

  {:else if demo === "buffer-size"}
    <div class="winbar"><i></i>設定 · 音訊裝置</div>
    <div class="row" style="left:20px; top:52px; width:340px;">
      <span class="lbl" style="width:90px;">Buffer Size</span>
      <div class="sel" class:on={at(1, 4)}>
        <div class="swap mono" style="min-width:36px;">
          <span class="sw" class:hide={p >= 4}>512</span>
          <span class="sw over" class:show={p >= 4}>128</span>
        </div>
        <span class="dim">▾</span>
      </div>
    </div>
    <div class="row" style="left:20px; top:170px;">
      <span class="lbl" style="width:90px;">延遲</span>
      <div class="swap" style="min-width:64px;">
        <span class="chip mono sw" class:hide={p >= 4}>10.7ms</span>
        <span class="chip ok mono sw over" class:show={p >= 4}>2.7ms</span>
      </div>
    </div>
    <div class="list" class:open={at(2, 4)} style="left:118px; top:72px;">
      <div class="opt mono">64</div>
      <div class="opt mono on">128</div>
      <div class="opt mono">256</div>
      <div class="opt mono">512</div>
    </div>
    <div class="cur" class:down={at(1) || at(3)} style="left:{p < 1 ? 60 : p < 2 ? 240 : p < 3 ? 270 : 265}px; top:{p < 1 ? 200 : p < 2 ? 50 : p < 3 ? 100 : 96}px;">
      <svg viewBox="0 0 24 24"><path d="M5 3l14 7-6.2 1.6L10 18.6z" /></svg>
    </div>

  {:else if demo === "asio-panel"}
    <div class="winbar"><i></i>設定 · 音訊裝置</div>
    <div class="row" style="left:20px; top:52px;">
      <span class="lbl" style="width:90px;">ASIO 控制面板</span>
      <button class="mbtn" class:on={at(1, 3)}>開啟面板</button>
    </div>
    <div class="row" style="left:20px; top:208px;">
      <span class="lbl" style="width:90px;">狀態</span>
      <div class="swap" style="min-width:72px;">
        <span class="chip sw" class:hide={p >= 9}>執行中</span>
        <span class="chip sw over" class:show={p >= 9 && p < 11}>重建引擎…</span>
        <span class="chip ok sw over" class:show={p >= 11}>執行中</span>
      </div>
    </div>
    <div class="panel" class:open={at(3, 9)} style="left:120px; top:76px; width:220px; height:128px;">
      <div class="panel-bar">Studio Audio 驅動面板</div>
      <div class="row" style="left:12px; top:38px;">
        <span class="lbl" style="width:52px;">取樣率</span>
        <div class="sel" class:on={at(4, 7)}>
          <div class="swap mono" style="min-width:48px;">
            <span class="sw" class:hide={p >= 7}>48000</span>
            <span class="sw over" class:show={p >= 7}>96000</span>
          </div>
          <span class="dim">▾</span>
        </div>
      </div>
      <div class="list" class:open={at(5, 7)} style="left:70px; top:58px;">
        <div class="opt mono">44100</div>
        <div class="opt mono">48000</div>
        <div class="opt mono on">96000</div>
      </div>
    </div>
    <div class="cur" class:down={at(1) || at(4) || at(6)} style="left:{p < 1 ? 60 : p < 2 ? 165 : p < 3 ? 210 : p < 4 ? 235 : p < 5 ? 245 : 265}px; top:{p < 1 ? 200 : p < 2 ? 50 : p < 3 ? 88 : p < 4 ? 116 : p < 5 ? 116 : 180}px;">
      <svg viewBox="0 0 24 24"><path d="M5 3l14 7-6.2 1.6L10 18.6z" /></svg>
    </div>

  {:else if demo === "wasapi-note"}
    <p class="cap" style="left:0; right:0; top:14px; text-align:center;">取樣率跟隨系統輸出裝置</p>
    <div class="mini" style="left:20px; top:56px; width:160px; height:62px;">
      <div class="panel-bar">Windows 音效設定</div>
      <div class="row" style="left:10px; top:34px;">
        <span class="lbl">取樣率</span>
        <div class="swap mono" style="min-width:72px;">
          <span class="sw" class:hide={p >= 4}>48000 Hz</span>
          <span class="sw over" class:show={p >= 4}>96000 Hz</span>
        </div>
      </div>
    </div>
    <span class="arrow" class:show={at(1, 7)} style="left:186px; top:100px;">→</span>
    <div class="mini open" style="left:216px; top:56px; width:170px; height:62px;">
      <div class="panel-bar">RoudaMix</div>
      <div class="row" style="left:10px; top:34px;">
        <span class="lbl">系統音訊</span>
        <div class="swap mono" style="min-width:64px;">
          <span class="sw" class:hide={p >= 5}>48 kHz</span>
          <span class="sw ok over" class:show={p >= 5}>96 kHz</span>
        </div>
      </div>
    </div>
    <p class="cap dim" style="left:0; right:0; top:150px; text-align:center;">WASAPI 無 Buffer 選項</p>

  {:else if demo === "add-track"}
    <div class="winbar"><i></i>RoudaMix</div>
    <div class="row" style="left:20px; top:36px;">
      <button class="mbtn" class:on={at(1, 3)}>＋ 新增音軌</button>
    </div>
    <div class="strip" class:open={p >= 3} style="left:20px; right:20px; top:74px;">
      <span class="tname">麥克風</span>
      <span class="cell">
        <span class="lbl">來源</span>
        <div class="swap" style="min-width:44px;">
          <span class="sw" class:hide={p >= 7}>未設定</span>
          <span class="sw over" class:show={p >= 7}>麥克風</span>
        </div>
      </span>
      <span class="cell mono dim">1 in</span>
    </div>
    <div class="list" class:open={at(5, 7)} style="left:120px; top:112px;">
      <div class="opt on">麥克風</div>
      <div class="opt">Line-In 1</div>
    </div>
    <div class="cur" class:down={at(1) || at(4) || at(6)} style="left:{p < 1 ? 60 : p < 2 ? 88 : p < 4 ? 160 : p < 5 ? 160 : 195}px; top:{p < 1 ? 200 : p < 2 ? 34 : p < 4 ? 130 : p < 5 ? 78 : 118}px;">
      <svg viewBox="0 0 24 24"><path d="M5 3l14 7-6.2 1.6L10 18.6z" /></svg>
    </div>

  {:else if demo === "set-output"}
    <div class="winbar"><i></i>RoudaMix</div>
    <div class="strip open" style="left:20px; right:20px; top:40px;">
      <span class="tname">麥克風</span>
      <span class="cell">
        <span class="lbl">輸出</span>
        <div class="swap" style="min-width:110px;">
          <span class="sw" class:hide={p >= 5}>Monitor</span>
          <span class="sw over" class:show={p >= 5}>Monitor＋Stream</span>
        </div>
        <span class="dim">▾</span>
      </span>
    </div>
    <p class="cap dim" style="left:0; right:0; top:152px; text-align:center;">監聽自己、同時送進輸出</p>
    <div class="list" class:open={at(2, 5)} style="left:104px; top:78px;">
      <div class="opt">✓ Monitor</div>
      <div class="opt on">Stream</div>
      <div class="opt">聲道 1-2</div>
    </div>
    <div class="cur" class:down={at(1) || at(4)} style="left:{p < 1 ? 60 : p < 2 ? 180 : 200}px; top:{p < 1 ? 200 : p < 2 ? 44 : 108}px;">
      <svg viewBox="0 0 24 24"><path d="M5 3l14 7-6.2 1.6L10 18.6z" /></svg>
    </div>

  {:else if demo === "add-plugin"}
    <div class="winbar"><i></i>RoudaMix</div>
    <div class="strip open" style="left:20px; right:20px; top:40px;">
      <span class="tname">麥克風</span>
      <div class="swap" style="min-width:56px; text-align:center;">
        <span class="slot sw" class:hide={p >= 4}>＋</span>
        <span class="slot filled sw over" class:show={p >= 4}>壓縮器</span>
      </div>
      <div class="swap" style="min-width:0;">
        <span class="sw"></span>
        <span class="slot sw over" class:show={p >= 6}>＋</span>
      </div>
    </div>
    <p class="cap dim" style="left:0; right:0; top:152px; text-align:center;">插槽依序串接；點擊已裝插件可開編輯器</p>
    <div class="list" class:open={at(2, 4)} style="left:96px; top:78px;">
      <div class="opt on">壓縮器</div>
      <div class="opt">等化器</div>
      <div class="opt">降噪</div>
    </div>
    <div class="cur" class:down={at(1) || at(3)} style="left:{p < 1 ? 60 : p < 2 ? 150 : 170}px; top:{p < 1 ? 200 : p < 2 ? 44 : 88}px;">
      <svg viewBox="0 0 24 24"><path d="M5 3l14 7-6.2 1.6L10 18.6z" /></svg>
    </div>

  {:else if demo === "mix"}
    <div class="winbar"><i></i>RoudaMix</div>
    <div class="strip open" style="left:20px; right:20px; top:36px;">
      <span class="tname">麥克風</span>
      <div class="meter" class:muted={at(8, 11)}>
        <i></i><i></i><i></i><i></i><i></i>
      </div>
      <div class="slider">
        <span class="fill" style="width:{at(4, 8) || p >= 11 ? 68 : 26}%"></span>
        <span class="knob" style="left:{at(4, 8) || p >= 11 ? 66 : 24}%"></span>
      </div>
      <button class="mbtn mute" class:on={at(8, 11)}>M</button>
    </div>
    <div class="row" style="left:20px; top:104px;">
      <span class="kchip" class:down={at(1)}>Ctrl</span>
      <span class="kchip" class:down={at(1)}>M</span>
      <span class="lbl">全域快捷鍵切換監聽靜音</span>
    </div>
    <p class="cap dim" style="left:0; right:0; top:140px; text-align:center;">音量拉高 → 錶擺動；M 靜音 → 錶歸零</p>
    <div class="cur" class:down={at(3) || at(7)} style="left:{p < 1 ? 60 : p < 2 ? 300 : p < 3 ? 260 : p < 7 ? 330 : 310}px; top:{p < 1 ? 200 : p < 2 ? 120 : p < 3 ? 100 : 44}px;">
      <svg viewBox="0 0 24 24"><path d="M5 3l14 7-6.2 1.6L10 18.6z" /></svg>
    </div>

  {:else if demo === "session"}
    <div class="winbar"><i></i>RoudaMix
      <span class="chip" class:ok={p >= 3} style="margin-left:auto;">{p >= 3 ? "已儲存" : "未儲存"}</span>
    </div>
    <div class="row" style="left:20px; top:34px;">
      <span class="kchip" class:down={at(1)}>Ctrl</span>
      <span class="kchip" class:down={at(1)}>S</span>
      <span class="lbl">儲存場景</span>
    </div>
    <div class="strip" class:open={p >= 2} style="left:20px; right:20px; top:64px;">
      <span class="tname">麥克風</span>
      <span class="cell mono dim">session.rmx</span>
    </div>
    <div class="panel" class:open={at(6, 12)} style="left:150px; top:96px; width:210px; height:62px;">
      <div class="panel-bar">Session</div>
      <div class="row" style="left:10px; top:34px; right:10px;">
        <span class="mono">session.rmx</span>
        <button class="mbtn" class:on={at(8, 10)}>還原</button>
      </div>
    </div>
    <p class="cap dim late" class:show={at(10, 13)} style="left:0; right:0; top:172px; text-align:center;">還原後回到儲存時的場景</p>
    <div class="cur" class:down={at(8)} style="left:{p < 1 ? 60 : p < 6 ? 90 : 300}px; top:{p < 1 ? 200 : p < 6 ? 34 : 132}px;">
      <svg viewBox="0 0 24 24"><path d="M5 3l14 7-6.2 1.6L10 18.6z" /></svg>
    </div>
  {/if}
</div>

<style>
  /* mini 視窗 mockup:主題變數繪製,循環重演。純示意,無互動(aria-hidden)。 */
  .demo {
    position: relative;
    width: 420px;
    height: 240px;
    margin: 0 auto;
    background: var(--bg);
    border: 1px solid var(--border);
    border-radius: 6px;
    overflow: hidden;
    font-size: 11px;
    user-select: none;
  }
  .winbar {
    position: absolute;
    left: 0;
    right: 0;
    top: 0;
    height: 22px;
    display: flex;
    align-items: center;
    gap: 6px;
    padding: 0 8px;
    background: var(--bg-raised);
    border-bottom: 1px solid var(--border);
    color: var(--text-dim);
  }
  .winbar i {
    width: 8px;
    height: 8px;
    border-radius: 50%;
    background: var(--border);
  }
  .row {
    position: absolute;
    display: flex;
    align-items: center;
    gap: 8px;
  }
  .lbl { color: var(--text-dim); white-space: nowrap; }
  .mono { font-family: var(--mono); font-variant-numeric: tabular-nums; }
  .dim { color: var(--text-dim); }
  .sel {
    display: inline-flex;
    align-items: center;
    gap: 6px;
    padding: 2px 8px;
    background: var(--surface);
    border: 1px solid var(--border);
    border-radius: 3px;
    color: var(--text-dim);
  }
  .sel.on { border-color: var(--accent); color: var(--text); }
  /* 前後值切換：兩值疊在同位；疊層值預設隱藏，.show 才現（否則新舊值同時可見=文字重疊） */
  .swap { position: relative; display: inline-block; text-align: left; }
  .sw.over { position: absolute; left: 0; top: 50%; translate: 0 -50%; white-space: nowrap; opacity: 0; }
  .sw.over.show { opacity: 1; }
  .hide { opacity: 0; }
  .show { opacity: 1; }
  .list {
    position: absolute;
    z-index: 3;
    min-width: 150px;
    background: var(--surface);
    border: 1px solid var(--border);
    border-radius: 3px;
    box-shadow: 0 6px 18px rgb(0 0 0 / 0.45);
    opacity: 0;
    visibility: hidden;
    transition: opacity 0.18s, visibility 0.18s;
  }
  .list.open { opacity: 1; visibility: visible; }
  .opt { padding: 3px 8px; white-space: nowrap; color: var(--text-dim); }
  .opt.on { background: var(--bg-raised); color: var(--text); }
  .chip {
    padding: 1px 8px;
    border: 1px solid var(--border);
    border-radius: 999px;
    color: var(--text-dim);
    white-space: nowrap;
  }
  .chip.ok, .sw.ok { color: #7bd88f; }
  .chip.ok { border-color: #7bd88f; }
  .mbtn {
    padding: 2px 10px;
    font-size: 11px;
    border: 1px solid var(--border);
    border-radius: 3px;
    background: var(--bg-raised);
    color: var(--text);
  }
  .mbtn.on, .mbtn.mute.on { border-color: var(--accent); color: var(--accent); }
  .panel, .mini {
    position: absolute;
    background: var(--surface);
    border: 1px solid var(--border);
    border-radius: 4px;
    box-shadow: 0 8px 24px rgb(0 0 0 / 0.5);
    opacity: 0;
    visibility: hidden;
    transition: opacity 0.2s, visibility 0.2s, translate 0.2s;
    translate: 0 4px;
  }
  .panel.open, .mini.open { opacity: 1; visibility: visible; translate: 0 0; }
  .panel-bar {
    padding: 3px 8px;
    background: var(--bg-raised);
    border-bottom: 1px solid var(--border);
    border-radius: 4px 4px 0 0;
    color: var(--text-dim);
  }
  .strip {
    position: absolute;
    display: flex;
    align-items: center;
    gap: 10px;
    padding: 8px 10px;
    background: var(--surface);
    border: 1px solid var(--border);
    border-radius: 6px;
    opacity: 0;
    translate: 0 6px;
    transition: opacity 0.25s, translate 0.25s;
  }
  .strip.open { opacity: 1; translate: 0 0; }
  .tname { font-weight: 600; }
  .cell { display: inline-flex; align-items: center; gap: 6px; color: var(--text); }
  .slot {
    min-width: 56px;
    text-align: center;
    padding: 2px 8px;
    border: 1px dashed var(--border);
    border-radius: 3px;
    color: var(--text-dim);
    transition: opacity 0.2s;
  }
  .slot.filled { border-style: solid; border-color: var(--accent); color: var(--text); }
  .cap { position: absolute; color: var(--text); margin: 0; opacity: 1; }
  /* 延後出現的說明（如 session 還原提示）：預設隱藏 */
  .cap.late { opacity: 0; transition: opacity 0.2s; }
  .cap.late.show { opacity: 1; }
  .cap.dim, .cap.show { color: var(--text-dim); }
  .arrow {
    position: absolute;
    color: var(--text-dim);
    opacity: 0;
    transition: opacity 0.3s;
  }
  .arrow.show { opacity: 1; }
  .kchip {
    padding: 1px 7px;
    border: 1px solid var(--border);
    border-bottom-width: 2px;
    border-radius: 4px;
    background: var(--bg-raised);
    font-family: var(--mono);
    font-size: 10px;
    transition: transform 0.12s;
  }
  .kchip.down { transform: translateY(1px) scale(0.94); border-color: var(--accent); }
  .slider {
    position: relative;
    width: 90px;
    height: 4px;
    border-radius: 2px;
    background: var(--bg);
  }
  .fill {
    position: absolute;
    left: 0;
    top: 0;
    bottom: 0;
    border-radius: 2px;
    background: var(--accent);
    transition: width 0.4s;
  }
  .knob {
    position: absolute;
    top: -4px;
    width: 4px;
    height: 12px;
    margin-left: -2px;
    border-radius: 2px;
    background: var(--text);
    transition: left 0.4s;
  }
  .meter {
    display: flex;
    align-items: flex-end;
    gap: 2px;
    height: 26px;
  }
  .meter i {
    width: 4px;
    height: 100%;
    border-radius: 1px;
    background: #7bd88f;
    transform-origin: bottom;
    animation: mt 0.9s ease-in-out infinite alternate;
  }
  .meter i:nth-child(2) { animation-delay: 0.12s; }
  .meter i:nth-child(3) { animation-delay: 0.24s; }
  .meter i:nth-child(4) { animation-delay: 0.36s; }
  .meter i:nth-child(5) { animation-delay: 0.48s; }
  @keyframes mt {
    from { transform: scaleY(0.25); }
    to { transform: scaleY(0.9); }
  }
  .meter.muted i { animation-name: mtoff; }
  @keyframes mtoff {
    from { transform: scaleY(0.08); }
    to { transform: scaleY(0.12); }
  }
  .cur {
    position: absolute;
    z-index: 5;
    width: 15px;
    height: 15px;
    color: var(--text);
    filter: drop-shadow(0 1px 2px rgb(0 0 0 / 0.6));
    transition: left 0.45s ease, top 0.45s ease, transform 0.1s;
    pointer-events: none;
  }
  .cur.down { transform: scale(0.78); }
  .cur svg { display: block; width: 100%; height: 100%; fill: currentColor; stroke: var(--bg); stroke-width: 1; }
</style>
