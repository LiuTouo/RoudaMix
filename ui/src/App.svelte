<script lang="ts">
  import { onMount } from "svelte";
  import { open, save } from "@tauri-apps/plugin-dialog";
  import Knob from "./lib/Knob.svelte";
  import MeterCanvas from "./lib/MeterCanvas.svelte";
  import Rack from "./lib/Rack.svelte";
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
    ParamInfo,
    ScanModule,
    Snapshot,
  } from "./lib/types";

  let conn = $state<ConnectionStatus>({ connected: false, epoch: 0, engineVersion: "" });
  let snap = $state<Snapshot | null>(null);
  let status = $state<EngineStatus | null>(null);
  let meters = $state<MetersFrame | null>(null);
  let devices = $state<DeviceInfo[]>([]);
  let selected = $state("");
  let busy = $state(false);
  let notice = $state("");
  let sineFreq = $state(440);

  // rack 狀態
  let focusId = $state<number | null>(null);
  let paramInfos = $state<Map<number, ParamInfo[]>>(new Map());
  let optimistic = $state<Map<number, Map<number, number>>>(new Map());
  let scanOpen = $state(false);
  let scanning = $state(false);
  let modules = $state<ScanModule[]>([]);

  const rack = $derived(status?.rack ?? []);
  const focusSlot = $derived(rack.find((s) => s.instanceId === focusId) ?? null);
  const outStrip = $derived(meters?.strips?.find((s) => s.instanceId === 0xffffffff));

  // set_param 不廣播 status —— 樂觀值只在本地;下一次 status(rack ref 變)即被權威值覆蓋
  $effect(() => {
    void status?.rack;
    optimistic = new Map();
  });

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
    });
    await onMeters((m) => (meters = m));
    refreshDevices().catch(() => {});
  });

  async function refreshDevices() {
    try {
      const r = await engineCommand("list_devices");
      devices = (r.devices as DeviceInfo[]) ?? [];
      if (!selected && devices.length) selected = devices[0].deviceKey;
      if (notice === "not connected") notice = ""; // 啟動競態殘留,成功即清
    } catch (e) {
      notice = String(e);
    }
  }

  async function start() {
    busy = true;
    notice = "";
    try {
      await engineCommand("start", { deviceKey: selected, sampleRate: null });
    } catch (e) {
      notice = String(e);
    }
    busy = false;
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

  async function setSource(source: "sine" | "passthrough") {
    if (source === "passthrough" && status?.running) {
      const ok = window.confirm(
        "切到 Passthrough 會把輸入直接送到輸出。\n接喇叭可能產生回授嘯叫,建議先戴耳機。\n要繼續嗎?",
      );
      if (!ok) return;
    }
    try {
      await engineCommand("set_source", { source, sineFreq });
    } catch (e) {
      notice = String(e);
    }
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
      paramInfos = new Map(); // instanceId 全新,舊 focus 參數作廢
      focusId = null;
      const dk = r.deviceKey as string | null;
      if (dk && devices.some((d) => d.deviceKey === dk)) selected = dk;
      notice = "";
    } catch (e) {
      notice = String(e);
    }
  }

  // ---------- rack 操作 ----------

  async function focus(id: number) {
    focusId = id;
    if (!paramInfos.has(id)) {
      try {
        const r = await engineCommand("get_params", { instanceId: id });
        const infos = (r.params as ParamInfo[]) ?? [];
        paramInfos = new Map(paramInfos).set(id, infos);
      } catch (e) {
        notice = String(e);
      }
    }
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
      await focus(r.instanceId as number);
    } catch (e) {
      notice = String(e);
    }
  }

  function setParam(instanceId: number, paramId: number, v: number) {
    const m = new Map(optimistic);
    const inner = new Map(m.get(instanceId) ?? []);
    inner.set(paramId, v);
    m.set(instanceId, inner);
    optimistic = m;
    engineCommand("set_param", { instanceId, paramId, value: v }).catch((e) => {
      notice = String(e);
    });
  }

  function knobValue(slotId: number, paramId: number): number {
    return (
      optimistic.get(slotId)?.get(paramId) ??
      rack.find((s) => s.instanceId === slotId)?.params.find((p) => p.paramId === paramId)
        ?.normalized ??
      paramInfos.get(slotId)?.find((p) => p.paramId === paramId)?.normalized ??
      0
    );
  }

  const running = $derived(status?.running ?? false);
  const basename = (p: string) => p.split(/[\\/]/).pop() ?? p;
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
  {/if}
  {#if status?.pluginFails}
    <span class="err mono" title="RT 端 plugin process 失敗次數(失敗時維持 bypass 效果)"
      >plugin fail {status.pluginFails}</span
    >
  {/if}
</header>

<div class="devicebar">
  <select bind:value={selected} disabled={running || devices.length === 0}>
    {#each devices as d (d.deviceKey)}
      <option value={d.deviceKey}>{d.name} ({d.maxIn}in/{d.maxOut}out)</option>
    {:else}
      <option value="">(無 ASIO 裝置)</option>
    {/each}
  </select>
  {#if running}
    <button class="danger" onclick={stop} disabled={busy}>Stop</button>
  {:else}
    <button class="primary" onclick={start} disabled={busy || !selected}>Start</button>
  {/if}
  <button onclick={refreshDevices} disabled={running}>重新掃描</button>
  <button onclick={saveSession}>儲存 Session</button>
  <button onclick={loadSession}>載入 Session</button>

  <span class="sep"></span>

  <div class="seg">
    <button class:active={status?.source !== "passthrough"} onclick={() => setSource("sine")}
      >Sine</button
    >
    <button class:active={status?.source === "passthrough"} onclick={() => setSource("passthrough")}
      >Passthrough</button
    >
  </div>
  <input
    type="number"
    min="20"
    max="20000"
    bind:value={sineFreq}
    disabled={status?.source === "passthrough"}
    onchange={() => setSource("sine")}
  />
  <span class="dim mono">Hz</span>

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
            class:on={focusSlot.bypassed}
            onclick={() => bypass(focusSlot)}>{focusSlot.bypassed ? "Bypassed" : "Bypass"}</button
          >
        </div>
        <div class="knobs">
          {#each paramInfos.get(focusSlot.instanceId) ?? [] as p (p.paramId)}
            {#if !p.bypass}
              <Knob
                label={p.name}
                def={p.default}
                value={knobValue(focusSlot.instanceId, p.paramId)}
                onChange={(v) => setParam(focusSlot.instanceId, p.paramId, v)}
              />
            {/if}
          {/each}
        </div>
      </div>
      <p class="hint dim">旋鈕:拖曳繞中心轉 · 滾輪微調 · 雙擊回預設值</p>
    {:else}
      <div class="card">
        <div class="cardhead"><span>Engine 輸出</span></div>
        <MeterCanvas strip={outStrip} height={64} />
        <p class="hint dim">
          {rack.length
            ? "點左側 slot 檢視參數;＋ 加入 plugin"
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
  .sep {
    width: 1px;
    height: 22px;
    background: var(--border);
  }
  .seg {
    display: flex;
  }
  .seg button:first-child {
    border-radius: 6px 0 0 6px;
  }
  .seg button:last-child {
    border-radius: 0 6px 6px 0;
  }
  .seg button + button {
    border-left: none;
  }
  .seg button.active {
    background: var(--accent);
    color: #0d1117;
    border-color: var(--accent);
  }
  select,
  button,
  input {
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
  input {
    width: 72px;
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
  .knobs {
    display: flex;
    flex-wrap: wrap;
    gap: 12px;
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
