<script lang="ts">
  // 軌條:輸入/輸出軌共用。由上而下 = 名稱列 → 來源/輸出裝置 → 目的地多選 →
  // VST 展開列表(電源=bypass、雙擊=原生 GUI、上→下=訊號序)→ 推桿+靜音 →
  // 錶 → 底部顏色條。engine 溝通自含(ipc 直呼),App 只餵狀態。
  import MeterCanvas from "./MeterCanvas.svelte";
  import { powerOff, powerOn } from "./icons";
  import { mountDragGhost, removeDragGhost } from "./ghost";
  import { engineCommand } from "./ipc";
  import { cssColor, parseColor, stripOfTrack } from "./tracks";
  import type {
    AudioApp,
    DeviceInfo,
    MeterStrip,
    RackSlot,
    RenderDevice,
    ScanModule,
    Track,
  } from "./types";

  let {
    track,
    tracks,
    devices,
    selectedDeviceKey,
    strips,
    dropBefore = false,
    dropAfter = false,
    dragging = false,
  }: {
    track: Track;
    tracks: Track[];
    devices: DeviceInfo[];
    selectedDeviceKey: string;
    strips: MeterStrip[] | undefined;
    dropBefore?: boolean;
    dropAfter?: boolean;
    dragging?: boolean;
  } = $props();

  let err = $state("");
  // 掃描(各軌自含:開了才掃,清單不共用);列表開在主視窗置中 dialog
  let scanning = $state(false);
  let modules = $state<ScanModule[]>([]);
  let scanDlg = $state<HTMLDialogElement | null>(null);
  let destDlg = $state<HTMLDialogElement | null>(null);
  // app 程序清單 / WASAPI render 裝置清單(focus 時拉,保持常新)
  let apps = $state<AudioApp[]>([]);
  let renderDevices = $state<RenderDevice[]>([]);

  const dev = $derived(devices.find((d) => d.deviceKey === selectedDeviceKey) ?? null);
  const isOutput = $derived(track.kind === "output");
  const basename = (p: string) => p.split(/[\\/]/).pop() ?? p;

  // ASIO pair 選項:基底偶數 ch,顯示「1/2 名稱」
  function pairOptions(names: string[], dir: "in" | "out") {
    const opts: { value: number; label: string }[] = [];
    for (let ch = 0; ch + 1 < names.length; ch += 2) {
      opts.push({ value: ch, label: `${ch + 1}/${ch + 2} ${names[ch]}` });
    }
    if (opts.length === 0 && (dir === "in" ? (dev?.maxIn ?? 0) : (dev?.maxOut ?? 0)) >= 2) {
      const n = dir === "in" ? (dev?.maxIn ?? 0) : (dev?.maxOut ?? 0);
      for (let ch = 0; ch + 1 < n; ch += 2) opts.push({ value: ch, label: `${ch + 1}/${ch + 2}` });
    }
    return opts;
  }

  async function setSource(value: string) {
    err = "";
    try {
      if (value === "") {
        await engineCommand("track_set_source", { trackId: track.trackId, source: null });
      } else if (value.startsWith("m")) {
        // 單聲道來源:m{ch} → 單 ch 複製到 L/R(mic 監聽兩耳)
        await engineCommand("track_set_source", {
          trackId: track.trackId,
          source: { type: "asioIn", channel: Number(value.slice(1)), mono: true },
        });
      } else {
        await engineCommand("track_set_source", {
          trackId: track.trackId,
          source: { type: "asioIn", channel: Number(value) },
        });
      }
    } catch (e) {
      err = String(e);
    }
  }

  async function loadApps() {
    try {
      const r = await engineCommand("list_audio_apps", {});
      apps = (r.apps as AudioApp[]) ?? [];
    } catch (e) {
      err = String(e);
    }
  }

  async function loadRenderDevices() {
    try {
      const r = await engineCommand("list_render_devices", {});
      renderDevices = (r.devices as RenderDevice[]) ?? [];
    } catch (e) {
      err = String(e);
    }
  }

  async function setAppSource(value: string) {
    err = "";
    if (value === "") {
      try {
        await engineCommand("track_set_source", { trackId: track.trackId, source: null });
      } catch (e) {
        err = String(e);
      }
      return;
    }
    const app = apps.find((a) => a.pid === Number(value));
    try {
      await engineCommand("track_set_source", {
        trackId: track.trackId,
        source: { type: "app", pid: Number(value), name: app?.name },
      });
    } catch (e) {
      err = String(e); // app_not_found / unsupported_windows 等
    }
  }

  async function setOutput(value: string) {
    err = "";
    try {
      if (value === "") {
        await engineCommand("track_set_output", { trackId: track.trackId, output: null });
      } else if (value.startsWith("asio:")) {
        await engineCommand("track_set_output", {
          trackId: track.trackId,
          output: { type: "asioOut", channel: Number(value.slice(5)) },
        });
      } else {
        await engineCommand("track_set_output", {
          trackId: track.trackId,
          output: { type: "wasapi", deviceId: value.slice(7) },
        });
      }
    } catch (e) {
      err = String(e);
    }
  }

  async function toggleDest(destId: number, checked: boolean) {
    err = "";
    const dests = checked
      ? [...track.dests, destId]
      : track.dests.filter((d) => d !== destId);
    try {
      await engineCommand("track_set_dests", { trackId: track.trackId, dests });
    } catch (e) {
      err = String(e); // cycle_detected 等:engine 權威,UI 顯示即可
    }
  }

  async function setColor(css: string) {
    err = "";
    try {
      await engineCommand("track_set", { trackId: track.trackId, color: parseColor(css) });
    } catch (e) {
      err = String(e);
    }
  }

  // 音量:拖曳中顯示本地值(thumb 不被 telemetry 回推拉回),60ms 節流送 engine
  let dragGain = $state<number | null>(null);
  let lastSent = 0;
  const shownGain = $derived(dragGain ?? track.gain);

  async function sendGain(v: number) {
    err = "";
    try {
      await engineCommand("track_set", { trackId: track.trackId, gain: v });
    } catch (e) {
      err = String(e);
    }
  }

  // 拖曳/滾輪時在滑鼠旁顯示當前數值(input 事件無座標,用 pointer 事件)
  let tip = $state<{ x: number; y: number; v: number } | null>(null);
  let tipTimer: ReturnType<typeof setTimeout> | undefined;
  let faderHeld = false;
  function showTip(x: number, y: number, v: number, sticky = true) {
    tip = { x, y, v };
    clearTimeout(tipTimer);
    if (!sticky) tipTimer = setTimeout(() => (tip = null), 600); // 滾輪 = 顯示後自動收
  }
  function onFaderDown(e: PointerEvent) {
    faderHeld = true;
    showTip(e.clientX, e.clientY, shownGain);
  }
  function onFaderMove(e: PointerEvent) {
    if (faderHeld) showTip(e.clientX, e.clientY, shownGain);
  }
  function onFaderUp() {
    faderHeld = false;
    tip = null;
  }

  function onGainInput(e: Event) {
    const v = Number((e.currentTarget as HTMLInputElement).value);
    dragGain = v;
    const now = performance.now();
    if (now - lastSent >= 60) {
      lastSent = now;
      void sendGain(v); // leading throttle;尾隨由 onchange 補送
    }
  }
  function onGainChange(e: Event) {
    tip = null;
    void sendGain(Number((e.currentTarget as HTMLInputElement).value));
  }
  function onGainClick(e: MouseEvent) {
    if (e.ctrlKey) {
      lastSent = performance.now();
      dragGain = 1; // ctrl+左鍵 = 恢復預設 1.0
      showTip(e.clientX, e.clientY, 1, false);
      void sendGain(1);
    }
  }
  let wheelTimer: ReturnType<typeof setTimeout> | undefined;
  function onGainWheel(e: WheelEvent) {
    e.preventDefault(); // 滾輪在推桿上 = 調音量,不捲頁面
    const cur = dragGain ?? track.gain;
    const v = Math.max(0, Math.min(1.5, cur - Math.sign(e.deltaY) * 0.02));
    dragGain = v;
    showTip(e.clientX, e.clientY, v, false);
    const now = performance.now();
    if (now - lastSent >= 60) {
      lastSent = now;
      void sendGain(v);
    } else {
      // 尾隨補送:最後幾格不能被 throttle 吃掉(engine 要追上 dragGain)
      clearTimeout(wheelTimer);
      wheelTimer = setTimeout(() => {
        lastSent = performance.now();
        void sendGain(dragGain ?? v);
      }, 70);
    }
  }
  // engine 廣播的 gain 追上本地值才清,thumb 不回跳
  $effect(() => {
    if (dragGain !== null && Math.abs(track.gain - dragGain) < 0.005) dragGain = null;
  });

  async function setMute(mute: boolean) {
    err = "";
    try {
      await engineCommand("track_set", { trackId: track.trackId, mute });
    } catch (e) {
      err = String(e);
    }
  }

  // ---- 雙擊改名(engine track_set 已支援 name)----
  let editing = $state(false);
  let draft = $state("");
  let nameInput: HTMLInputElement | undefined = $state();

  function startEdit() {
    draft = track.name;
    editing = true;
  }
  function commitName() {
    if (!editing) return; // 防 Esc 移除編輯框後 blur 二次送出
    editing = false;
    const n = draft.trim();
    if (n && n !== track.name) {
      err = "";
      engineCommand("track_set", { trackId: track.trackId, name: n }).catch(
        (e) => (err = String(e)),
      );
    }
  }
  $effect(() => {
    if (editing) nameInput?.select();
  });

  async function removeTrack() {
    err = "";
    try {
      await engineCommand("track_remove", { trackId: track.trackId });
    } catch (e) {
      err = String(e);
    }
  }

  // ---- VST 鏈 ----

  async function bypass(slot: RackSlot) {
    err = "";
    try {
      await engineCommand("set_bypass", {
        instanceId: slot.instanceId,
        bypassed: !slot.bypassed,
      });
    } catch (e) {
      err = String(e);
    }
  }

  // 開啟即忘:關閉由 plugin 原生視窗自己做(engine 端冪等)
  async function openEditor(slot: RackSlot) {
    err = "";
    try {
      await engineCommand("open_editor", { instanceId: slot.instanceId });
    } catch (e) {
      err = String(e);
    }
  }

  // ---- VST 鏈拖曳排序(move_plugin = erase+insert 最終位置;▲▼ 已移除)----
  let plugDrag = $state<number | null>(null); // 被拖 instanceId
  let plugDropAt = $state<number | null>(null); // 插入位(chain index)

  function onPlugDragStart(e: DragEvent, slot: RackSlot) {
    if ((e.target as HTMLElement).closest?.("button, input")) {
      e.preventDefault(); // 電源/移除鈕不開拖曳
      return;
    }
    e.stopPropagation(); // 別 bubble 到 lane 的軌道拖曳(會蓋 ghost + 誤開軌道排序)
    plugDrag = slot.instanceId;
    e.dataTransfer?.setData("text/plain", String(slot.instanceId));
    if (e.dataTransfer) {
      e.dataTransfer.effectAllowed = "move";
      const row = (e.target as HTMLElement).closest<HTMLElement>(".plug");
      if (row) mountDragGhost(e.dataTransfer, row, row.offsetWidth || 170);
    }
  }
  function onPlugDragOver(e: DragEvent, i: number) {
    if (plugDrag === null || !e.dataTransfer) return;
    e.preventDefault();
    e.dataTransfer.dropEffect = "move";
    const r = (e.currentTarget as HTMLElement).getBoundingClientRect();
    plugDropAt = e.clientY < r.top + r.height / 2 ? i : i + 1;
  }
  function onPlugDrop(e: DragEvent) {
    if (plugDrag === null) return;
    e.preventDefault();
    const from = track.plugins.findIndex((s) => s.instanceId === plugDrag);
    if (from >= 0 && plugDropAt !== null) {
      let ni = plugDropAt > from ? plugDropAt - 1 : plugDropAt; // 先移除造成左移要補回
      ni = Math.max(0, Math.min(ni, track.plugins.length - 1));
      if (ni !== from) {
        err = "";
        engineCommand("move_plugin", { instanceId: plugDrag, newIndex: ni }).catch(
          (e2) => (err = String(e2)),
        );
      }
    }
    plugDrag = null;
    plugDropAt = null;
    removeDragGhost();
  }
  function onPlugDragEnd() {
    plugDrag = null;
    plugDropAt = null;
    removeDragGhost();
  }

  async function removePlugin(id: number) {
    err = "";
    try {
      await engineCommand("remove_plugin", { instanceId: id });
    } catch (e) {
      err = String(e);
    }
  }

  async function scan() {
    scanning = true;
    err = "";
    try {
      const r = await engineCommand("scan_plugins", {});
      modules = (r.plugins as ScanModule[]) ?? [];
      scanDlg?.showModal(); // 掃完彈出置中列表(0 個也開,顯示「找不到」)
    } catch (e) {
      err = String(e);
    }
    scanning = false;
  }

  async function addPlugin(path: string, classId: string) {
    err = "";
    try {
      await engineCommand("add_plugin", { trackId: track.trackId, path, classId });
      modules = [];
      scanDlg?.close();
    } catch (e) {
      err = String(e);
    }
  }
