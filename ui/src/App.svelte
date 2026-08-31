<script lang="ts">
  import { onMount } from "svelte";
  import { open, save } from "@tauri-apps/plugin-dialog";
  import { getCurrentWindow } from "@tauri-apps/api/window";
  import TrackStrip from "./lib/TrackStrip.svelte";
  import { mountDragGhost, removeDragGhost } from "./lib/ghost";
  import { isDirty, resolveDirtyChoice, type DirtyChoice } from "./lib/dirty";
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
    MissingPlugin,
    ScanFailure,
    ScanModule,
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
  let ensuredDefaults = false; // 首次連線確保有系統輸出;engine 端保保證唯一
  // ---- B:authoritative dirty(engine revision vs 上次存/載基準)----
  let revision = $state<number | null>(null); // engine 權威版號(status/snapshot/reply 帶回)
  let cleanRevision = $state<number | null>(null); // 上次成功存/載當下的 revision
  const dirty = $derived(revision !== null && cleanRevision !== null && revision !== cleanRevision);
  // ---- E:背景掃描 job(共用 registry,所有軌共用一份清單)----
  let scanModules = $state<ScanModule[]>([]);
  let scanFailed = $state<ScanFailure[]>([]);
  let scanJobId = $state<number | null>(null);
  let scanRunning = $state(false);
  let scanProgress = $state<{ done: number; total: number } | null>(null);
  let scanNotice = $state("");
  // ---- A:load_session 的 missing diagnostics 摘要(頂欄)----
  let missing = $state<MissingPlugin[]>([]);
  // ---- B:未儲存變更三分支 dialog ----
  let dirtyDlg = $state<HTMLDialogElement | null>(null);
  let dirtyResolve: ((c: DirtyChoice) => void) | null = null;
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
      if (typeof s.status.revision === "number") revision = s.status.revision;
      // 全域 plugin registry 重連也對齊(不用重新掃);掃描進行中不覆寫
      if (!scanRunning && Array.isArray(s.lastScan)) {
        scanModules = s.lastScan as ScanModule[];
      }
      ensureDefaults().catch(() => {}); // 首次連上空場景也要補系統輸出(snapshot 不走 status 事件)
    });
    await onEngineEvent((kind, payload) => {
      if (kind === "status") {
        status = payload as EngineStatus;
        const st = payload as EngineStatus;
        if (typeof st.revision === "number") revision = st.revision;
        // stream 狀態的權威對齊:engine 跑著時 UI 選擇跟著實際值(失敗回滾後也正確)
        if (st.running && st.deviceKey) selected = st.deviceKey;
        ensureDefaults().catch(() => {});
      }
      // 硬體面板關閉:driver 設定可能變(率),且 SSL 這類 driver 在面板動 buffer 後
      // 現有 stream 會死流 —— 一律重掃 + 重建(短暫中斷換取與硬體同步)
      if (kind === "devices_changed") onPanelClosed().catch(() => {});
      // ---- E:掃描 job events(jobId 不符 = 上一代的 late event,忽略)----
      if (kind === "scan_progress") {
        const p = payload as { jobId: number; done: number; total: number };
        if (p.jobId === scanJobId) scanProgress = { done: p.done, total: p.total };
      }
      if (kind === "scan_done") {
        const p = payload as { jobId: number; plugins: ScanModule[]; failed: ScanFailure[] };
        if (p.jobId === scanJobId) {
          scanModules = p.plugins ?? [];
          scanFailed = p.failed ?? [];
          scanRunning = false;
          scanProgress = null;
        }
      }
      if (kind === "scan_failed") {
        const p = payload as { jobId: number; error: string };
        if (p.jobId === scanJobId) {
          scanNotice = p.error;
          scanRunning = false;
          scanProgress = null;
        }
      }
      if (kind === "scan_cancelled") {
        const p = payload as { jobId: number };
        if (p.jobId === scanJobId) {
          scanRunning = false;
          scanProgress = null;
        }
      }
    });
    await onMeters((m) => (meters = m));
    // ---- B:關窗前 dirty 詢問(儲存/捨棄/取消;取消 = 真的不關)----
    await getCurrentWindow().onCloseRequested(async (e) => {
      if (!dirty) return; // clean:直接關
      e.preventDefault();
      const choice = await askDirty();
      const plan = resolveDirtyChoice(dirty, choice);
      if (plan.shouldSave) {
        const okSave = await saveSessionForClose();
        if (!okSave) return; // 存失敗 = 不退出(不得覆蓋失敗就關)
      }
      if (plan.proceed || plan.shouldSave) void getCurrentWindow().destroy();
    });
    settingsReady();
    refreshDevices().catch(() => {});
  });

  /** 開三分支 modal;使用者選完 resolve */
  function askDirty(): Promise<DirtyChoice> {
    return new Promise((res) => {
      dirtyResolve = res;
      dirtyDlg?.showModal();
    });
  }
  function answerDirty(c: DirtyChoice) {
    dirtyDlg?.close();
    dirtyResolve?.(c);
    dirtyResolve = null;
  }

  /** 關窗/載入前的存檔:有 lastSessionPath 直接覆寫,否則開存檔對話框。回傳是否成功 */
  async function saveSessionForClose(): Promise<boolean> {
    const target = appSettings?.lastSessionPath;
    let path = target ?? null;
    if (!path) {
      try {
        path = await save({
          title: "儲存 Session",
          defaultPath: "session.rmsession",
          filters: [{ name: "RoudaMix Session", extensions: ["rmsession"] }],
        });
      } catch {
        return false;
      }
      if (!path) return false;
    }
    try {
      const r = await engineCommand("save_session", {
        path,
        deviceKey: selected || null,
        sampleRate: status?.running ? Math.round(status.sampleRate) : null,
        bufferSize: status?.running ? status.bufferSize : bufSize,
      });
      if (typeof r.revision === "number") cleanRevision = r.revision;
      rememberLastSession(path);
      notice = "";
      restoreError = "";
      return true;
    } catch (e) {
      notice = String(e);
      return false;
    }
  }

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

  // 首次 snapshot 空 = 新場景:補系統輸出(monitor/stream;engine 端保證唯一性)
  async function ensureDefaults() {
    if (!status) return;
    await settingsReady();
    if (restorePath()) await startupRestore(); // 恢復完 snapshot 會帶 tracks,自然跳過空白預設
    if (ensuredDefaults || !status || status.tracks.length > 0) return;
    ensuredDefaults = true;
    try {
      const r = await engineCommand("ensure_system_outputs", {});
      // 新空白場景的系統輸出 = 基準狀態,不算使用者未存變更
      if (typeof r.revision === "number") cleanRevision = r.revision;
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
    if (status?.running) queueRestart();
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

  // ---- C:交易式裝置/Buffer 切換。onchange 立即切(無 Apply);busy 鎖住控制防
  // 連點競態;切換序列化(promise chain);新設定起不來 = 自動恢復最後可工作的
  // 裝置/Buffer;恢復也失敗 = engine 已 stopped,權威 status event 會把 UI 帶回現實 ----
  let lastGood = $state<{ key: string; buf: number | null } | null>(null); // 最後成功 start 的設定
  let switchChain: Promise<void> = Promise.resolve();

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
      lastGood = { key, buf: bufSize };
      audioStale = false;
    } catch (e) {
      audioStale = true;
      notice = String(e);
    }
    busy = false;
  }

  function queueRestart(key = selected) {
    // 連續選擇排隊依序跑;busy 鎖(select disabled)已擋大部分,這裡兜底序列化
    switchChain = switchChain.then(() => doRestart(key));
  }

  async function doRestart(key: string) {
    if (!key) return;
    busy = true;
    notice = "";
    const wantBuf = bufSize;
    try {
      await engineCommand("stop");
      await engineCommand("start", {
        deviceKey: key,
        sampleRate: null,
        bufferSize: wantBuf,
      });
      lastGood = { key, buf: wantBuf };
      audioStale = false;
    } catch (e) {
      // 新設定失敗:回滾到最後可工作設定(成功 = UI 回權威值 + 顯示原因)
      notice = String(e);
      if (lastGood && (lastGood.key !== key || lastGood.buf !== wantBuf)) {
        try {
          await engineCommand("stop");
          await engineCommand("start", {
            deviceKey: lastGood.key,
            sampleRate: null,
            bufferSize: lastGood.buf,
          });
          selected = lastGood.key; // UI 回到實際權威值
          bufSize = lastGood.buf;
          notice = `切換失敗,已恢復原裝置/Buffer — ${String(e)}`;
          audioStale = false;
        } catch (e2) {
          audioStale = true;
          notice = `切換與回滾都失敗,音訊已停止 — ${String(e2)}`;
        }
      } else {
        audioStale = true;
      }
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
      lastGood = null; // 使用者主動停:沒有「最後可工作」可回滾
    } catch (e) {
      notice = String(e);
    }
    busy = false;
  }

  // ---- E:背景掃描 job(共用 registry;回覆立即回 jobId,進度走 events)----

  async function startScan() {
    if (scanRunning) return;
    scanNotice = "";
    try {
      const r = await engineCommand("start_scan", {});
      scanJobId = r.jobId as number;
      if (!r.reused) {
        scanModules = [];
        scanFailed = [];
      }
      scanRunning = true;
      scanProgress = null;
    } catch (e) {
      scanNotice = String(e);
    }
  }

  async function cancelScan() {
    try {
      await engineCommand("cancel_scan", {});
    } catch (e) {
      scanNotice = String(e);
    }
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
      const r = await engineCommand("save_session", {
        path,
        deviceKey: selected || null,
        sampleRate: status?.running ? Math.round(status.sampleRate) : null,
        bufferSize: status?.running ? status.bufferSize : bufSize,
      });
      if (typeof r.revision === "number") cleanRevision = r.revision; // 存成功才清 dirty
      rememberLastSession(path);
      notice = "";
      restoreError = ""; // 手動救回 = 啟動失敗警示該滅
    } catch (e) {
      notice = String(e);
    }
  }

  // session 載入後套裝置 + 自動啟用(率跟 driver 現行值);跑著時 = 重建到 session 裝置
  function applyLoadedSession(r: Record<string, unknown>) {
    if (typeof r.revision === "number") cleanRevision = r.revision; // 載成功才清 dirty
    missing = (r.missing as MissingPlugin[]) ?? [];
    const dk = r.deviceKey as string | null;
    if (dk && devices.some((d) => d.deviceKey === dk)) {
      selected = dk;
      // session 的 buffer:合法值才套,否則 driver preferred
      const sb = r.bufferSize as number | null;
      const dev = devices.find((d) => d.deviceKey === dk);
      if (sb && dev?.bufferSizes?.includes(sb)) bufSize = sb;
      else if (dev) applyDeviceDefaults(dev);
      if (status?.running) queueRestart(dk);
      else start(dk);
    }
  }

  async function loadSession() {
    // dirty 先問(儲存/捨棄/取消):取消 = 真的不載;存失敗 = 不覆蓋現況
    if (dirty) {
      const choice = await askDirty();
      const plan = resolveDirtyChoice(dirty, choice);
      if (plan.shouldSave) {
        const okSave = await saveSessionForClose();
        if (!okSave) return;
      } else if (!plan.proceed) {
        return; // cancel
      }
    }
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
  {#if missing.length > 0}
    <button
      class="err aslink"
      onclick={() => (missing = [])}
      title={missing
        .map((m) => `${m.trackName}[${m.index}] ${m.name || m.pluginPath}:${m.message}`)
        .join("\n")}
      >⚠ {missing.length} 個 plugin 無法載入(已保留 placeholder)— 點此收起</button
    >
  {/if}
  {#if dirty}
    <span class="dim" title="有未儲存的變更">● 未儲存</span>
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
            scanModules={scanModules}
            scanFailed={scanFailed}
            scanRunning={scanRunning}
            scanProgress={scanProgress}
            scanNotice={scanNotice}
            onScan={startScan}
            onCancelScan={cancelScan}
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
            scanModules={scanModules}
            scanFailed={scanFailed}
            scanRunning={scanRunning}
            scanProgress={scanProgress}
            scanNotice={scanNotice}
            onScan={startScan}
            onCancelScan={cancelScan}
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
        disabled={devices.length === 0 || busy}
        onchange={(e) => {
          const d = devices.find((x) => x.deviceKey === e.currentTarget.value);
          if (d) {
            applyDeviceDefaults(d);
            queueRestart(d.deviceKey); // 即時切換:stop → start 新裝置(失敗自動回滾)
          }
        }}
      >
        {#each devices as d (d.deviceKey)}
          <option value={d.deviceKey}>{d.name} ({d.maxIn}in/{d.maxOut}out)</option>
        {:else}
          <option value="">(無 ASIO 裝置)</option>
        {/each}
      </select>
      {#if busy}
        <span class="dim">切換中…</span>
      {/if}
    </div>
    <div class="formrow">
      <label class="formlabel" for="setbuf">Buffer</label>
      <select
        id="setbuf"
        value={bufSize ?? ""}
        disabled={!selDev || selDev.bufferSizes.length === 0 || busy}
        onchange={(e) => {
          bufSize = Number(e.currentTarget.value);
          queueRestart();
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

<!-- B:未儲存變更三分支。取消 = 不關窗/不載入;儲存失敗 = 視同取消(不得覆蓋) -->
<dialog bind:this={dirtyDlg} class="dirtydlg">
  <p class="dirtyq">有未儲存的變更 — 要先儲存嗎?</p>
  <div class="dirtyrow">
    <button class="primary" onclick={() => answerDirty("save")}>儲存</button>
    <button class="danger" onclick={() => answerDirty("discard")}>捨棄變更</button>
    <button onclick={() => answerDirty("cancel")}>取消</button>
  </div>
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
  /* B:未儲存變更詢問(置中 modal,同 settingsdlg 手法) */
  .dirtydlg {
    background: var(--bg-panel);
    color: var(--text);
    border: 1px solid var(--border);
    border-radius: 8px;
    padding: 16px 18px;
    width: min(360px, 90vw);
  }
  .dirtydlg[open] {
    display: flex;
    flex-direction: column;
    gap: 14px;
    position: fixed;
    top: 50%;
    left: 50%;
    transform: translate(-50%, -50%);
    margin: 0;
  }
  .dirtydlg::backdrop {
    background: rgb(0 0 0 / 0.5);
  }
  .dirtyq {
    margin: 0;
    font-size: 14px;
  }
  .dirtyrow {
    display: flex;
    gap: 8px;
    justify-content: flex-end;
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
