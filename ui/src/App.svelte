<script lang="ts">
  import { onMount } from "svelte";
  import { open, save } from "@tauri-apps/plugin-dialog";
  import MeterCanvas from "./lib/MeterCanvas.svelte";
  import Rack from "./lib/Rack.svelte";
  import SpectrumCanvas from "./lib/SpectrumCanvas.svelte";
  import {
    connectStatus,
    onConnection,
    onSnapshot,
    onEngineEvent,
    onMeters,
    engineCommand,
  } from "./lib/ipc";
  import type {
    ConnectionStatus,
    DeviceInfo,
    EngineStatus,
    MetersFrame,
    ScanModule,
    Snapshot,
  } from "./lib/types";

  let conn = $state<ConnectionStatus>({ connected: false, epoch: 0, engineVersion: "" });
  let snap = $state<Snapshot | null>(null);
  let status = $state<EngineStatus | null>(null);
  let meters = $state<MetersFrame | null>(null);
  let devices = $state<DeviceInfo[]>([]);
  let selected = $state("");
  let bufSize = $state<number | null>(null); // buffer 是 ASIO host 權威;實數,初始 = driver preferred
  let inputMono = $state(true); // ch1 複製 L+R(mic 監聽)
  let busy = $state(false);
  let notice = $state("");
  let panelOpen = $state(false); // 硬體面板開啟中:stream 可能停,關閉後自動重建

  // rack 狀態
  let focusId = $state<number | null>(null);
  let scanOpen = $state(false);
  let scanning = $state(false);
  let modules = $state<ScanModule[]>([]);

  const rack = $derived(status?.rack ?? []);
  // 沒明確選擇時 fallback 第一張:rack 非空必見面板(UI 重啟從快照恢復後
  // focusId 歸 null,右側空白會讓人找不到 GUI/Bypass 按鈕)
  const focusSlot = $derived(rack.find((s) => s.instanceId === focusId) ?? rack[0] ?? null);
  const outStrip = $derived(meters?.strips?.find((s) => s.instanceId === 0xffffffff));

  onMount(async () => {
    conn = await connectStatus().catch(() => conn);
    await onConnection((c) => {
      conn = c;
      if (c.connected && devices.length === 0) refreshDevices().catch(() => {});
    });
    await onSnapshot((s) => {
      snap = s;
      status = s.status;
    });
    await onEngineEvent((kind, payload) => {
      if (kind === "status") status = payload as EngineStatus;
      // 硬體面板關閉:driver 設定可能變(率),且 SSL 這類 driver 在面板動 buffer 後
      // 現有 stream 會死流 —— 一律重掃 + 重建(短暫中斷換取與硬體同步)
      if (kind === "devices_changed") onPanelClosed().catch(() => {});
    });
    await onMeters((m) => (meters = m));
    refreshDevices().catch(() => {});
  });

  // 裝置選定。取樣率 = 硬體面板權威(driver 現行);buffer = ASIO host 權威
  // (createBuffers 時 host 決定;driver 面板裡的緩衝選擇不影響 ASIO stream,會被蓋掉)
  function applyDeviceDefaults(dev: DeviceInfo) {
    selected = dev.deviceKey;
    // 新裝置 = driver preferred(清單沒有 preferred 時取最小值)
    bufSize = dev.bufferSizes.includes(dev.preferredBufferSize)
      ? dev.preferredBufferSize
      : (dev.bufferSizes[0] ?? null);
  }

  // 硬體面板關閉後:重掃 + 重建 stream(面板期間動過率/緩衝都會讓現有 stream 失效)
  async function onPanelClosed() {
    panelOpen = false;
    await refreshDevices();
    if (status?.running && !busy) restartWith(inputMono);
  }

  async function refreshDevices() {
    try {
      const r = await engineCommand("list_devices");
      devices = (r.devices as DeviceInfo[]) ?? [];
      if (!selected && devices.length) {
        applyDeviceDefaults(devices[0]);
        start(); // 自動啟用:開 app 即跑
      }
      if (notice === "not connected") notice = ""; // 啟動競態殘留,成功即清
    } catch (e) {
      notice = String(e);
    }
  }

  async function start(deviceKey?: string) {
    const key = deviceKey ?? selected;
    if (!key || busy || status?.running) return;
    busy = true;
    notice = "";
    try {
      await engineCommand("start", {
        deviceKey: key,
        sampleRate: null, // 率 = driver 現行(硬體面板權威)
        bufferSize: bufSize, // buffer = host 權威;null = driver preferred
        inputMono,
      });
    } catch (e) {
      notice = String(e);
    }
    busy = false;
  }

  // 跑著時改 mono/buffer:ASIO 要重建 = stop → start
  async function restartWith(mono: boolean) {
    if (busy) return;
    busy = true;
    notice = "";
    try {
      await engineCommand("stop");
      await engineCommand("start", {
        deviceKey: selected,
        sampleRate: null,
        bufferSize: bufSize,
        inputMono: mono,
      });
    } catch (e) {
      notice = String(e);
    }
    busy = false;
  }

  async function openDevicePanel() {
    try {
      await engineCommand("open_device_panel", {});
      panelOpen = true; // devices_changed 回來 = 面板關閉,清旗標並重建
    } catch (e) {
      notice = String(e);
    }
  }

  async function stop() {
    busy = true;
    try {
      await engineCommand("stop");
    } catch (e) {
      notice = String(e);
    }
    busy = false;
  }

  // ---------- session 存/載 ----------

  async function saveSession() {
    try {
      const path = await save({
        title: "儲存 Session",
        defaultPath: "session.rmsession",
        filters: [{ name: "RoudaMix Session", extensions: ["rmsession"] }],
      });
      if (!path) return;
      await engineCommand("save_session", {
        path,
        deviceKey: selected || null,
        sampleRate: status?.running ? Math.round(status.sampleRate) : null,
        bufferSize: status?.running ? status.bufferSize : bufSize,
        inputMono,
      });
      notice = "";
    } catch (e) {
      notice = String(e);
    }
  }

  async function loadSession() {
    try {
      const path = await open({
        title: "載入 Session",
        multiple: false,
        directory: false,
        filters: [{ name: "RoudaMix Session", extensions: ["rmsession"] }],
      });
      if (!path) return;
      const r = await engineCommand("load_session", { path });
      focusId = null; // instanceId 全新,舊 focus 作廢
      const dk = r.deviceKey as string | null;
      if (dk && devices.some((d) => d.deviceKey === dk)) {
        selected = dk;
        if (typeof r.inputMono === "boolean") inputMono = r.inputMono;
        // session 的 buffer:合法值才套,否則 driver preferred
        const sb = r.bufferSize as number | null;
        const dev = devices.find((d) => d.deviceKey === dk);
        if (sb && dev?.bufferSizes?.includes(sb)) bufSize = sb;
        else if (dev) applyDeviceDefaults(dev);
        start(); // 自動啟用:載入 session 即恢復現場(率跟 driver 現行值)
      }
      notice = "";
    } catch (e) {
      notice = String(e);
    }
  }

  // ---------- rack 操作 ----------

  function focus(id: number) {
    focusId = id;
  }

  async function bypass(slot: { instanceId: number; bypassed: boolean }) {
    try {
      await engineCommand("set_bypass", {
        instanceId: slot.instanceId,
        bypassed: !slot.bypassed,
      });
    } catch (e) {
      notice = String(e);
    }
  }

  async function move(id: number, delta: number) {
    const i = rack.findIndex((s) => s.instanceId === id);
    if (i < 0) return;
    const newIndex = i + delta;
    if (newIndex < 0 || newIndex >= rack.length) return;
    try {
      await engineCommand("move_plugin", { instanceId: id, newIndex });
    } catch (e) {
      notice = String(e);
    }
  }

  async function remove(id: number) {
    try {
      await engineCommand("remove_plugin", { instanceId: id });
      if (focusId === id) focusId = null;
    } catch (e) {
      notice = String(e);
    }
  }

  // 開啟即忘:關閉由 plugin 原生視窗自己做(engine 端冪等,已開時直接成功)
  async function openEditor(slot: { instanceId: number }) {
    try {
      await engineCommand("open_editor", { instanceId: slot.instanceId });
    } catch (e) {
      notice = String(e);
    }
  }

  // ---------- plugin preset(.vstpreset 檔案式 state)----------

  async function savePreset(slot: { instanceId: number; name: string }) {
    try {
      const path = await save({
        title: "儲存 Preset",
        defaultPath: `${slot.name}.vstpreset`,
        filters: [{ name: "VST3 Preset", extensions: ["vstpreset"] }],
      });
      if (!path) return;
      await engineCommand("save_preset", { instanceId: slot.instanceId, path });
      notice = "";
    } catch (e) {
      notice = String(e);
    }
  }

  async function loadPreset(slot: { instanceId: number }) {
    try {
      const path = await open({
        title: "載入 Preset",
        multiple: false,
        directory: false,
        filters: [{ name: "VST3 Preset", extensions: ["vstpreset"] }],
      });
      if (!path) return;
      await engineCommand("load_preset", { instanceId: slot.instanceId, path });
      notice = "";
    } catch (e) {
      notice = String(e);
    }
  }

  async function openScan() {
    scanOpen = true;
    scanning = true;
    try {
      const r = await engineCommand("scan_plugins", {});
      modules = (r.plugins as ScanModule[]) ?? [];
    } catch (e) {
      notice = String(e);
      scanOpen = false;
    }
    scanning = false;
  }

  async function addPlugin(path: string, classId: string) {
    try {
      const r = await engineCommand("add_plugin", { path, classId });
      scanOpen = false;
      focus(r.instanceId as number);
    } catch (e) {
      notice = String(e);
    }
  }

  const running = $derived(status?.running ?? false);
  const basename = (p: string) => p.split(/[\\/]/).pop() ?? p;
  const selDev = $derived(devices.find((d) => d.deviceKey === selected) ?? null);