</script>

<div
  class="strip"
  class:out={isOutput}
  class:dropbefore={dropBefore}
  class:dropafter={dropAfter}
  class:dragging={dragging}
  draggable="true"
  data-track-id={track.trackId}
  title="拖曳空白處排序 · 雙擊名稱改名"
>
  <div class="head">
    <input
      type="color"
      class="swatch"
      value={cssColor(track.color)}
      title="軌道顏色"
      onchange={(e) => setColor(e.currentTarget.value)}
    />
    {#if editing}
      <input
        class="nameedit"
        bind:this={nameInput}
        bind:value={draft}
        draggable="false"
        onkeydown={(e) => {
          if (e.key === "Enter") commitName();
          else if (e.key === "Escape") editing = false;
        }}
        onblur={commitName}
        ondblclick={(e) => e.stopPropagation()}
      />
    {:else}
      <span class="name" title={track.name} ondblclick={startEdit}>{track.name}</span>
    {/if}
    <span class="badge">{track.kind}</span>
    <span style="flex:1"></span>
    <button class="mini danger" onclick={removeTrack} title="刪除軌道">×</button>
  </div>

  <div class="row">
    {#if track.kind === "audio"}
      <span class="lbl">輸入</span>
      <select
        value={track.source?.type === "asioIn"
          ? track.source.mono
            ? `m${track.source.channel}`
            : String(track.source.channel)
          : ""}
        onchange={(e) => setSource(e.currentTarget.value)}
        disabled={!dev}
      >
        <option value="">(無)</option>
        {#each pairOptions(dev?.inputNames ?? [], "in") as o (o.value)}
          <option value={String(o.value)}>{o.label}</option>
        {/each}
        {#each (dev?.inputNames ?? []) as nm, ch (ch)}
          <option value="m{ch}">{ch + 1} {nm}(單聲)</option>
        {/each}
      </select>
    {:else if track.kind === "app"}
      <span class="lbl">輸入</span>
      <select
        value={track.source?.type === "app" ? String(track.source.pid) : ""}
        onfocus={loadApps}
        onchange={(e) => setAppSource(e.currentTarget.value)}
        title="抓該 App 的聲音(process loopback);清單 = 正在出聲的程式"
      >
        <option value="">(選 App — 點此重新整理)</option>
        {#each apps as a (a.pid)}
          <option value={String(a.pid)}>{a.name}</option>
        {/each}
      </select>
    {:else if track.kind === "fx"}
      <span class="lbl dim" title="FX 軌:上游軌把輸出指到這裡(insert 型)">insert · 無輸入</span>
    {:else}
      <span class="lbl">輸出裝置</span>
      <select
        value={track.output?.type === "asioOut"
          ? `asio:${track.output.channel}`
          : track.output?.type === "wasapi" && track.output.deviceId
            ? `wasapi:${track.output.deviceId}`
            : ""}
        onchange={(e) => setOutput(e.currentTarget.value)}
        onfocus={loadRenderDevices}
        title="ASIO 輸出 pair 或 WASAPI 裝置(串流用,如 VB-Cable)"
      >
        <option value="">(無)</option>
        {#if dev}
          <optgroup label="ASIO">
            {#each pairOptions(dev.outputNames, "out") as o (o.value)}
              <option value={`asio:${o.value}`}>{o.label}</option>
            {/each}
          </optgroup>
        {/if}
        <optgroup label="WASAPI">
          {#each renderDevices as d (d.id)}
            <option value={`wasapi:${d.id}`}>{d.name}{d.default ? "（預設）" : ""}</option>
          {/each}
        </optgroup>
      </select>
    {/if}
  </div>

  <button class="destsbtn" onclick={() => destDlg?.showModal()} title="選擇輸出目的地(勾選即套用)">
    輸出到 ({track.dests.length})
  </button>

  <div class="lower">
  <div class="vstcol">
  <details class="vst">
    <summary>VST ({track.plugins.length})</summary>
    {#each track.plugins as s, i (s.instanceId)}
      <div
        class="plug"
        draggable="true"
        class:dragging={plugDrag === s.instanceId}
        class:dropbefore={plugDropAt === i}
        class:dropafter={plugDropAt === i + 1 && plugDropAt === track.plugins.length}
        ondragstart={(e) => onPlugDragStart(e, s)}
        ondragover={(e) => onPlugDragOver(e, i)}
        ondrop={onPlugDrop}
        ondragend={onPlugDragEnd}
        title="拖曳上下排序"
      >
        <button
          class="mini power"
          class:off={s.bypassed}
          onclick={() => bypass(s)}
          title={s.bypassed ? "Bypassed(點此啟用)" : "啟用中(點此 Bypass)"}
        >
          <img class="picon" src={s.bypassed ? powerOff : powerOn} alt="" draggable="false" />
        </button>
        <span class="plugname" title="雙擊開啟 plugin 原生 GUI" ondblclick={() => openEditor(s)}
          >{s.name}</span
        >
        <button class="mini danger" onclick={() => removePlugin(s.instanceId)} title="移除">×</button>
      </div>
    {:else}
      <span class="dim">無插件</span>
    {/each}
    {#if !scanning}
      <span class="scanrow">
        <button class="mini add" onclick={scan}>＋ 掃描加入</button>
      </span>
    {:else}
      <span class="dim">掃描中…</span>
    {/if}
  </details>
  </div>

  <dialog bind:this={scanDlg} class="scanlistdlg" onclose={() => (modules = [])}>
    <div class="cardhead">
      <span>VST 插件列表 — 加入「{track.name}」</span>
      <span style="flex:1"></span>
      <button onclick={() => scanDlg?.close()} title="關閉(不加入)">×</button>
    </div>
    {#if modules.length === 0}
      <p class="dim">找不到 VST3</p>
    {:else}
      <div class="scanlist">
        {#each modules as m (m.path)}
          <div class="mod">
            <div class="modpath mono" title={m.path}>{basename(m.path)}</div>
            <div class="classes">
              {#each m.classes as c (c.uid)}
                <button
                  class="mini"
                  onclick={() => addPlugin(m.path, c.uid)}
                  title={`${c.vendor} ${c.version}`}
                  >{c.name}</button
                >
              {/each}
            </div>
          </div>
        {/each}
      </div>
    {/if}
  </dialog>

  <dialog bind:this={destDlg} class="destlistdlg">
    <div class="cardhead">
      <span>輸出到 — 「{track.name}」</span>
      <span style="flex:1"></span>
      <button onclick={() => destDlg?.close()} title="關閉">×</button>
    </div>
    <div class="destlist">
      {#each tracks.filter((t) => t.trackId !== track.trackId) as t (t.trackId)}
        <label class="dest">
          <input
            type="checkbox"
            checked={track.dests.includes(t.trackId)}
            onchange={(e) => toggleDest(t.trackId, e.currentTarget.checked)}
          />
          <span class="dot" style="background:{cssColor(t.color)}"></span>
          {t.name}
        </label>
      {:else}
        <span class="dim">沒有其他軌道</span>
      {/each}
    </div>
  </dialog>

  <div class="fader">
    <div class="fctl">
      <button
        class="mini mute"
        class:on={track.mute}
        onclick={() => setMute(!track.mute)}
        title="靜音">M</button
      >
      <input
        type="range"
        min="0"
        max="1.5"
        step="0.01"
        value={shownGain}
        oninput={onGainInput}
        onchange={onGainChange}
        onclick={onGainClick}
        onwheel={onGainWheel}
        onpointerdown={onFaderDown}
        onpointermove={onFaderMove}
        onpointerup={onFaderUp}
        onpointercancel={onFaderUp}
        title="音量(滾輪微調 · ctrl+點擊 = 恢復 100%)"
      />
      <span class="gain mono">{Math.round(shownGain * 100)}%</span>
    </div>
    <div class="meterwrap">
      <MeterCanvas strip={stripOfTrack(track.trackId, strips)} />
    </div>
  </div>
  </div>

  <div class="colorbar" style="background:{cssColor(track.color)}"></div>

  {#if err || track.error}
    <p class="err mono">{err || track.error}</p>
  {/if}

  {#if tip}
    <div class="gaintip mono" style="left:{tip.x + 14}px; top:{tip.y - 28}px"
      >{Math.round(tip.v * 100)}%</div
    >
  {/if}
</div>

<style>
  .strip {
    background: var(--bg-panel);
    border: 1px solid var(--border);
    border-radius: 8px;
    padding: 8px 10px 0;
    display: flex;
    flex-direction: column;
    gap: 8px;
    flex: 0 0 250px;
    width: 250px;
    min-height: 0;
    overflow-y: auto;
  }
  .strip.dragging {
    opacity: 0.35;
    border-style: dashed;
    border-color: var(--accent);
  }
  /* 插入指示:inset 不被 overflow/鄰件裁切 */
  .strip.dropbefore {
    box-shadow: inset 3px 0 0 0 var(--accent);
  }
  .strip.dropafter {
    box-shadow: inset -3px 0 0 0 var(--accent);
  }
  .head {
    display: flex;
    align-items: center;
    gap: 6px;
    min-width: 0;
  }
  .swatch {
    width: 16px;
    height: 16px;
    padding: 0;
    border: 1px solid var(--border);
    border-radius: 4px;
    background: none;
    cursor: pointer;
  }
  .name {
    font-size: 13px;
    font-weight: 600;
    white-space: nowrap;
    overflow: hidden;
    text-overflow: ellipsis;
  }
  .nameedit {
    font-size: 13px;
    font-weight: 600;
    min-width: 0;
    flex: 1;
    padding: 1px 4px;
  }
  .badge {
    font-size: 10px;
    color: var(--text-dim);
    border: 1px solid var(--border);
    border-radius: 4px;
    padding: 0 4px;
  }
  .row {
    display: flex;
    align-items: center;
    gap: 6px;
    min-width: 0;
  }
  .row select {
    flex: 1;
    min-width: 0;
    padding: 3px 6px;
    font-size: 12px;
  }
  .lbl {
    color: var(--text-dim);
    font-size: 12px;
    flex-shrink: 0;
  }
  details {
    font-size: 12px;
  }
  summary {
    cursor: pointer;
    color: var(--text-dim);
    user-select: none;
  }
  summary:hover {
    color: var(--text);
  }
  .dest {
    display: flex;
    align-items: center;
    gap: 6px;
  }
  .dot {
    width: 8px;
    height: 8px;
    border-radius: 50%;
    flex-shrink: 0;
  }
  .vst {
    display: flex;
    flex-direction: column;
    gap: 4px;
  }
  .plug {
    display: flex;
    align-items: center;
    gap: 4px;
    padding-left: 10px;
    border-radius: 4px;
  }
  .plug.dragging {
    opacity: 0.35;
  }
  .plug.dropbefore {
    box-shadow: inset 0 2px 0 0 var(--accent);
  }
  .plug.dropafter {
    box-shadow: inset 0 -2px 0 0 var(--accent);
  }
  .power {
    display: inline-flex;
    align-items: center;
    padding: 2px 4px;
  }
  .picon {
    width: 12px;
    height: 12px;
    pointer-events: none;
  }
  .plugname {
    color: var(--text);
    padding: 2px 4px;
    cursor: default;
    font-size: 12px;
    white-space: nowrap;
    overflow: hidden;
    text-overflow: ellipsis;
    flex: 1;
    min-width: 0;
  }
  .plugname:hover {
    color: var(--accent);
  }
  .idx {
    color: var(--text-dim);
    font-size: 10px;
  }
  .mini {
    padding: 1px 6px;
    font-size: 11px;
    line-height: 1.4;
  }
  .power.off {
    color: var(--text-dim);
    opacity: 0.6;
  }
  .power:not(.off) {
    color: var(--ok);
  }
  .mute.on {
    background: var(--warn);
    border-color: var(--warn);
    color: #14161a;
    font-weight: 700;
  }
  .danger:hover {
    color: var(--err);
  }
  /* 下段 = fader(左)+ VST 欄(右);DOM 序 vst 在前,order 翻到右邊 */
  .lower {
    flex: 1;
    min-height: 0;
    display: flex;
    gap: 8px;
  }
  .vstcol {
    order: 2;
    flex: 1;
    min-width: 0;
    display: flex;
    flex-direction: column;
  }
  .vstcol .vst {
    flex: 1;
    min-height: 0;
    overflow-y: auto;
  }
  /* 垂直 fader(窄)+ 垂直錶;M / 推桿 / % 同一欄直排 */
  .fader {
    order: 1;
    flex: 0 0 auto;
    min-height: 0;
    display: flex;
    align-items: stretch;
    gap: 6px;
  }
  .fctl {
    display: flex;
    flex-direction: column;
    align-items: center;
    gap: 6px;
    min-height: 0;
  }
  .fader input[type="range"] {
    -webkit-appearance: none;
    appearance: none;
    writing-mode: vertical-lr;
    direction: rtl; /* min 下、max 上;WebView2(Chromium ≥123)原生支援 */
    width: 16px;
    min-width: 16px;
    flex: 1;
    min-height: 0;
    background: transparent;
    padding: 0;
  }
  /* 去網頁感:fader cap 樣式(槽 = 內凹深色,cap 帶 accent 上緣) */
  .fader input[type="range"]::-webkit-slider-runnable-track {
    width: 100%;
    border-radius: 4px;
    background: var(--bg);
    border: 1px solid var(--border);
  }
  .fader input[type="range"]::-webkit-slider-thumb {
    -webkit-appearance: none;
    width: 16px;
    height: 10px;
    border-radius: 3px;
    background: linear-gradient(180deg, #2e333a, #1a1d22);
    border: 1px solid #4a525c;
    border-top-color: var(--accent);
    box-shadow: 0 1px 4px rgb(0 0 0 / 0.6);
    margin-left: -1px; /* 抵銷 track border,對齊槽 */
  }
  .meterwrap {
    flex: 0 0 46px;
    min-height: 0;
    display: flex;
  }
  .gain {
    font-size: 11px;
    color: var(--text-dim);
  }
  .colorbar {
    height: 4px;
    border-radius: 2px;
    margin: 0 -10px; /* 吃掉 padding,通欄 */
  }
  /* 掃描/輸出目的地列表 dialog(主視窗置中;手法同 settingsdlg:open 才套 display) */
  .scanlistdlg,
  .destlistdlg {
    background: var(--bg-panel);
    color: var(--text);
    border: 1px solid var(--border);
    border-radius: 8px;
    padding: 14px 16px;
    width: min(440px, 90vw);
  }
  .scanlistdlg[open],
  .destlistdlg[open] {
    display: flex;
    flex-direction: column;
    gap: 10px;
    position: fixed;
    top: 50%;
    left: 50%;
    transform: translate(-50%, -50%);
    margin: 0;
  }
  .scanlistdlg::backdrop,
  .destlistdlg::backdrop {
    background: rgb(0 0 0 / 0.5);
  }
  .scanlistdlg .cardhead,
  .destlistdlg .cardhead {
    display: flex;
    align-items: center;
    gap: 8px;
    font-weight: 600;
    font-size: 13px;
  }
  .scanlistdlg p,
  .destlistdlg p {
    margin: 2px 0;
  }
  .destsbtn {
    background: none;
    border: none;
    color: var(--text-dim);
    padding: 0;
    font-size: 12px;
    text-align: left;
    cursor: pointer;
    align-self: flex-start;
  }
  .destsbtn:hover {
    color: var(--text);
  }
  .destlist {
    display: flex;
    flex-direction: column;
    gap: 6px;
    max-height: 320px;
    overflow-y: auto;
  }
  .scanlist {
    display: flex;
    flex-direction: column;
    gap: 8px;
    max-height: 320px;
    overflow-y: auto;
  }
  .mod {
    display: flex;
    flex-direction: column;
    gap: 2px;
  }
  .modpath {
    color: var(--text-dim);
    font-size: 10px;
  }
  .classes {
    display: flex;
    flex-wrap: wrap;
    gap: 4px;
  }
  .add {
    align-self: flex-start;
  }
  .scanrow {
    display: flex;
    gap: 4px;
    align-self: flex-start;
  }
  .dim {
    color: var(--text-dim);
    font-size: 11px;
  }
  .mono {
    font-family: var(--mono);
  }
  .err {
    color: var(--warn);
    font-size: 11px;
    margin: 0;
    padding-bottom: 6px;
  }
  .gaintip {
    position: fixed;
    z-index: 100;
    background: var(--bg-raised);
    border: 1px solid var(--accent);
    color: var(--text);
    font-size: 12px;
    line-height: 1.4;
    padding: 2px 7px;
    border-radius: 4px;
    pointer-events: none;
    box-shadow: 0 2px 10px rgb(0 0 0 / 0.5);
  }
</style>
