<script lang="ts">
  import { onMount } from "svelte";
  import { open, save } from "@tauri-apps/plugin-dialog";
  import {
    disable as disableAutostart,
    enable as enableAutostart,
    isEnabled as isAutostartEnabled,
  } from "@tauri-apps/plugin-autostart";
  import { getCurrentWindow } from "@tauri-apps/api/window";
  import TrackStrip from "./lib/TrackStrip.svelte";
  import LatencyDrawer from "./lib/LatencyDrawer.svelte";
  import ContextMenu from "./lib/ContextMenu.svelte";
  import { mountDragGhost, removeDragGhost } from "./lib/ghost";
  import {
    initialRevisionDirty,
    isRevisionDirty,
    resolveDirtyChoice,
    transitionRevisionDirty,
    type DirtyChoice,
    type RevisionDirtyState,
  } from "./lib/revisionDirty";
  import { applyStatus, type AuthoritativeStatusPayload } from "./lib/applyStatus";
  import {
    initialDeviceStream,
    isDeviceStreamBusy,
    sameDeviceStreamConfig,
    transitionDeviceStream,
    type DeviceStreamConfig,
    type DeviceStreamRequest,
  } from "./lib/deviceStream";
  import { MutationQueue, mutKey, type MutationRunResult } from "./lib/mutations";
  import { connView, epochChanged } from "./lib/connPhase";
  import { errorText, friendlyError, OverloadDetector } from "./lib/errors";
  import { visibleRange, spacerWidths, dropPosFromX } from "./lib/laneView";
  import { reorderLane } from "./lib/laneOrder";
  import {
    initialScanJob,
    isScanJobRunning,
    scanCompletionNotice,
    transitionScanJob,
    type ScanJobState,
  } from "./lib/scanJob";
  import {
    connectStatus,
    onConnection,
    onSnapshot,
    onEngineEvent,
    onMeters,
    onTelemetryAbiMismatch,
    getSettings,
    setSettings,
    listSessions,
    onTrayExitRequested,
    onSingleInstance,
    quitApp,
    respawnEngine,
    type AppSettings,
    type SettingsReply,
  } from "./lib/ipc";
  import { engineCommand, toCommandError, type CommandError } from "./lib/protocol-commands.generated";
  import { samplesToMs } from "./lib/format";
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
  let connProbeErr = $state<CommandError | null>(null); // 主動 get_snapshot 的錯誤(version mismatch 等)
  let snap = $state<unknown>(null);
  let status = $state<EngineStatus | null>(null);
  let latencyEnabled = $state(false);
  let latencyDrawerOpen = $state(false);
  let meters = $state<MetersFrame | null>(null);
  let devices = $state<DeviceInfo[]>([]);
  let selected = $state("");
  let bufSize = $state<number | null>(null); // buffer 是 ASIO host 權威;實數,初始 = driver preferred
  let deviceStream = $state(initialDeviceStream());
  const busy = $derived(isDeviceStreamBusy(deviceStream));
  let notice = $state("");
  let panelOpen = $state(false); // 硬體面板開啟中:stream 可能停,關閉後自動重建
  let settingsOpen = $state(false); // 設定 modal;已開再按 = 無操作,天然單例
  let settingsDlg = $state<HTMLDialogElement | null>(null);
  let tab = $state<"audio" | "general" | "about">("audio");
  let appSettings = $state<AppSettings | null>(null);
  let autostartEnabled = $state<boolean | null>(null);
  let autostartBusy = $state(false);
  let startMinimizedBusy = $state(false);
  let folderFiles = $state<string[]>([]);
  const audioStale = $derived(deviceStream.stale); // 應該在跑但沒跑(啟動失敗)→ 頂欄極簡警示
  let ensuredDefaults = false; // 首次連線確保有系統輸出;engine 端保保證唯一
  const mutations = new MutationQueue((key, error, tag) => {
    if (key === mutKey.device && tag !== "auto-start") showNotice(error);
  });
  // ---- O:頂層通知中心(錯誤/狀態帶動作;per-track 錯誤留在 TrackStrip)----
  interface Notice {
    id: number;
    kind: "error" | "info";
    msg: string;
    raw?: string; // 技術細節(可複製)
    dismissible: boolean;
    autoDismissMs?: number;
    onDismiss?: () => void;
  }
  let notices = $state<Notice[]>([]);
  let nextNoticeId = 1;
  const latencyRuntimeStates = new Map<string, string>();
  function addNotice(
    kind: Notice["kind"],
    msg: string,
    raw?: string,
    autoDismissMs = 0,
    dismissible = autoDismissMs <= 0,
    onDismiss?: () => void,
  ): void {
    const id = nextNoticeId++;
    notices = [
      ...notices.slice(-4),
      { id, kind, msg, raw, dismissible, autoDismissMs, onDismiss },
    ];
    if (autoDismissMs > 0) setTimeout(() => dismissNotice(id), autoDismissMs);
  }
  function dismissNotice(id: number): void {
    const dismissed = notices.find((n) => n.id === id);
    notices = notices.filter((n) => n.id !== id);
    dismissed?.onDismiss?.();
  }
  function observeLatencyRuntime(tracks: Track[]): void {
    revisionDirty = transitionRevisionDirty(revisionDirty, { type: "runtimeObserved" });
    if (!latencyEnabled) return;
    for (const track of tracks) {
      for (const plugin of track.plugins) {
        for (const [variant, state] of [
          ["primary", plugin.runtimeState ?? "active"],
          ["monitor", plugin.monitorState ?? "active"],
        ] as const) {
          const key = `${plugin.instanceId}:${variant}`;
          const previous = latencyRuntimeStates.get(key);
          latencyRuntimeStates.set(key, state);
          if (state === previous) continue;
          const role = variant === "monitor" ? "Monitor Shadow" : "Primary";
          if (state === "suspended" || state === "degraded")
            addNotice(
              "error",
              `${plugin.name} 的 ${role} 已${state === "suspended" ? "暫停" : "降級"}，受影響路徑改送 dry audio。`,
            );
          else if (state === "active" && previous && previous !== "active")
            addNotice("info", `${plugin.name} 的 ${role} 已恢復。`, undefined, 3000);
        }
      }
    }
  }
  /** P1-O:可複製診斷(WebView2 secure context 下 clipboard 可用;失敗 = 提示) */
  async function copyText(t: string): Promise<boolean> {
    try {
      if (!navigator.clipboard) return false;
      await navigator.clipboard.writeText(t);
      return true;
    } catch {
      return false;
    }
  }
  function copyNotice(n: Notice): void {
    void copyText(`${n.msg}\n${n.raw ?? ""}`).then((ok) =>
      addNotice("info", ok ? "已複製到剪貼簿" : "複製失敗(剪貼簿不可用)"),
    );
  }
  // ---- J:callback load 持續過載警示(單次尖峰不洗版)----
  const overload = new OverloadDetector(1.0, 6, 12); // ~0.13s 連續超載起算、~0.27s 正常解除
  let overloadOn = $state(false);
  function feedLoad(): void {
    const v = meters?.callbackLoad ?? 0;
    if (overload.sample(v)) {
      overloadOn = overload.isOverloaded;
      if (overloadOn)
        addNotice(
          "error",
          "音訊負載持續過載 —— 請增大 Buffer 或減少 plugin",
          `callbackLoad = ${Math.round(v * 100)}%`,
        );
    }
  }
  // ---- M:右鍵選單(全域一份;TrackStrip 發起)----
  let menu = $state<{
    x: number;
    y: number;
    label: string;
    items: Array<{ label: string; disabled?: boolean; run: () => void }>;
  } | null>(null);
  function openMenu(
    x: number,
    y: number,
    label: string,
    items: Array<{ label: string; disabled?: boolean; run: () => void }>,
  ): void {
    menu = { x, y, label, items };
  }
  // ---- B:authoritative dirty(engine revision vs 上次存/載基準)----
  let revisionDirty = $state<RevisionDirtyState>(initialRevisionDirty());
  let currentSessionPath: string | null = null; // 本次實際載入/儲存的檔案；不可用「最近 Session」替代
  const dirty = $derived(isRevisionDirty(revisionDirty));
  function confirmRevisionBaseline(value: unknown): void {
    if (typeof value !== "number") return;
    revisionDirty = transitionRevisionDirty(revisionDirty, {
      type: "baselineConfirmed",
      revision: value,
    });
  }
  function rejectRevisionBaseline(): void {
    revisionDirty = transitionRevisionDirty(revisionDirty, { type: "baselineRejected" });
  }
  // ---- E:背景掃描 job(共用 registry,所有軌共用一份清單)----
  let scanModules = $state<ScanModule[]>([]);
  let scanFailed = $state<ScanFailure[]>([]);
  let scanJob = $state<ScanJobState<ScanModule, ScanFailure>>(initialScanJob());
  const scanRunning = $derived(isScanJobRunning(scanJob));
  const scanProgress = $derived(scanJob.progress);
  const scanNotice = $derived(scanJob.error);
  /** 快照事件、status 事件與主動 get_snapshot 共用的唯一權威縮減入口。 */
  function applyAuthoritativeStatus(payload: AuthoritativeStatusPayload): void {
    const applied = applyStatus(
      {
        status,
        latencyEnabled,
        scanModules,
        revisionDirty,
        deviceStream,
        selection: selected ? { deviceKey: selected, bufferSize: bufSize } : null,
      },
      payload,
      { scanRunning, devicesLoaded: devices.length > 0 },
    );

    status = applied.state.status;
    latencyEnabled = applied.state.latencyEnabled;
    scanModules = applied.state.scanModules;
    revisionDirty = applied.state.revisionDirty;
    deviceStream = applied.state.deviceStream;
    if (applied.state.selection) {
      selected = applied.state.selection.deviceKey;
      bufSize = applied.state.selection.bufferSize;
    }
    observeLatencyRuntime(applied.effects.latencyTracks);
    if (applied.effects.refreshDevices) void refreshDevices();
    if (applied.effects.ensureDefaults) void ensureDefaults();
  }
  // ---- A:load_session 的 missing diagnostics 摘要(頂欄)----
  let missing = $state<MissingPlugin[]>([]);
  // ---- B:未儲存變更三分支 dialog ----
  let dirtyDlg = $state<HTMLDialogElement | null>(null);
  let dirtyResolve: ((c: DirtyChoice) => void) | null = null;
  type CloseBehavior = NonNullable<AppSettings["closeBehavior"]>;
  let closeBehaviorDlg = $state<HTMLDialogElement | null>(null);
  let closeBehaviorResolve: ((choice: CloseBehavior | null) => void) | null = null;
  let closeFlowActive = false;
  let exitFlowActive = false;
  $effect(() => {
    if (settingsOpen) settingsDlg?.showModal();
    // open 才 close:dialog.close() 對未 open 的 dialog throw InvalidStateError
    else if (settingsDlg?.open) settingsDlg.close();
  });

  const tracks = $derived<Track[]>(status?.tracks ?? []);
  const telemetryStrips = $derived(status?.telemetryStrips ?? []);
  const meterView = $derived({ table: telemetryStrips, strips: meters?.strips });
  const inputTracks = $derived(tracks.filter((t) => t.kind !== "output"));
  const outputTracks = $derived(tracks.filter((t) => t.kind === "output"));

  // ---- A:啟動/重連順序 = 先註冊全部 listener → 讀 connection → 主動權威同步。
  // 反過來(先 connectStatus 再註冊)會漏接連線/snapshot 事件:bridge 在 spawn 後
  // 立即推 snapshot,註冊前的推送就是丟失。teardown 解除全部(HMR/重掛不重複事件)。
  const unsubs: Array<() => void> = [];
  let disposed = false;
  /** 註冊 listener;teardown 已跑 = 立即退訂(不殘留) */
  function sub(u: () => void): void {
    if (disposed) u();
    else unsubs.push(u);
  }
  onMount(() => {
    // P2-M/P1-O 折衷:原生網頁右鍵選單全面不出現(去網頁感 —— 軌/插件右鍵走
    // 自訂主題選單);但「需要複製的文字區」與輸入框放行原生選單(右鍵 Copy /
    // 貼上),user-select 已開放、Ctrl+C 亦通 —— 複製能力不受影響
    const onCtx = (e: MouseEvent) => {
      const t = e.target as HTMLElement;
      if (t.closest?.(".err, .notice-msg, .apppath, .dirpath, input, textarea")) return;
      e.preventDefault();
    };
    window.addEventListener("contextmenu", onCtx);
    unsubs.push(() => window.removeEventListener("contextmenu", onCtx));

    // 主介面全域儲存快捷鍵；攔下 WebView 的預設「儲存網頁」行為。
    const onKeyDown = (e: KeyboardEvent) => {
      if (
        e.defaultPrevented ||
        e.repeat ||
        !e.ctrlKey ||
        e.altKey ||
        e.metaKey ||
        e.shiftKey ||
        e.key.toLowerCase() !== "s"
      )
        return;
      e.preventDefault();
      void saveSessionForClose();
    };
    window.addEventListener("keydown", onKeyDown);
    unsubs.push(() => window.removeEventListener("keydown", onKeyDown));

    void (async () => {

      sub(await onTrayExitRequested(() => void requestAppExit()));
      sub(
        await onSingleInstance(() =>
          addNotice("info", "RoudaMix 已經在執行，已切換到目前的主視窗"),
        ),
      );

      sub(
      await onConnection((c) => {
        // P1-A:reconnect epoch 對齊 —— engine 換代重連,本地一次性旗標作廢重跑
        if (epochChanged(conn.epoch, c.epoch)) {
          ensuredDefaults = false;
          deviceStream = transitionDeviceStream(deviceStream, { type: "reset" }).state;
          devices = [];
          selected = "";
          bufSize = null;
          restoreError = "";
          currentSessionPath = null; // 新 engine 尚未成功恢復任何檔案，不得覆寫上一代 Session
          scanJob = transitionScanJob(scanJob, { type: "reset" }).state;
        }
        conn = c;
        if (c.connected && devices.length === 0) void refreshDevices();
      }),
    );
      sub(
      await onSnapshot((s) => {
        snap = s;
        applyAuthoritativeStatus(s);
      }),
    );
      sub(
      await onEngineEvent((kind, payload) => {
        if (kind === "status") {
          applyAuthoritativeStatus({ status: payload as EngineStatus });
        }
        // 硬體面板關閉:driver 設定可能變(率),且 SSL 這類 driver 在面板動 buffer 後
        // 現有 stream 會死流 —— 一律重掃 + 重建(短暫中斷換取與硬體同步)
        if (kind === "devices_changed") void onPanelClosed();
        // ---- E:掃描 job events(jobId 不符 = 上一代的 late event,忽略)----
        if (kind === "scan_progress") {
          const p = payload as { jobId: number; done: number; total: number };
          scanJob = transitionScanJob(scanJob, {
            type: "progress",
            jobId: p.jobId,
            done: p.done,
            total: p.total,
          }).state;
        }
        if (kind === "scan_done") {
          const p = payload as { jobId: number; plugins: ScanModule[]; failed: ScanFailure[] };
          const transition = transitionScanJob(scanJob, {
            type: "completed",
            jobId: p.jobId,
            modules: p.plugins ?? [],
            failures: p.failed ?? [],
          });
          scanJob = transition.state;
          if (transition.accepted && transition.state.result?.outcome === "success") {
            scanModules = transition.state.result.modules;
            scanFailed = transition.state.result.failures;
            const completionNotice = scanCompletionNotice(
              scanModules.length,
              scanFailed.length,
            );
            addNotice(
              "info",
              completionNotice.message,
              undefined,
              completionNotice.autoDismissMs,
              completionNotice.dismissible,
              () => {
                scanJob = transitionScanJob(scanJob, {
                  type: "dismiss",
                  jobId: p.jobId,
                }).state;
              },
            );
          }
        }
        if (kind === "scan_failed") {
          const p = payload as { jobId: number; error: string };
          const transition = transitionScanJob(scanJob, {
            type: "failed",
            jobId: p.jobId,
            error: p.error,
          });
          scanJob = transition.state;
          if (transition.accepted) {
            addNotice("error", "VST 掃描失敗，已保留原清單", p.error, 0, true, () => {
              scanJob = transitionScanJob(scanJob, {
                type: "dismiss",
                jobId: p.jobId,
              }).state;
            });
          }
        }
        if (kind === "scan_cancelled") {
          const p = payload as { jobId: number };
          const transition = transitionScanJob(scanJob, {
            type: "cancelled",
            jobId: p.jobId,
          });
          scanJob = transition.state;
          if (transition.accepted) {
            addNotice("info", "VST 掃描已取消，已保留原清單", undefined, 0, true, () => {
              scanJob = transitionScanJob(scanJob, {
                type: "dismiss",
                jobId: p.jobId,
              }).state;
            });
          }
        }
      }),
    );
      sub(
      await onMeters((m) => {
        meters = m;
        feedLoad(); // J:負載警示 debounce(持續過載才通知)
      }),
    );
      sub(
      await onTelemetryAbiMismatch(({ expected, found }) => {
        addNotice(
          "error",
          "Plugin Process Load 暫時不可用；Plugin Latency 與 PDC 仍正常。",
          `Telemetry ABI 不相容：UI 需要 v${expected}，engine 提供 v${found}`,
        );
      }),
    );
    // 主視窗關閉一律攔下：首次詢問關閉行為；縮至系統匣不觸發 dirty 詢問，
    // 真正退出才沿用未儲存 Session 保護。
      sub(
      await getCurrentWindow().onCloseRequested((e) => {
        e.preventDefault();
        void requestWindowClose();
      }),
    );

    // listeners 全掛好 → 讀現況(漏接的 snapshot 事件靠下一步主動拉補)
    conn = await connectStatus().catch(() => conn);
    // 主動權威同步:連線已久/事件早發過的場合,snapshot 事件不會再來
    try {
      const r = await engineCommand("get_snapshot", {});
      const s = r.snapshot as { status: EngineStatus; lastScan: ScanModule[] | null; capabilities?: string[] };
      applyAuthoritativeStatus(s);
      connProbeErr = null;
      // 每次程式啟動主動跑一次；持久 fingerprint 讓未變更 VST 不重新載入。
      await startScan();
    } catch (e) {
      connProbeErr = toCommandError(e); // version mismatch 等分類顯示(connView)
    }
      settingsReady();
    })();

    // teardown:解除所有 Tauri listener(HMR/重掛不重複事件)
    return () => {
      disposed = true;
      for (const u of unsubs) u();
    };
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

  function askCloseBehavior(): Promise<CloseBehavior | null> {
    return new Promise((resolve) => {
      closeBehaviorResolve = resolve;
      closeBehaviorDlg?.showModal();
    });
  }

  function answerCloseBehavior(choice: CloseBehavior | null) {
    closeBehaviorDlg?.close();
    closeBehaviorResolve?.(choice);
    closeBehaviorResolve = null;
  }

  function acceptSettingsReply(patch: Partial<AppSettings>, reply: SettingsReply): void {
    if (!appSettings) {
      appSettings = reply.settings;
    } else {
      const current = appSettings as unknown as Record<string, unknown>;
      const normalized = reply.settings as unknown as Record<string, unknown>;
      const next = { ...appSettings } as unknown as Record<string, unknown>;
      for (const key of Object.keys(patch)) {
        if (Object.is(current[key], (patch as Record<string, unknown>)[key]))
          next[key] = normalized[key];
      }
      appSettings = next as unknown as AppSettings;
    }
    if (reply.warnings.length > 0)
      addNotice("error", "設定有部分值不合法,已回復預設", reply.warnings.join("\n"));
  }

  type SettingsMutationTag =
    | "close-behavior"
    | "preferences"
    | "last-session"
    | "last-working"
    | "start-minimized";
  function writeSettings(
    tag: SettingsMutationTag,
    patch: Partial<AppSettings>,
  ): Promise<MutationRunResult<SettingsReply>> {
    return mutations.run(mutKey.settings, tag, () => setSettings(patch));
  }

  async function rememberCloseBehavior(choice: CloseBehavior): Promise<void> {
    const previous = appSettings?.closeBehavior ?? null;
    if (appSettings) appSettings.closeBehavior = choice;
    const patch = { closeBehavior: choice };
    const result = await writeSettings("close-behavior", patch);
    if (result.status === "completed") acceptSettingsReply(patch, result.value);
    else if (result.status === "failed") {
      if (appSettings?.closeBehavior === choice) appSettings.closeBehavior = previous;
      addNotice(
        "error",
        "關閉視窗偏好儲存失敗，下次仍會再次詢問",
        errorText(result.error),
      );
    }
  }

  async function requestAppExit(): Promise<void> {
    if (exitFlowActive) return;
    exitFlowActive = true;
    try {
      if (dirty) {
        const choice = await askDirty();
        const plan = resolveDirtyChoice(dirty, choice);
        if (plan.shouldSave) {
          const okSave = await saveSessionForClose();
          if (!okSave) return;
        }
        if (!plan.proceed && !plan.shouldSave) return;
      }
      await quitApp();
    } catch (e) {
      addNotice("error", "程式結束失敗", errorText(e));
    } finally {
      exitFlowActive = false;
    }
  }

  async function requestWindowClose(): Promise<void> {
    if (closeFlowActive || exitFlowActive) return;
    closeFlowActive = true;
    try {
      await settingsReady();
      let behavior = appSettings?.closeBehavior ?? null;
      if (!behavior) {
        behavior = await askCloseBehavior();
        if (!behavior) return;
        await rememberCloseBehavior(behavior);
      }
      if (behavior === "tray") await getCurrentWindow().hide();
      else await requestAppExit();
    } catch (e) {
      addNotice("error", "關閉視窗動作失敗", errorText(e));
    } finally {
      closeFlowActive = false;
    }
  }

  /** 儲存目前 Session：已有目前檔案就覆寫，否則開存檔對話框。回傳是否成功 */
  async function saveSessionForClose(): Promise<boolean> {
    let path = currentSessionPath;
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
      confirmRevisionBaseline(r.revision);
      currentSessionPath = path;
      rememberLastSession(path);
      notice = "";
      restoreError = "";
      return true;
    } catch (e) {
      rejectRevisionBaseline();
      // P1-F:存檔失敗 dirty 不清(cleanRevision 沒動)、原檔仍在(原子寫入)
      showNotice(e);
      addNotice("error", "Session 儲存失敗 —— 未儲存的變更仍在", errorText(e));
      return false;
    }
  }

  // ---------- 應用層設定 + 啟動場景恢復 ----------

  let settingsP: Promise<void> | null = null;
  function settingsReady() {
    // 快取單次讀取;ensureDefaults 前必 await,避免 settings 未到就先建空白場景
    settingsP ??= getSettings()
      .then((r) => {
        appSettings = r.settings;
        // P1-E:normalize 警告(known field 壞值已回預設)—— 頂欄通知呈現
        if (r.warnings.length > 0)
          addNotice("error", "設定檔有問題,部分值已回復預設", r.warnings.join("\n"));
      })
      .catch(() => {}); // 讀不到 = 用預設(blank)
    return settingsP;
  }

  async function persistSettings() {
    if (!appSettings) return;
    const patch = { ...appSettings };
    const result = await writeSettings("preferences", patch);
    if (result.status === "completed") acceptSettingsReply(patch, result.value);
    else if (result.status === "failed")
      // 存失敗不擋 UI;下次啟動退回舊值(原子寫入:舊檔完整保留)
      addNotice("error", "設定儲存失敗(下次啟動沿用舊值)", errorText(result.error));
  }

  function rememberLastSession(p: string) {
    if (appSettings) appSettings.lastSessionPath = p;
    const patch = { lastSessionPath: p };
    void writeSettings("last-session", patch).then((result) => {
      if (result.status === "completed") acceptSettingsReply(patch, result.value);
    });
  }

  // 依啟動模式算出要恢復的 session 路徑(blank = null)
  function restorePath(): string | null {
    if (!appSettings) return null;
    if (appSettings.startupMode === "last") return appSettings.lastSessionPath;
    if (appSettings.startupMode === "folder" && appSettings.sessionDir && appSettings.startupFile)
      return `${appSettings.sessionDir}\\${appSettings.startupFile}`;
    return null;
  }

  let restoreError = $state(""); // 啟動恢復失敗(檔案不存在/損壞)→ 頂列提示
  type SessionMutationTag = "startup-restore" | "manual-restore";
  function restoreSession(tag: SessionMutationTag, path: string) {
    return mutations.run(mutKey.session, tag, () => engineCommand("load_session", { path }));
  }

  async function startupRestore(): Promise<void> {
    const path = restorePath();
    if (!path || currentSessionPath !== null || restoreError) return;
    // 併發觸發只等待目前 restore；不再排第二次 load_session。
    if (mutations.busy(mutKey.session)) {
      await mutations.whenIdle(mutKey.session);
      return startupRestore();
    }
    const restoreEpoch = conn.epoch;
    const result = await restoreSession("startup-restore", path);
    if (conn.epoch !== restoreEpoch) return;
    if (result.status === "completed") {
      applyLoadedSession(result.value);
      currentSessionPath = path;
    } else if (result.status === "failed") {
      rejectRevisionBaseline();
      // 檔案不存在/損壞 = 開空白 + 頂列提示,不擋啟動
      restoreError = errorText(result.error);
    }
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
      confirmRevisionBaseline(r.revision);
    } catch {
      rejectRevisionBaseline();
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

  // 錯誤提示:結構化錯誤進 notice;「啟動競態殘留」的清理以 bridge 提供的 code 判斷
  let lastNoticeErr: CommandError | null = null;
  function showNotice(e: unknown) {
    lastNoticeErr = toCommandError(e);
    notice = errorText(e);
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
      deviceStream = transitionDeviceStream(deviceStream, {
        type: "devicesChanged",
        devices: devices.map((device) => ({
          deviceKey: device.deviceKey,
          name: device.name,
          preferredBufferSize: device.preferredBufferSize,
          bufferSizes: device.bufferSizes,
        })),
      }).state;
      if (!selected && devices.length) {
        // P1-D:啟動偏好 —— 上次「成功啟動」的裝置優先(還在清單才用),
        // 否則依列舉序逐個嘗試到成功(不是 devices[0] 失敗即停)
        await autoStart();
      }
      if (lastNoticeErr?.code === "not_connected") {
        lastNoticeErr = null;
        notice = ""; // 啟動競態殘留,成功即清(code 判斷,非訊息字面值)
      }
    } catch (e) {
      showNotice(e);
    }
  }

  function statusDeviceConfig(value: EngineStatus): DeviceStreamConfig | null {
    return value.running && value.deviceKey
      ? { deviceKey: value.deviceKey, bufferSize: value.bufferSize }
      : null;
  }

  async function executeDeviceRequest(request: DeviceStreamRequest): Promise<EngineStatus> {
    if (request.kind === "stop") return engineCommand("stop");
    if (request.kind === "restart" || request.kind === "rollback")
      await engineCommand("stop");
    return engineCommand("start", {
      deviceKey: request.target!.deviceKey,
      sampleRate: null,
      bufferSize: request.target!.bufferSize,
    });
  }

  /** 執行狀態機目前的單一 effect；每次 command 都經同一 device queue。 */
  async function runDeviceRequest(
    request: DeviceStreamRequest,
    tag: "auto-start" | "switch" | "transport",
  ): Promise<boolean> {
    if (deviceStream.request?.id !== request.id) return false;
    const requestEpoch = conn.epoch;
    if (request.kind === "auto-start" && request.target) {
      selected = request.target.deviceKey;
      bufSize = request.target.bufferSize;
    }
    const rollbackFailure = request.kind === "rollback" ? deviceStream.lastStartError : null;
    const result = await mutations.run(mutKey.device, tag, () => executeDeviceRequest(request));
    if (conn.epoch !== requestEpoch) return false;
    if (result.status === "superseded") return false;

    if (result.status === "failed") {
      const transition = transitionDeviceStream(
        deviceStream,
        request.kind === "stop"
          ? { type: "stopFailed", requestId: request.id }
          : { type: "startFailed", requestId: request.id, failure: result.error },
      );
      if (!transition.accepted) return false;
      deviceStream = transition.state;
      const next = deviceStream.request;
      if (next && next.id !== request.id)
        return runDeviceRequest(next, tag);
      if (request.kind === "rollback") {
        notice = `切換與回滾都失敗,音訊已停止 — ${errorText(result.error)}`;
        addNotice("error", "裝置切換與回滾都失敗,音訊已停止", errorText(result.error));
      }
      return false;
    }

    const actual = statusDeviceConfig(result.value) ?? request.target;
    const alreadyObserved =
      request.kind === "stop"
        ? deviceStream.phase === "idle" && deviceStream.request === null
        : deviceStream.phase === "running" &&
          deviceStream.request === null &&
          sameDeviceStreamConfig(deviceStream.lastGood, actual);
    const transition = transitionDeviceStream(
      deviceStream,
      request.kind === "stop"
        ? { type: "stopSucceeded", requestId: request.id }
        : { type: "startSucceeded", requestId: request.id, actual: actual! },
    );
    if (transition.accepted) deviceStream = transition.state;
    else if (!alreadyObserved) return false;
    applyAuthoritativeStatus({ status: result.value });

    const reached =
      request.kind === "stop"
        ? deviceStream.phase === "idle" && deviceStream.request === null
        : deviceStream.phase === "running" &&
          deviceStream.request === null &&
          sameDeviceStreamConfig(deviceStream.lastGood, actual);
    if (!reached) return false;
    if (request.kind !== "stop" && actual) persistLastWorking(actual.deviceKey, actual.bufferSize);
    if (request.kind === "rollback" && rollbackFailure) {
      notice = `切換失敗,已恢復原裝置/Buffer — ${errorText(rollbackFailure)}`;
    }
    return true;
  }

  /** 依偏好順序只發動一輪；候選推進與失敗累積由狀態機持有。 */
  async function autoStart(): Promise<void> {
    await settingsReady();
    const preferredDeviceKey = appSettings?.lastWorkingDevice ?? null;
    const requested = transitionDeviceStream(deviceStream, {
      type: "autoStart",
      preferredDeviceKey,
      preferredBufferSize: appSettings?.lastWorkingBuffer ?? null,
    });
    if (!requested.accepted || !requested.state.request) return;
    deviceStream = requested.state;
    const ok = await runDeviceRequest(requested.state.request, "auto-start");
    const failures = deviceStream.autoStartFailures.map((item) => {
      const name = devices.find((device) => device.deviceKey === item.target.deviceKey)?.name;
      return `${name ?? item.target.deviceKey}:${friendlyError(item.failure).friendly}`;
    });
    if (
      !ok &&
      deviceStream.phase === "failed" &&
      deviceStream.request === null &&
      failures.length > 0
    ) {
      addNotice("error", "沒有任何 ASIO 裝置能成功啟動", failures.join("\n"));
      notice = failures.join(" | ");
      return;
    }
    if (!ok) return;
    const actual = deviceStream.lastGood;
    if (actual && actual.deviceKey !== preferredDeviceKey) {
      const name = devices.find((device) => device.deviceKey === actual.deviceKey)?.name;
      addNotice(
        "info",
        `已改用「${name ?? actual.deviceKey}」啟動(偏好裝置不可用)`,
        `偏好:${preferredDeviceKey ?? "無"};失敗:${failures.join(" | ")}`,
      );
    }
  }

  async function start(deviceKey = selected): Promise<boolean> {
    if (!deviceKey) return false;
    const target = { deviceKey, bufferSize: bufSize };
    const requested = transitionDeviceStream(
      deviceStream,
      status?.running || deviceStream.request
        ? { type: "restartRequested", target }
        : { type: "startRequested", target },
    );
    if (!requested.accepted || !requested.state.request) return false;
    deviceStream = requested.state;
    notice = "";
    return runDeviceRequest(requested.state.request, "transport");
  }

  /** P1-D:lastWorkingDevice/lastWorkingBuffer —— 僅成功 start 後呼叫 */
  function persistLastWorking(key: string, buf: number | null): void {
    if (appSettings?.lastWorkingDevice === key && appSettings?.lastWorkingBuffer === buf) return;
    if (appSettings) {
      appSettings.lastWorkingDevice = key;
      appSettings.lastWorkingBuffer = buf;
    }
    const patch = { lastWorkingDevice: key, lastWorkingBuffer: buf };
    void writeSettings("last-working", patch).then((result) => {
      if (result.status === "completed") acceptSettingsReply(patch, result.value);
    });
  }

  function queueRestart(key = selected): void {
    if (!key) return;
    const requested = transitionDeviceStream(deviceStream, {
      type: "restartRequested",
      target: { deviceKey: key, bufferSize: bufSize },
    });
    if (!requested.accepted || !requested.state.request) return;
    deviceStream = requested.state;
    notice = "";
    void runDeviceRequest(requested.state.request, "switch");
  }

  async function openDevicePanel() {
    try {
      await engineCommand("open_device_panel", {});
      panelOpen = true; // devices_changed 回來 = 面板關閉,清旗標並重建
    } catch (e) {
      showNotice(e);
    }
  }

  async function stop() {
    const requested = transitionDeviceStream(deviceStream, { type: "stopRequested" });
    if (!requested.accepted || !requested.state.request) return;
    deviceStream = requested.state;
    await runDeviceRequest(requested.state.request, "transport");
  }

  // ---- E:背景掃描 job(共用 registry;回覆立即回 jobId,進度走 events)----

  async function startScan(): Promise<boolean> {
    const started = transitionScanJob(scanJob, { type: "startRequested" });
    if (!started.accepted) return false;
    scanJob = started.state;
    try {
      const r = await engineCommand("start_scan", {});
      scanJob = transitionScanJob(scanJob, {
        type: "startConfirmed",
        jobId: r.jobId as number,
      }).state;
      return true;
    } catch (e) {
      const message = errorText(e);
      scanJob = transitionScanJob(scanJob, {
        type: "startRejected",
        error: message,
      }).state;
      addNotice("error", "無法開始 VST 掃描", message, 0, true, () => {
        scanJob = transitionScanJob(scanJob, { type: "dismiss", jobId: null }).state;
      });
      return false;
    }
  }

  async function cancelScan() {
    if (!scanRunning) return;
    try {
      await engineCommand("cancel_scan", {});
    } catch (e) {
      const message = errorText(e);
      scanJob = transitionScanJob(scanJob, {
        type: "cancelRejected",
        error: message,
      }).state;
      const failedJobId = scanJob.jobId;
      addNotice("error", "無法取消 VST 掃描", message, 0, true, () => {
        scanJob = transitionScanJob(scanJob, {
          type: "dismiss",
          jobId: failedJobId,
        }).state;
      });
    }
  }

  // ---------- 軌道新增 ----------

  async function addTrack(kind: "audio" | "app" | "fx" | "output") {
    try {
      await engineCommand("track_add", { kind });
    } catch (e) {
      showNotice(e);
    }
  }

  // ---------- 拖曳排序(HTML5 DnD;事件委派在 lane,跨群組不 preventDefault = 不可放) ----------
  // P1-I:strip 位置純幾何計算(laneView.ts)—— 虛擬化後不在 DOM 的 strip 也算得對

  type LaneGroup = "input" | "output";
  let drag = $state<{ id: number; group: LaneGroup } | null>(null);
  let dropAt = $state<{ group: LaneGroup; pos: number } | null>(null);
  // dragstart 的 target 會被重定向到 draggable 祖先(strip),推桿等控制項的真目標
  // 只能靠 pointerdown(capture)先記起來
  let pressEl: HTMLElement | null = null;

  // P1-I:lane 捲動位置(虛擬化窗口)—— svelte:window 不動;各 lane onscroll 更新
  let laneScroll = $state({ input: 0, output: 0 });
  let laneWidth = $state({ input: 0, output: 0 });
  let inputLaneEl = $state<HTMLDivElement | null>(null);
  let outputLaneEl = $state<HTMLDivElement | null>(null);
  const inputWin = $derived(visibleRange(laneScroll.input, laneWidth.input, inputTracks.length));
  const outputWin = $derived(visibleRange(laneScroll.output, laneWidth.output, outputTracks.length));
  const inputSp = $derived(spacerWidths(inputWin.start, inputWin.end, inputTracks.length));
  const outputSp = $derived(spacerWidths(outputWin.start, outputWin.end, outputTracks.length));

  function bindLane(group: LaneGroup): HTMLDivElement | null {
    return group === "input" ? inputLaneEl : outputLaneEl;
  }
  function onLaneScroll(group: LaneGroup, e: Event): void {
    const el = e.currentTarget as HTMLElement;
    laneScroll[group] = el.scrollLeft;
    laneWidth[group] = el.clientWidth;
  }

  function lanePos(lane: HTMLElement, x: number, count: number): number {
    // 虛擬化安全:幾何計算,不查 DOM
    return dropPosFromX(x, lane.getBoundingClientRect().left, lane.scrollLeft, count);
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
      dropAt = {
        group,
        pos: lanePos(e.currentTarget as HTMLElement, e.clientX, laneArr(group).length),
      };
    };
  }
  function onDrop(group: LaneGroup) {
    return (e: DragEvent) => {
      if (!drag || drag.group !== group) return;
      e.preventDefault();
      const arr = laneArr(group);
      const dragIdx = arr.findIndex((t) => t.trackId === drag!.id);
      if (dragIdx >= 0) {
        const pos = lanePos(e.currentTarget as HTMLElement, e.clientX, arr.length);
        const reordered = reorderLane(
          tracks.map((t) => t.trackId),
          arr.map((t) => t.trackId),
          drag.id,
          pos,
        );
        const target = reordered.indexOf(drag.id);
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
      confirmRevisionBaseline(r.revision); // 存成功才清 dirty
      currentSessionPath = path;
      rememberLastSession(path);
      notice = "";
      restoreError = ""; // 手動救回 = 啟動失敗警示該滅
    } catch (e) {
      rejectRevisionBaseline();
      showNotice(e);
      addNotice("error", "Session 儲存失敗", errorText(e));
    }
  }

  /** P1-L:起始區「最近 Session」入口 —— 直接載上次路徑(與 loadSession 同款 dirty 流程) */
  async function reopenLastSession(): Promise<void> {
    const p = appSettings?.lastSessionPath;
    if (!p) return;
    if (dirty) {
      const choice = await askDirty();
      const plan = resolveDirtyChoice(dirty, choice);
      if (plan.shouldSave) {
        const okSave = await saveSessionForClose();
        if (!okSave) return;
      } else if (!plan.proceed) {
        return;
      }
    }
    try {
      const result = await restoreSession("manual-restore", p);
      if (result.status === "superseded") return;
      if (result.status === "failed") throw result.error;
      applyLoadedSession(result.value);
      currentSessionPath = p;
      notice = "";
      restoreError = "";
    } catch (e) {
      rejectRevisionBaseline();
      showNotice(e);
      addNotice("error", "Session 載入失敗", errorText(e));
    }
  }

  // session 載入後套裝置 + 自動啟用(率跟 driver 現行值);跑著時 = 重建到 session 裝置
  function applyLoadedSession(r: Record<string, unknown>) {
    confirmRevisionBaseline(r.revision); // 載成功才清 dirty
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
      const result = await restoreSession("manual-restore", path);
      if (result.status === "superseded") return;
      if (result.status === "failed") throw result.error;
      applyLoadedSession(result.value);
      currentSessionPath = path;
      rememberLastSession(path);
      notice = "";
      restoreError = "";
    } catch (e) {
      rejectRevisionBaseline();
      showNotice(e);
      addNotice("error", "Session 載入失敗", errorText(e));
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

  async function refreshAutostartState() {
    if (autostartBusy) return;
    autostartBusy = true;
    try {
      autostartEnabled = await isAutostartEnabled();
    } catch (e) {
      autostartEnabled = null;
      addNotice("error", "無法讀取 Windows 自動啟動狀態", errorText(e));
    } finally {
      autostartBusy = false;
    }
  }

  async function updateAutostart(enabled: boolean) {
    if (autostartBusy) return;
    autostartBusy = true;
    try {
      if (enabled) await enableAutostart();
      else await disableAutostart();
      const actual = await isAutostartEnabled();
      if (actual !== enabled) throw new Error("Windows 回報的自動啟動狀態未完成變更");
      autostartEnabled = actual;
    } catch (e) {
      try {
        autostartEnabled = await isAutostartEnabled();
      } catch {
        autostartEnabled = null;
      }
      addNotice("error", enabled ? "開啟自動啟動失敗" : "關閉自動啟動失敗", errorText(e));
    } finally {
      autostartBusy = false;
    }
  }

  async function updateStartMinimized(enabled: boolean) {
    if (!appSettings || startMinimizedBusy) return;
    startMinimizedBusy = true;
    const previous = appSettings.startMinimizedOnAutostart;
    appSettings.startMinimizedOnAutostart = enabled;
    const patch = { startMinimizedOnAutostart: enabled };
    const result = await writeSettings("start-minimized", patch);
    if (result.status === "completed") {
      acceptSettingsReply(patch, result.value);
    } else if (result.status === "failed") {
      appSettings.startMinimizedOnAutostart = previous;
      addNotice("error", "自動啟動縮小偏好儲存失敗", errorText(result.error));
    }
    startMinimizedBusy = false;
  }

  function openGeneral() {
    tab = "general";
    refreshFolderFiles().catch(() => {});
    void refreshAutostartState();
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
          startMinimizedOnAutostart: false,
        };
      appSettings.sessionDir = d as string;
      appSettings.startupFile = null; // 換資料夾 = 舊選擇作廢
      await persistSettings();
      await refreshFolderFiles();
    } catch (e) {
      showNotice(e);
    }
  }

  const running = $derived(status?.running ?? false);
  /** P1-L:連線顯示模型(細分 phase + tone;connPhase.ts 純函式) */
  const cv = $derived(connView(conn, connProbeErr));
  const selDev = $derived(devices.find((d) => d.deviceKey === selected) ?? null);
  // ASIO / plugin latency 單位 = samples;統一走 lib/format.samplesToMs(2 位小數)