</script>

<header class="bar">
  <span class="dot" class:ok={conn.connected}></span>
  <span>{conn.connected ? "已連線" : "連線中…"}</span>
  <span class="dim mono">engine {conn.engineVersion || "?"}</span>
  <span class="dim mono">epoch {conn.epoch}</span>
  <span style="flex:1"></span>
  {#if running}
    <span class="dot ok"></span>
    <span class="mono"
      >{status!.sampleRate} Hz · buf {status!.bufferSize} · lat
      {status!.inputLatency}/{status!.outputLatency} · xrun {status!.xruns}</span
    >
  {:else if selDev?.currentSampleRate}
    <span class="dim mono" title="driver 現行取樣率(在硬體面板改)">{selDev.currentSampleRate} Hz · buf {bufSize ?? selDev.preferredBufferSize}</span>
  {/if}
  {#if status?.pluginFails}
    <span class="err mono" title="RT 端 plugin process 失敗次數(失敗時維持 bypass 效果)"
      >plugin fail {status.pluginFails}</span
    >
  {/if}
</header>

<div class="devicebar">
  <select
    bind:value={selected}
    disabled={running || devices.length === 0}
    onchange={(e) => {
      const d = devices.find((x) => x.deviceKey === e.currentTarget.value);
      if (d) applyDeviceDefaults(d);
      start();
    }}
  >
    {#each devices as d (d.deviceKey)}
      <option value={d.deviceKey}>{d.name} ({d.maxIn}in/{d.maxOut}out)</option>
    {:else}
      <option value="">(無 ASIO 裝置)</option>
    {/each}
  </select>
  <label
    title="ASIO 緩衝 = host 決定(在這裡選,即時重建);driver 面板裡的緩衝選擇不影響 ASIO stream。小 = 低延遲,大 = 穩"
  >
    <select
      value={bufSize ?? ""}
      disabled={!selDev || selDev.bufferSizes.length === 0}
      onchange={(e) => {
        bufSize = Number(e.currentTarget.value);
        if (running) restartWith(inputMono);
      }}
    >
      {#each selDev?.bufferSizes ?? [] as b (b)}
        <option value={b}>{b}</option>
      {/each}
    </select>
  </label>
  <label title="輸入 ch1 複製到左右聲道(mic 監聽);關 = 立體聲 1:1"
    ><input
      type="checkbox"
      checked={inputMono}
      onchange={(e) => {
        inputMono = e.currentTarget.checked;
        restartWith(inputMono);
      }}
    />Mono</label
  >
  <button
    onclick={openDevicePanel}
    disabled={!running}
    title="開硬體驅動控制面板(取樣率在這改;緩衝請用 RoudaMix 的 Buffer 下拉 —— 面板的緩衝選擇會被 ASIO 蓋掉)。關閉面板後自動同步並重建"
    >硬體面板</button
  >
  {#if panelOpen}
    <span class="err mono" title="面板期間 driver 時脈可能切換,聲音中斷屬正常;關閉面板後自動重掃並重建 stream"
      >面板開啟中 — 聲音可能中斷,關閉面板後自動恢復</span
    >
  {/if}
  {#if running}
    <button class="danger" onclick={stop} disabled={busy}>Stop</button>
  {:else}
    <button class="primary" onclick={() => start()} disabled={busy || !selected}>Start</button>
  {/if}
  <button onclick={saveSession}>儲存 Session</button>
  <button onclick={loadSession}>載入 Session</button>

  <span style="flex:1"></span>
  {#if status?.error}
    <span class="err mono">{status.error}</span>
  {/if}
  {#if notice}
    <span class="err mono">{notice}</span>
  {/if}
</div>

<main>
  <Rack
    {rack}
    strips={meters?.strips ?? []}
    {focusId}
    onfocus={focus}
    onbypass={bypass}
    onmove={move}
    onremove={remove}
    onadd={openScan}
  />

  <section class="panel">
    {#if scanOpen}
      <div class="card scan">
        <div class="cardhead">
          <span>VST3 掃描</span>
          <button onclick={() => (scanOpen = false)}>×</button>
        </div>
        {#if scanning}
          <p class="dim">掃描中(數秒)…</p>
        {:else if modules.length === 0}
          <p class="dim">找不到 VST3(預設 Common Files\\VST3、Program Files\\VST3)</p>
        {:else}
          {#each modules as m (m.path)}
            <div class="mod">
              <div class="modpath mono" title={m.path}>{basename(m.path)}</div>
              <div class="classes">
                {#each m.classes as c (c.uid)}
                  <button onclick={() => addPlugin(m.path, c.uid)} title={`${c.vendor} ${c.version} · ${c.subcategories}`}>
                    {c.name}
                  </button>
                {/each}
              </div>
            </div>
          {/each}
        {/if}
      </div>
    {:else if focusSlot}
      <div class="card">
        <div class="cardhead">
          <span>{focusSlot.name}</span>
          <span class="dim mono" title={focusSlot.pluginPath}>{basename(focusSlot.pluginPath)}</span>
          <span style="flex:1"></span>
          <button
            onclick={() => openEditor(focusSlot)}
            title="開 plugin 自帶原生 GUI 視窗(engine process 內彈出);關閉用視窗自己的 ✕"
            >開啟原生 GUI</button
          >
          <button
            class:on={focusSlot.bypassed}
            onclick={() => bypass(focusSlot)}>{focusSlot.bypassed ? "Bypassed" : "Bypass"}</button
          >
          <button onclick={() => savePreset(focusSlot)} title="把目前參數存成 .vstpreset"
            >存 Preset</button
          >
          <button onclick={() => loadPreset(focusSlot)} title="載入 .vstpreset 套用"
            >載 Preset</button
          >
        </div>
        <MeterCanvas
          strip={meters?.strips?.find((s) => s.instanceId === focusSlot.instanceId)}
          height={48}
        />
      </div>
      <p class="hint dim">參數設定在 plugin 原生 GUI 視窗內(按上方式開啟)</p>
    {:else}
      <div class="card">
        <div class="cardhead"><span>Engine 輸出</span></div>
        <MeterCanvas strip={outStrip} height={64} />
        <SpectrumCanvas
          spectrum={meters?.spectrum ?? null}
          sampleRate={meters?.sampleRate ?? 0}
          height={120}
        />
        <p class="hint dim">
          {rack.length
            ? "點左側 slot 開 plugin 原生 GUI;＋ 加入 plugin"
            : "左側 ＋ 掃描並加入 VST3 plugin,再按 Start 出聲"}
        </p>
      </div>
    {/if}
  </section>
</main>

<style>
  .bar {
    display: flex;
    align-items: center;
    gap: 14px;
    padding: 8px 14px;
    background: var(--bg-panel);
    border-bottom: 1px solid var(--border);
  }
  .devicebar {
    display: flex;
    align-items: center;
    gap: 10px;
    padding: 10px 14px;
    background: var(--bg-panel);
    border-bottom: 1px solid var(--border);
  }
  .dot {
    width: 9px;
    height: 9px;
    border-radius: 50%;
    background: var(--warn);
  }
  .dot.ok {
    background: var(--ok);
  }
  .dim {
    color: var(--text-dim);
  }
  .mono {
    font-family: var(--mono);
    font-size: 12px;
  }
  .err {
    color: var(--warn);
  }
  select,
  button {
    background: var(--bg);
    color: var(--text);
    border: 1px solid var(--border);
    border-radius: 6px;
    padding: 5px 10px;
    font-size: 13px;
  }
  button.primary {
    background: var(--accent);
    color: #0d1117;
    border-color: var(--accent);
    font-weight: 600;
  }
  button.danger {
    background: var(--warn);
    color: #fff;
    border-color: var(--warn);
    font-weight: 600;
  }
  button:disabled,
  select:disabled {
    opacity: 0.45;
  }
  button.on {
    background: var(--warn);
    border-color: var(--warn);
    color: #14161a;
    font-weight: 600;
  }
  main {
    padding: 14px;
    display: flex;
    gap: 14px;
    flex: 1;
    min-height: 0;
  }
  .panel {
    flex: 1;
    min-width: 0;
    display: flex;
    flex-direction: column;
    gap: 10px;
    align-items: flex-start;
  }
  .card {
    background: var(--bg-panel);
    border: 1px solid var(--border);
    border-radius: 8px;
    padding: 10px 12px;
    display: flex;
    flex-direction: column;
    gap: 10px;
    width: 100%;
  }
  .cardhead {
    display: flex;
    align-items: center;
    gap: 10px;
    font-size: 13px;
  }
  .hint {
    margin: 0;
    font-size: 11px;
  }
  .scan {
    max-height: 100%;
    overflow-y: auto;
  }
  .mod {
    display: flex;
    flex-direction: column;
    gap: 4px;
    padding-bottom: 8px;
    border-bottom: 1px solid var(--border);
  }
  .mod:last-child {
    border-bottom: none;
    padding-bottom: 0;
  }
  .modpath {
    color: var(--text-dim);
    font-size: 11px;
  }
  .classes {
    display: flex;
    flex-wrap: wrap;
    gap: 6px;
  }
</style>
