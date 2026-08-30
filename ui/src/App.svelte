<script lang="ts">
  import { onMount } from "svelte";
  import { open, save } from "@tauri-apps/plugin-dialog";
  import TrackStrip from "./lib/TrackStrip.svelte";
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
    Track,
  } from "./lib/types";

  let conn = $state<ConnectionStatus>({ connected: false, epoch: 0, engineVersion: "" });
  let snap = $state<unknown>(null);
  let status = $state<EngineStatus | null>(null);
  let meters = $state<MetersFrame | null>(null);
  let devices = $state<DeviceInfo[]>([]);
  let selected = $state("");
  let bufSize = $state<number | null>(null); // buffer 是 ASIO host 權威;實數,初始 = driver preferred
  let busy = $state(false);
  let notice = $state("");
  let panelOpen = $state(false); // 硬體面板開啟中:stream 可能停,關閉後自動重建
  let settingsOpen = $state(false); // 設定 modal;已開再按 = 無操作,天然單例
  let settingsDlg = $state<HTMLDialogElement | null>(null);
  let tab = $state<"audio" | "about">("audio");
  let audioStale = $state(false); // 應該在跑但沒跑(啟動失敗)→ 頂欄極簡警示
  let ensuredDefaults = false; // 首次連線建「監聽/串流」;之後使用者刪光也不重 建
  $effect(() => {
    if (settingsOpen) settingsDlg?.showModal();
    // open 才 close:dialog.close() 對未 open 的 dialog throw InvalidStateError
    else if (settingsDlg?.open) settingsDlg.close();
  });

  const tracks = $derived<Track[]>(status?.tracks ?? []);
  const inputTracks = $derived(tracks.filter((t) => t.kind !== "output"));
  const outputTracks = $derived(tracks.filter((t) => t.kind === "output"));

  onMount(async () => {
    conn = await connectStatus().catch(() => conn);
    await onConnection((c) => {
      conn = c;
      if (c.connected && devices.length === 0) refreshDevices().catch(() => {});
    });
    await onSnapshot((s) => {
      snap = s;
      status = s.status;
      ensureDefaults().catch(() => {}); // 首次連上空場景也要建監聽/串流(snapshot 不走 status 事件)
    });
    await onEngineEvent((kind, payload) => {
      if (kind === "status") {
        status = payload as EngineStatus;
        ensureDefaults().catch(() => {});
      }
      // 硬體面板關閉:driver 設定可能變(率),且 SSL 這類 driver 在面板動 buffer 後
      // 現有 stream 會死流 —— 一律重掃 + 重建(短暫中斷換取與硬體同步)
      if (kind === "devices_changed") onPanelClosed().catch(() => {});
    });
    await onMeters((m) => (meters = m));
    refreshDevices().catch(() => {});
  });

  // 首次 snapshot 空 = 新場景:自動建輸出軌「監聽/串流」(監聽 = ASIO 主輸出 pair 0)
  async function ensureDefaults() {
    if (ensuredDefaults || !status || status.tracks.length > 0) return;
    ensuredDefaults = true;
    try {
      const r = await engineCommand("track_add", { kind: "output", name: "監聽" });
      await engineCommand("track_set_output", {
        trackId: r.trackId as number,
        output: { type: "asioOut", channel: 0 },
      });
      await engineCommand("track_add", { kind: "output", name: "串流" });
    } catch {
      // 連線競態:失敗就等下一個 status 事件再試
      ensuredDefaults = false;
    }
  }

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
    if (status?.running && !busy) restartWith();
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
      });
      audioStale = false;
    } catch (e) {
      audioStale = true;
      notice = String(e);
    }
    busy = false;
  }

  // 跑著時改 buffer/裝置:ASIO 要重建 = stop → start(換裝置 = 即時切換;
  // 新裝置開不起來 = audioStale + 設定頁錯誤,不 fallback)
  async function restartWith(key = selected) {
    if (busy) return;
    busy = true;
    notice = "";
    try {
      await engineCommand("stop");
      await engineCommand("start", {
        deviceKey: key,
        sampleRate: null,
        bufferSize: bufSize,
      });
      audioStale = false;
    } catch (e) {
      audioStale = true;
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
      audioStale = false; // 主動停 = 預期不跑,警示該滅
    } catch (e) {
      notice = String(e);
    }
    busy = false;
  }

  // ---------- 軌道新增 ----------

  async function addTrack(kind: "audio" | "app" | "fx" | "output") {
    try {
      await engineCommand("track_add", { kind });
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
        bufferSize: status?.running ? status.bufferSize : bufSize,
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
      const dk = r.deviceKey as string | null;
      if (dk && devices.some((d) => d.deviceKey === dk)) {
        selected = dk;
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

  const running = $derived(status?.running ?? false);
  const selDev = $derived(devices.find((d) => d.deviceKey === selected) ?? null);
</script>

<header class="bar">
  <span class="dot" class:ok={conn.connected}></span>
  <span>{conn.connected ? "已連線" : "連線中…"}</span>
  <button class="settings" onclick={() => (settingsOpen = true)}>設定</button>
  {#if audioStale}
    <button class="err aslink" onclick={() => (settingsOpen = true)} title={notice}
      >音訊未啟動 — 詳情見設定</button
    >
  {/if}
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

<main>
  <!-- 左欄:輸入軌(audio / app / fx) -->
  <section class="col">
    <div class="colhead">
      <span class="coltitle">輸入</span>
      <button class="mini" onclick={() => addTrack("audio")}>＋ Audio</button>
      <button class="mini" onclick={() => addTrack("app")}>＋ App</button>
      <button class="mini" onclick={() => addTrack("fx")}>＋ FX</button>
    </div>
    <div class="colbody">
      {#each inputTracks as t (t.trackId)}
        <TrackStrip track={t} {tracks} {devices} selectedDeviceKey={selected} strips={meters?.strips} />
      {:else}
        <p class="dim hint">用上方按鈕新增 Audio / App / FX 軌</p>
      {/each}
    </div>
  </section>

  <!-- 中央:留白(M5+ 再放 routing 總覽) -->
  <section class="center">
    <p class="dim hint">{running ? "中央區保留(日後:routing 總覽)" : ""}</p>
  </section>

  <!-- 右欄:輸出軌 -->
  <section class="col">
    <div class="colhead">
      <span class="coltitle">輸出</span>
      <button class="mini" onclick={() => addTrack("output")}>＋ 輸出軌</button>
    </div>
    <div class="colbody">
      {#each outputTracks as t (t.trackId)}
        <TrackStrip track={t} {tracks} {devices} selectedDeviceKey={selected} strips={meters?.strips} />
      {:else}
        <p class="dim hint">新增輸出軌(監聽 / 串流)</p>
      {/each}
    </div>
  </section>
</main>

<dialog
  bind:this={settingsDlg}
  class="settingsdlg"
  onclose={() => (settingsOpen = false)}
>
  <div class="cardhead">
    <span>設定</span>
    <span style="flex:1"></span>
    <button onclick={() => settingsDlg?.close()}>×</button>
  </div>
  <div class="tabs">
    <button class:on={tab === "audio"} onclick={() => (tab = "audio")}>音訊 / Session</button>
    <button class:on={tab === "about"} onclick={() => (tab = "about")}>關於</button>
  </div>
  {#if tab === "audio"}
    <div class="formrow">
      <label class="formlabel" for="setdev">裝置</label>
      <select
        id="setdev"
        bind:value={selected}
        disabled={devices.length === 0}
        onchange={(e) => {
          const d = devices.find((x) => x.deviceKey === e.currentTarget.value);
          if (d) {
            applyDeviceDefaults(d);
            restartWith(d.deviceKey); // 即時切換:stop → start 新裝置
          }
        }}
      >
        {#each devices as d (d.deviceKey)}
          <option value={d.deviceKey}>{d.name} ({d.maxIn}in/{d.maxOut}out)</option>
        {:else}
          <option value="">(無 ASIO 裝置)</option>
        {/each}
      </select>
    </div>
    <div class="formrow">
      <label class="formlabel" for="setbuf">Buffer</label>
      <select
        id="setbuf"
        value={bufSize ?? ""}
        disabled={!selDev || selDev.bufferSizes.length === 0}
        onchange={(e) => {
          bufSize = Number(e.currentTarget.value);
          restartWith();
        }}
      >
        {#each selDev?.bufferSizes ?? [] as b (b)}
          <option value={b}>{b}</option>
        {/each}
      </select>
    </div>
    <div class="formrow">
      <button
        onclick={openDevicePanel}
        disabled={!running}
        title="開硬體驅動控制面板(取樣率在這改;緩衝請用 Buffer 下拉 —— 面板的緩衝選擇會被 ASIO 蓋掉)。關閉面板後自動同步並重建"
        >硬體面板</button
      >
      {#if panelOpen}
        <span class="err mono"
          >面板開啟中 — 聲音可能中斷,關閉面板後自動恢復</span
        >
      {/if}
    </div>
    <div class="formrow">
      {#if running}
        <button class="danger" onclick={stop} disabled={busy}>Stop</button>
      {:else}
        <button class="primary" onclick={() => start()} disabled={busy || !selected}
          >Start</button
        >
      {/if}
      <span style="flex:1"></span>
      <button onclick={saveSession}>儲存 Session</button>
      <button onclick={loadSession}>載入 Session</button>
    </div>
    {#if status?.error}
      <p class="err mono">{status.error}</p>
    {/if}
    {#if notice}
      <p class="err mono">{notice}</p>
    {/if}
  {:else}
    <h2>關於</h2>
    <p>RoudaMix</p>
    <p class="dim mono">Engine 版本:{conn.engineVersion || "未知(尚未連線)"}</p>
  {/if}
</dialog>

<style>
  .bar {
    display: flex;
    align-items: center;
    gap: 14px;
    padding: 8px 14px;
    background: var(--bg-panel);
    border-bottom: 1px solid var(--border);
  }
  .settings {
    padding: 2px 8px;
    font-size: 12px;
  }
  .aslink {
    background: none;
    border: none;
    padding: 2px 0;
    cursor: pointer;
    text-decoration: underline;
    text-underline-offset: 3px;
  }
  .settingsdlg {
    background: var(--bg-panel);
    color: var(--text);
    border: 1px solid var(--border);
    border-radius: 8px;
    padding: 14px 16px;
    width: min(560px, 90vw);
  }
  /* 只在 open 慢套:display:flex 若無條件寫,會蓋掉 UA 的 dialog:not([open])
     { display:none } → dialog 載入即常駐顯示、close() 因無 open attr throw 關不掉 */
  .settingsdlg[open] {
    display: flex;
    flex-direction: column;
    gap: 12px;
    position: fixed; /* 釘死自適應視窗正中(UA 預設位置不可靠) */
    top: 50%;
    left: 50%;
    transform: translate(-50%, -50%);
    margin: 0;
  }
  .settingsdlg::backdrop {
    background: rgb(0 0 0 / 0.5);
  }
  .settingsdlg h2 {
    font-size: 13px;
    color: var(--text-dim);
    margin: 4px 0 2px;
  }
  .settingsdlg p {
    margin: 2px 0;
    font-size: 14px;
  }
  .tabs {
    display: flex;
    gap: 4px;
    border-bottom: 1px solid var(--border);
    margin: 0 -16px;
    padding: 0 16px;
  }
  .tabs button {
    background: none;
    border: none;
    border-bottom: 2px solid transparent;
    border-radius: 0;
    color: var(--text-dim);
    padding: 6px 10px;
  }
  .tabs button.on {
    background: none; /* 蓋掉全域 button.on 的橘底 */
    color: var(--text);
    border-bottom-color: var(--accent);
  }
  .formrow {
    display: flex;
    align-items: center;
    gap: 10px;
  }
  .formlabel {
    width: 52px;
    flex-shrink: 0;
    color: var(--text-dim);
    font-size: 13px;
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
    display: grid;
    grid-template-columns: 280px 1fr 300px;
    gap: 14px;
    flex: 1;
    min-height: 0;
  }
  .col {
    display: flex;
    flex-direction: column;
    gap: 8px;
    min-height: 0;
  }
  .colhead {
    display: flex;
    align-items: center;
    gap: 6px;
  }
  .coltitle {
    font-size: 13px;
    font-weight: 600;
    color: var(--text-dim);
    text-transform: uppercase;
    letter-spacing: 0.08em;
    margin-right: 4px;
  }
  .colhead .mini {
    padding: 2px 8px;
    font-size: 12px;
  }
  .colbody {
    flex: 1;
    min-height: 0;
    overflow-y: auto;
    display: flex;
    flex-direction: column;
    gap: 8px;
    padding-right: 2px;
  }
  .center {
    display: flex;
    align-items: flex-end;
    justify-content: center;
  }
  .hint {
    font-size: 11px;
  }
</style>