</script>

<header class="bar">
  <!-- P1-L:連線細分狀態(spawning/connected/spawn_failed/version mismatch…)+ retry -->
  <span class="dot" class:ok={cv.tone === "ok"} class:err={cv.tone === "err"}></span>
  <span data-tooltip={cv.detail ? `連線診斷：${cv.detail}` : `引擎連線狀態：${cv.label}`}>{cv.label}</span>
  {#if cv.phase === "spawn_failed" || cv.phase === "version_mismatch" || cv.phase === "disconnected"}
    <button class="settings" onclick={() => void respawnEngine()}>重試連線</button>
  {/if}
  {#if cv.phase !== "connected"}
    <button
      class="settings"
      data-tooltip="複製引擎連線狀態、版本與診斷原因至剪貼簿。"
      onclick={() =>
        void copyText(
          `phase=${cv.phase} connected=${conn.connected} epoch=${conn.epoch} engine=${conn.engineVersion} detail=${cv.detail}`,
        ).then((ok) => addNotice("info", ok ? "已複製連線診斷" : "複製失敗(剪貼簿不可用)"))}
      >複製診斷</button
    >
  {/if}
  <button class="settings" onclick={() => (settingsOpen = true)}>設定</button>
  <button
    class="settings"
    disabled={!conn.connected && !scanRunning}
    onclick={() => (scanRunning ? void cancelScan() : void startScan())}
    data-tooltip={scanRunning
      ? "取消目前的背景 VST 掃描；取消後會保留掃描前的 plugin 清單。"
      : "在背景掃描預設 VST3 目錄；未變更的 plugin 會沿用持久快取，不重新載入。"}
    >{scanRunning
      ? `取消掃描${scanProgress ? ` ${scanProgress.done}/${scanProgress.total}` : ""}`
      : "掃描 VST"}</button
  >
  {#if audioStale}
    <button class="err aslink" onclick={() => (settingsOpen = true)} data-tooltip={`音訊啟動異常：${notice}`}
      >音訊未啟動 — 詳情見設定</button
    >
  {/if}
  {#if restoreError}
    <button class="err aslink" onclick={() => (settingsOpen = true)} data-tooltip={`Session 恢復失敗：${restoreError}`}
      >Session 恢復失敗,已開空白 — 詳情見設定</button
    >
  {/if}
  {#if missing.length > 0}
    <button
      class="err aslink"
      onclick={() => (missing = [])}
      data-tooltip={`未載入的 plugin：\n${missing
        .map((m) => `${m.trackName} [${m.index}] ${m.name || m.pluginPath}：${m.message}`)
        .join("\n")}`}
      >⚠ {missing.length} 個 plugin 無法載入(已保留 placeholder)— 點此收起</button
    >
  {/if}
  {#if dirty}
    <span class="dim" data-tooltip="目前 Session 有尚未儲存的變更。">● 未儲存</span>
  {/if}
  <!-- P1-O:通知中心(最近數條;技術細節可複製) -->
  {#each notices as n (n.id)}
    <span class="notice" class:iserr={n.kind === "error"}>
      <span class="notice-msg" data-tooltip={`詳細訊息：${n.raw ?? n.msg}`}>{n.msg}</span>
      {#if n.raw}
        <button class="settings" onclick={() => copyNotice(n)} data-tooltip="複製此通知的完整技術資訊至剪貼簿。">複製</button>
      {/if}
      {#if n.dismissible}
        <button
          class="settings"
          onclick={() => dismissNotice(n.id)}
          aria-label="關閉此通知"
          data-tooltip="關閉此通知。"
        >×</button>
      {/if}
    </span>
  {/each}
  <span style="flex:1"></span>
  {#if running}
    <span class="dot ok"></span>
    <!-- P1-J:callback 負載 %(RT TSC 量測;持續 >100% = 過載警示走通知中心) -->
    <span
      class="mono"
      class:err={overloadOn}
      data-tooltip="音訊 callback 的即時運算負載；持續達 100% 以上表示處理逾時，可能產生 xrun 或爆音。"
      >load {Math.round((meters?.callbackLoad ?? 0) * 100)}%</span
    >
    <span class="mono"
      >{status!.sampleRate} Hz · buf {status!.bufferSize} · lat input/output
      {samplesToMs(status!.inputLatency, status!.sampleRate)}ms/{samplesToMs(
        status!.outputLatency,
        status!.sampleRate,
      )}ms · xrun {status!.xruns}</span
    >
  {:else if selDev?.currentSampleRate}
    <span
      class="dim mono"
      data-tooltip="目前由音訊驅動程式提供的取樣率與緩衝大小；取樣率請於硬體控制面板調整。"
      >{selDev.currentSampleRate} Hz · buf {bufSize ?? selDev.preferredBufferSize}</span
    >
  {/if}
  {#if latencyEnabled}
    <button
      class="settings mono"
      onclick={() => (latencyDrawerOpen = !latencyDrawerOpen)}
      aria-expanded={latencyDrawerOpen}
      aria-label="開啟 Plugin 延遲與 Process Load 明細"
      data-tooltip="Monitor／Stream 的最大有效 Plugin Path Latency；點擊開啟逐 instance 與 PDC 明細。"
      >plug M {samplesToMs(status?.pluginDelay?.monitorSamples ?? null, status?.sampleRate ?? 0)} / S {samplesToMs(status?.pluginDelay?.streamSamples ?? null, status?.sampleRate ?? 0)} ms</button
    >
  {/if}
  {#if status?.pluginFails}
    <span
      class="err mono"
      data-tooltip="即時音訊執行緒中的 plugin 處理失敗累計；失敗時該 plugin 會維持 bypass，避免中斷音訊。"
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
        bind:this={inputLaneEl}
        role="list"
        aria-label="輸入軌帶"
        onpointerdowncapture={(e) => (pressEl = e.target as HTMLElement)}
        onscroll={(e) => onLaneScroll("input", e)}
        ondragstart={onDragStart("input")}
        ondragover={onDragOver("input")}
        ondrop={onDrop("input")}
        ondragend={onDragEnd}
      >
        {#if inputSp.left > 0}<div class="spacer" style="flex:0 0 {inputSp.left}px" aria-hidden="true"></div>{/if}
        {#each inputTracks.slice(inputWin.start, inputWin.end) as t, vi (t.trackId)}
          {@const i = inputWin.start + vi}
          <TrackStrip
            track={t}
            {tracks}
            {devices}
            selectedDeviceKey={selected}
            {meterView}
            metered={t.metered !== false}
            {latencyEnabled}
            scanModules={scanModules}
            scanFailed={scanFailed}
            scanRunning={scanRunning}
            scanProgress={scanProgress}
            scanNotice={scanNotice}
            onCancelScan={cancelScan}
            {openMenu}
            dropBefore={dropAt?.group === "input" && dropAt.pos === i}
            dropAfter={dropAt?.group === "input" && dropAt.pos === i + 1}
            dragging={drag?.id === t.trackId}
          />
        {:else}
          <!-- P1-L:空輸入帶的起始區(中性入口:最近 session、裝置狀態、加入軌) -->
          <div class="startcard">
            <p class="dim">還沒有輸入軌 —— 加入 Audio(App 軌抓程式聲音)、或從上次的 Session 恢復。</p>
            <div class="startrow">
              <button class="mini" onclick={() => addTrack("audio")}>＋ Audio 軌(麥克風/樂器)</button>
              <button class="mini" onclick={() => addTrack("app")}>＋ App 軌(抓程式聲音)</button>
              <button class="mini" onclick={loadSession}>載入 Session…</button>
              {#if appSettings?.lastSessionPath}
                <button
                  class="mini"
                  onclick={() => reopenLastSession()}
                  data-tooltip={`重新開啟最近使用的 Session：\n${appSettings.lastSessionPath}`}
                  >最近:{appSettings.lastSessionPath.split(/[\\/]/).pop() ?? ""}</button
                >
              {/if}
            </div>
            <p class="dim startinfo">
              裝置:{selDev?.name ?? (devices.length ? "選擇中" : "無 ASIO 裝置")} ·
              {running ? `執行中 ${Math.round(status!.sampleRate)} Hz` : "音訊未啟動"} ·
              監聽/串流輸出軌已就緒
            </p>
          </div>
        {/each}
        {#if inputSp.right > 0}<div class="spacer" style="flex:0 0 {inputSp.right}px" aria-hidden="true"></div>{/if}
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
        bind:this={outputLaneEl}
        role="list"
        aria-label="輸出軌帶"
        onpointerdowncapture={(e) => (pressEl = e.target as HTMLElement)}
        onscroll={(e) => onLaneScroll("output", e)}
        ondragstart={onDragStart("output")}
        ondragover={onDragOver("output")}
        ondrop={onDrop("output")}
        ondragend={onDragEnd}
      >
        {#if outputSp.left > 0}<div class="spacer" style="flex:0 0 {outputSp.left}px" aria-hidden="true"></div>{/if}
        {#each outputTracks.slice(outputWin.start, outputWin.end) as t, vi (t.trackId)}
          {@const i = outputWin.start + vi}
          <TrackStrip
            track={t}
            {tracks}
            {devices}
            selectedDeviceKey={selected}
            {meterView}
            metered={t.metered !== false}
            {latencyEnabled}
            scanModules={scanModules}
            scanFailed={scanFailed}
            scanRunning={scanRunning}
            scanProgress={scanProgress}
            scanNotice={scanNotice}
            onCancelScan={cancelScan}
            {openMenu}
            dropBefore={dropAt?.group === "output" && dropAt.pos === i}
            dropAfter={dropAt?.group === "output" && dropAt.pos === i + 1}
            dragging={drag?.id === t.trackId}
          />
        {:else}
          <p class="dim hint">新增輸出軌(監聽 / 串流)</p>
        {/each}
        {#if outputSp.right > 0}<div class="spacer" style="flex:0 0 {outputSp.right}px" aria-hidden="true"></div>{/if}
      </div>
    </section>
  </section>
</main>

{#if latencyEnabled}
  <LatencyDrawer
    open={latencyDrawerOpen}
    {status}
    {meters}
    onClose={() => (latencyDrawerOpen = false)}
  />
{/if}

<!-- P2-M:右鍵選單(全域一份) -->
<ContextMenu {menu} onClose={() => (menu = null)} />

<dialog
  bind:this={settingsDlg}
  class="settingsdlg"
  onclose={() => (settingsOpen = false)}
>
  <div class="dialog-head">
    <span class="dialog-title">設定</span>
    <button
      class="dialog-close"
      type="button"
      aria-label="關閉設定視窗"
      data-tooltip="關閉設定視窗。"
      onclick={() => settingsDlg?.close()}>×</button
    >
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
        data-tooltip="開啟 ASIO 驅動程式控制面板以調整硬體取樣率。Buffer 大小請使用 RoudaMix 的 Buffer 選單；面板關閉後會自動同步並重建音訊引擎。"
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
    <h2>啟動</h2>
    <div class="formrow checkrow" class:disabled={autostartBusy || autostartEnabled === null}>
      <input
        id="autostartenabled"
        type="checkbox"
        checked={autostartEnabled === true}
        disabled={autostartBusy || autostartEnabled === null}
        onchange={(e) => void updateAutostart(e.currentTarget.checked)}
      />
      <label
        class="checklabel"
        for="autostartenabled"
        data-tooltip="在目前 Windows 使用者登入後自動啟動 RoudaMix。"
        >Windows 登入後自動啟動 RoudaMix</label
      >
      {#if autostartBusy}<span class="dim">讀取中…</span>{/if}
    </div>
    <div
      class="formrow checkrow"
      class:disabled={!appSettings ||
        autostartEnabled !== true ||
        autostartBusy ||
        startMinimizedBusy}
    >
      <input
        id="startminimizedonautostart"
        type="checkbox"
        checked={appSettings?.startMinimizedOnAutostart ?? false}
        disabled={!appSettings ||
          autostartEnabled !== true ||
          autostartBusy ||
          startMinimizedBusy}
        onchange={(e) => void updateStartMinimized(e.currentTarget.checked)}
      />
      <label
        class="checklabel"
        for="startminimizedonautostart"
        data-tooltip="只影響 Windows 自動啟動；手動開啟 RoudaMix 時仍會顯示主視窗。"
        >自動啟動時縮小到系統匣</label
      >
    </div>
    <h2>視窗</h2>
    <div class="formrow">
      <label class="formlabel" for="closebehavior">關閉視窗時</label>
      <select
        id="closebehavior"
        value={appSettings?.closeBehavior ?? ""}
        disabled={!appSettings}
        onchange={(e) => {
          if (!appSettings) return;
          appSettings.closeBehavior = e.currentTarget.value as CloseBehavior;
          persistSettings();
        }}
      >
        <option value="" disabled>首次關閉時詢問</option>
        <option value="tray">縮小到系統匣</option>
        <option value="exit">關閉程式</option>
      </select>
    </div>
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
      <span
        class="dim mono dirpath"
        data-tooltip={`Session 預設儲存位置：\n${appSettings?.sessionDir ?? "未設定"}`}
      >
        {appSettings?.sessionDir ?? "(未設定)"}
      </span>
      <button onclick={pickSessionDir}>選擇…</button>
    </div>
    {#if restoreError}
      <p class="err mono">啟動恢復失敗:{restoreError}</p>
    {/if}
    {#if appSettings?.startupMode === "last"}
      <div class="formrow">
        <span
          class="dim mono dirpath"
          data-tooltip={`啟動時開啟最近 Session：\n${appSettings.lastSessionPath ?? "尚未儲存過 Session"}`}
        >
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

<!-- 首次按主視窗關閉按鈕時選擇；偏好會寫入通用設定。Esc 僅取消本次關閉。 -->
<dialog
  bind:this={closeBehaviorDlg}
  class="dirtydlg"
  oncancel={(e) => {
    e.preventDefault();
    answerCloseBehavior(null);
  }}
>
  <p class="dirtyq">按下關閉按鈕時要執行哪個動作？</p>
  <p class="dim">之後可在「設定 → 通用」變更。</p>
  <div class="dirtyrow">
    <button class="primary" onclick={() => answerCloseBehavior("tray")}>縮小到系統匣</button>
    <button class="danger" onclick={() => answerCloseBehavior("exit")}>關閉程式</button>
  </div>
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
    /* P1-K:窄視窗不裁掉關閉/確認 —— 內容上限 85vh、垂直捲動 */
    max-height: 85vh;
    overflow-y: auto;
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
  .checkrow {
    min-height: 26px;
  }
  .checkrow input[type="checkbox"] {
    width: 16px;
    height: 16px;
    margin: 0;
    accent-color: var(--accent);
  }
  .checklabel {
    font-size: 13px;
  }
  .checkrow.disabled .checklabel {
    opacity: 0.45;
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
  /* P1-I:虛擬化 spacer(撐住捲軸寬度;flex gap 由 spacerWidths 校正) */
  .spacer {
    flex: 0 0 auto;
    min-width: 0;
  }
  /* P1-L:空帶起始區 */
  .startcard {
    border: 1px dashed var(--border);
    border-radius: 8px;
    padding: 14px;
    display: flex;
    flex-direction: column;
    gap: 10px;
    max-width: 460px;
  }
  .startcard p {
    margin: 0;
  }
  .startrow {
    display: flex;
    flex-wrap: wrap;
    gap: 6px;
  }
  .startinfo {
    font-size: 11px;
  }
  /* P1-O:頂欄通知 */
  .notice {
    display: inline-flex;
    align-items: center;
    gap: 4px;
    max-width: 340px;
  }
  .notice-msg {
    font-size: 12px;
    color: var(--text);
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
    user-select: text; /* P1-O:錯誤文字可選取複製 */
  }
  .notice.iserr .notice-msg {
    color: var(--warn);
  }
  .dot.err {
    background: var(--err);
  }
</style>
