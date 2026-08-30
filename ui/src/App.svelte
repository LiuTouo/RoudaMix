<script lang="ts">
  import { onMount } from "svelte";
  import { open, save } from "@tauri-apps/plugin-dialog";
  import TrackStrip from "./lib/TrackStrip.svelte";
  import { mountDragGhost, removeDragGhost } from "./lib/ghost";
  import {
    connectStatus,
    onConnection,
    onSnapshot,
    onEngineEvent,
    onMeters,
    engineCommand,
    getSettings,
    setSettings,
    listSessions,
    type AppSettings,
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
  let tab = $state<"audio" | "general" | "about">("audio");
  let appSettings = $state<AppSettings | null>(null);
  let folderFiles = $state<string[]>([]);
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
    // 原生網頁右鍵選單不要(之後換符合主題的自訂選單)
    window.addEventListener("contextmenu", (e) => e.preventDefault());
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
    settingsReady();
    refreshDevices().catch(() => {});
  });

  // ---------- 應用層設定 + 啟動場景恢復 ----------

  let settingsP: Promise<void> | null = null;
  function settingsReady() {
    // 快取單次讀取;ensureDefaults 前必 await,避免 settings 未到就先建空白場景
    settingsP ??= getSettings()
      .then((s) => {
        appSettings = s;
      })
      .catch(() => {}); // 讀不到 = 用預設(blank)
    return settingsP;
  }

  async function persistSettings() {
    if (!appSettings) return;
    try {
      await setSettings(appSettings);
    } catch {
      // 存失敗不擋 UI;下次啟動退回舊值
    }
  }

  function rememberLastSession(p: string) {
    if (appSettings) appSettings.lastSessionPath = p;
    setSettings({ lastSessionPath: p }).catch(() => {});
  }

  // 依啟動模式算出要恢復的 session 路徑(blank = null)
  function restorePath(): string | null {
    if (!appSettings) return null;
    if (appSettings.startupMode === "last") return appSettings.lastSessionPath;
    if (appSettings.startupMode === "folder" && appSettings.sessionDir && appSettings.startupFile)
      return `${appSettings.sessionDir}\\${appSettings.startupFile}`;
    return null;
  }

  let restoreP: Promise<void> | null = null;
  let restoreError = $state(""); // 啟動恢復失敗(檔案不存在/損壞)→ 頂列提示
  function startupRestore(): Promise<void> {
    // 共享同一個 in-flight promise:多個 status 事件同時觸發也只載一次
    restoreP ??= (async () => {
      const p = restorePath();
      if (!p) return;
      try {
        const r = await engineCommand("load_session", { path: p });
        applyLoadedSession(r);
      } catch (e) {
        // 檔案不存在/損壞 = 開空白 + 頂列提示,不擋啟動
        restoreError = String(e);
      }
    })();
    return restoreP;
  }

  // 首次 snapshot 空 = 新場景:自動建輸出軌「監聽/串流」(監聽 = ASIO 主輸出 pair 0)
  async function ensureDefaults() {
    if (!status) return;
    await settingsReady();
    if (restorePath()) await startupRestore(); // 恢復完 snapshot 會帶 tracks,自然跳過空白預設
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

  // ---------- 拖曳排序(HTML5 DnD;事件委派在 lane,跨群組不 preventDefault = 不可放) ----------

  type LaneGroup = "input" | "output";
  let drag = $state<{ id: number; group: LaneGroup } | null>(null);
  let dropAt = $state<{ group: LaneGroup; pos: number } | null>(null);
  // dragstart 的 target 會被重定向到 draggable 祖先(strip),推桿等控制項的真目標
  // 只能靠 pointerdown(capture)先記起來
  let pressEl: HTMLElement | null = null;

  function lanePos(lane: HTMLElement, x: number): number {
    // 第一個中心點在指標右側的 strip = 插入位;都沒有 = 尾端
    const kids = [...lane.querySelectorAll<HTMLElement>(".strip")];
    const hit = kids.findIndex((k) => k.getBoundingClientRect().left + k.offsetWidth / 2 > x);
    return hit === -1 ? kids.length : hit;
  }
  function laneArr(group: LaneGroup): Track[] {
    return group === "input" ? inputTracks : outputTracks;
  }
  function onDragStart(group: LaneGroup) {
    return (e: DragEvent) => {
      // 控制區(下拉/鈕/色盤/展開頭/推桿)不開拖曳;其餘整條 strip 都能拖
      if (pressEl?.closest?.("input, select, button, summary")) {
        e.preventDefault();
        return;
      }
      const el = (e.target as HTMLElement).closest?.("[data-track-id]") as HTMLElement | null;
      if (!el) return;
      drag = { id: Number(el.dataset.trackId), group };
      e.dataTransfer?.setData("text/plain", el.dataset.trackId ?? "");
      if (e.dataTransfer) {
        e.dataTransfer.effectAllowed = "move";
        mountDragGhost(e.dataTransfer, el, el.offsetWidth || 250);
      }
    };
  }
  function onDragOver(group: LaneGroup) {
    return (e: DragEvent) => {
      if (!drag || drag.group !== group || !e.dataTransfer) return;
      e.preventDefault();
      e.dataTransfer.dropEffect = "move";
      dropAt = { group, pos: lanePos(e.currentTarget as HTMLElement, e.clientX) };
    };
  }
  function onDrop(group: LaneGroup) {
    return (e: DragEvent) => {
      if (!drag || drag.group !== group) return;
      e.preventDefault();
      const arr = laneArr(group);
      const dragIdx = arr.findIndex((t) => t.trackId === drag!.id);
      if (dragIdx >= 0) {
        const pos = lanePos(e.currentTarget as HTMLElement, e.clientX);
        // lane 位置 → master 絕對索引(track_move = erase+insert;先移除造成左移要補回)
        let target =
          pos >= arr.length
            ? tracks.findIndex((t) => t.trackId === arr[arr.length - 1].trackId) + 1
            : tracks.findIndex((t) => t.trackId === arr[pos].trackId);
        if (pos > dragIdx) target -= 1;
        target = Math.max(0, Math.min(target, tracks.length - 1));
        engineCommand("track_move", { trackId: drag.id, newIndex: target }).catch(() => {});
      }
      drag = null;
      dropAt = null;
    };
  }
  function onDragEnd() {
    removeDragGhost();
    drag = null;
    dropAt = null; // Esc / 放到帶外也收尾
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
      rememberLastSession(path);
      notice = "";
      restoreError = ""; // 手動救回 = 啟動失敗警示該滅
    } catch (e) {
      notice = String(e);
    }
  }

  // session 載入後套裝置 + 自動啟用(率跟 driver 現行值);跑著時 = 重建到 session 裝置
  function applyLoadedSession(r: Record<string, unknown>) {
    const dk = r.deviceKey as string | null;
    if (dk && devices.some((d) => d.deviceKey === dk)) {
      selected = dk;
      // session 的 buffer:合法值才套,否則 driver preferred
      const sb = r.bufferSize as number | null;
      const dev = devices.find((d) => d.deviceKey === dk);
      if (sb && dev?.bufferSizes?.includes(sb)) bufSize = sb;
      else if (dev) applyDeviceDefaults(dev);
      if (status?.running) restartWith(dk);
      else start(dk);
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
      applyLoadedSession(r);
      rememberLastSession(path);
      notice = "";
      restoreError = "";
    } catch (e) {
      notice = String(e);
    }
  }

  // ---------- 通用設定 ----------

  async function refreshFolderFiles() {
    if (!appSettings?.sessionDir) {
      folderFiles = [];
      return;
    }
    folderFiles = await listSessions(appSettings.sessionDir).catch(() => []);
  }

  function openGeneral() {
    tab = "general";
    refreshFolderFiles().catch(() => {});
  }

  async function pickSessionDir() {
    try {
      const d = await open({
        title: "選擇 Session 資料夾",
        directory: true,
        multiple: false,
      });
      if (!d) return;
      if (!appSettings)
        appSettings = {
          startupMode: "blank",
          sessionDir: null,
          startupFile: null,
          lastSessionPath: null,
        };
      appSettings.sessionDir = d as string;
      appSettings.startupFile = null; // 換資料夾 = 舊選擇作廢
      await persistSettings();
      await refreshFolderFiles();
    } catch (e) {
      notice = String(e);
    }
  }

  const running = $derived(status?.running ?? false);
  const selDev = $derived(devices.find((d) => d.deviceKey === selected) ?? null);
  // ASIO getLatencies 單位 = samples;換算 ms 顯示(去尾零;driver 沒報 = —)
  function samplesToMs(n: number | null, rate: number): string {
    return n == null || rate <= 0 ? "—" : String(parseFloat(((n / rate) * 1000).toFixed(2)));
  }
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
  {#if restoreError}
    <button class="err aslink" onclick={() => (settingsOpen = true)} title={restoreError}
      >Session 恢復失敗,已開空白 — 詳情見設定</button
    >
  {/if}
  <span style="flex:1"></span>
  {#if running}
    <span class="dot ok"></span>
    <span class="mono"
      >{status!.sampleRate} Hz · buf {status!.bufferSize} · lat input/output
      {samplesToMs(status!.inputLatency, status!.sampleRate)}ms/{samplesToMs(
        status!.outputLatency,
        status!.sampleRate,
      )}ms · xrun {status!.xruns}</span
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
  <!-- 單一水平帶:輸入群組(左)→ 輸出群組(右),都往右長;超出寬度橫向卷動(shift+滾輪原生) -->
  <section class="board">
    <!-- 輸入群組:audio / app / fx -->
    <section class="group">
      <div class="colhead">
        <span class="coltitle">輸入</span>
        <button class="mini" onclick={() => addTrack("audio")}>＋ Audio</button>
        <button class="mini" onclick={() => addTrack("app")}>＋ App</button>
        <button class="mini" onclick={() => addTrack("fx")}>＋ FX</button>
      </div>
      <div
        class="lanes"
        onpointerdowncapture={(e) => (pressEl = e.target as HTMLElement)}
        ondragstart={onDragStart("input")}
        ondragover={onDragOver("input")}
        ondrop={onDrop("input")}
        ondragend={onDragEnd}
      >
        {#each inputTracks as t, i (t.trackId)}
          <TrackStrip
            track={t}
            {tracks}
            {devices}
            selectedDeviceKey={selected}
            strips={meters?.strips}
            dropBefore={dropAt?.group === "input" && dropAt.pos === i}
            dropAfter={dropAt?.group === "input" && dropAt.pos === i + 1}
            dragging={drag?.id === t.trackId}
          />
        {:else}
          <p class="dim hint">用上方按鈕新增 Audio / App / FX 軌</p>
        {/each}
      </div>
    </section>

    <!-- 輸出群組 -->
    <section class="group">
      <div class="colhead">
        <span class="coltitle">輸出</span>
        <button class="mini" onclick={() => addTrack("output")}>＋ 輸出軌</button>
      </div>
      <div
        class="lanes"
        onpointerdowncapture={(e) => (pressEl = e.target as HTMLElement)}
        ondragstart={onDragStart("output")}
        ondragover={onDragOver("output")}
        ondrop={onDrop("output")}
        ondragend={onDragEnd}
      >
        {#each outputTracks as t, i (t.trackId)}
          <TrackStrip
            track={t}
            {tracks}
            {devices}
            selectedDeviceKey={selected}
            strips={meters?.strips}
            dropBefore={dropAt?.group === "output" && dropAt.pos === i}
            dropAfter={dropAt?.group === "output" && dropAt.pos === i + 1}
            dragging={drag?.id === t.trackId}
          />
        {:else}
          <p class="dim hint">新增輸出軌(監聽 / 串流)</p>
        {/each}
      </div>
    </section>
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
    <button class:on={tab === "general"} onclick={openGeneral}>通用</button>
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
  {:else if tab === "general"}
    <h2>場景</h2>
    <div class="formrow">
      <label class="formlabel" for="startupmode">啟動時</label>
      <select
        id="startupmode"
        value={appSettings?.startupMode ?? "blank"}
        disabled={!appSettings}
        onchange={(e) => {
          if (!appSettings) return;
          appSettings.startupMode = e.currentTarget.value as AppSettings["startupMode"];
          persistSettings();
        }}
      >
        <option value="blank">建立空白 session</option>
        <option value="last">恢復上一次 session</option>
        <option value="folder">開啟指定資料夾內的 session 檔案</option>
      </select>
    </div>
    <div class="formrow">
      <label class="formlabel" for="sessiondir">Session 資料夾</label>
      <span class="dim mono dirpath" title={appSettings?.sessionDir ?? "未設定"}>
        {appSettings?.sessionDir ?? "(未設定)"}
      </span>
      <button onclick={pickSessionDir}>選擇…</button>
    </div>
    {#if restoreError}
      <p class="err mono">啟動恢復失敗:{restoreError}</p>
    {/if}
    {#if appSettings?.startupMode === "last"}
      <div class="formrow">
        <span class="dim mono dirpath" title={appSettings.lastSessionPath ?? ""}>
          上一次:{appSettings.lastSessionPath ?? "(尚未存過 session)"}
        </span>
      </div>
    {/if}
    {#if appSettings?.startupMode === "folder"}
      <div class="formrow">
        <label class="formlabel" for="startupfile">Session 檔案</label>
        <select
          id="startupfile"
          value={appSettings.startupFile ?? ""}
          disabled={!appSettings.sessionDir}
          onchange={(e) => {
            appSettings!.startupFile = e.currentTarget.value || null;
            persistSettings();
          }}
        >
          <option value="">建立空白 session</option>
          {#each folderFiles as f (f)}
            <option value={f}>{f}</option>
          {:else}
            {#if appSettings.sessionDir}
              <option value="">(資料夾內無 session 檔)</option>
            {/if}
          {/each}
        </select>
      </div>
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
    width: 96px;
    flex-shrink: 0;
    color: var(--text-dim);
    font-size: 13px;
  }
  .dirpath {
    flex: 1;
    min-width: 0;
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
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
    flex-direction: column;
    gap: 10px;
    flex: 1;
    min-height: 0;
    min-width: 0;
  }
  /* 單一水平帶:輸入群組(左)+ 輸出群組(右);超出寬 = 橫向卷動(shift+滾輪原生) */
  .board {
    flex: 1;
    min-height: 0;
    display: flex;
    gap: 20px;
    overflow-x: auto;
    overflow-y: hidden;
    padding-bottom: 4px;
  }
  .group {
    flex: 0 0 auto;
    display: flex;
    flex-direction: column;
    gap: 8px;
    min-height: 0;
  }
  .group + .group {
    border-left: 1px solid var(--border);
    padding-left: 20px;
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
  .lanes {
    flex: 1;
    min-height: 0;
    display: flex;
    gap: 8px;
  }
  .hint {
    font-size: 11px;
  }
</style>
