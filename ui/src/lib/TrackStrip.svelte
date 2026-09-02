<script lang="ts">
  // 軌條:輸入/輸出軌共用。由上而下 = 名稱列 → 來源/輸出裝置 → 目的地多選 →
  // VST 機架(電源=bypass、單擊名稱開 editor、上→下=訊號序)→ 推桿+靜音 →
  // 錶 → 底部顏色條。engine 溝通自含(ipc 直呼),App 只餵狀態。
  import MeterCanvas from "./MeterCanvas.svelte";
  import { powerOff, powerOn } from "./icons";
  import { mountDragGhost, removeDragGhost } from "./ghost";
  import { open as openFile } from "@tauri-apps/plugin-dialog";
  import { engineCommand } from "./protocol-commands.generated";
  import { cssColor, parseColor, stripOfTrack } from "./tracks";
  import { MutationQueue, mutKey } from "./mutations";
  import { reorderLane } from "./laneOrder";
  import {
    beginMonitorBypass,
    finishMonitorBypass,
    isMonitorBypassPending,
    type MonitorBypassTransactions,
  } from "./monitorBypass";
  import { friendlyError } from "./errors";
  import AppPicker from "./AppPicker.svelte";
  import ConfirmDialog from "./ConfirmDialog.svelte";
  import type {
    AudioApp,
    DeviceInfo,
    MeterStrip,
    RackSlot,
    RenderDevice,
    ScanFailure,
    ScanModule,
    Track,
  } from "./types";

  let {
    track,
    tracks,
    devices,
    selectedDeviceKey,
    strips,
    metered = true,
    latencyEnabled = false,
    // 掃描 job 由 App 統一跑(共用 registry,所有軌同一份清單;進度/取消也在 App)
    scanModules = [],
    scanFailed = [],
    scanRunning = false,
    scanProgress = null,
    scanNotice = "",
    onCancelScan,
    openMenu,
    dropBefore = false,
    dropAfter = false,
    dragging = false,
  }: {
    track: Track;
    tracks: Track[];
    devices: DeviceInfo[];
    selectedDeviceKey: string;
    strips: MeterStrip[] | undefined;
    metered?: boolean;
    latencyEnabled?: boolean;
    scanModules?: ScanModule[];
    scanFailed?: ScanFailure[];
    scanRunning?: boolean;
    scanProgress?: { done: number; total: number } | null;
    scanNotice?: string;
    onCancelScan: () => void;
    /** P2-M:請求右鍵選單(App 持有全域 ContextMenu;{x,y} + items) */
    openMenu: (x: number, y: number, label: string, items: Array<{ label: string; disabled?: boolean; run: () => void }>) => void;
    dropBefore?: boolean;
    dropAfter?: boolean;
    dragging?: boolean;
  } = $props();

  let err = $state("");
  let scanDlg = $state<HTMLDialogElement | null>(null);
  let destDlg = $state<HTMLDialogElement | null>(null);
  // WASAPI render 裝置清單(focus 時拉,保持常新)
  let renderDevices = $state<RenderDevice[]>([]);
  // P2-N:刪除確認(track/plugin);系統輸出軌不可刪(engine 權威)
  let confirmBox = $state<{ title: string; impact: string[]; confirmLabel: string } | null>(null);
  let pendingDelete: (() => void) | null = null;
  let monitorBypassTransactions = $state<MonitorBypassTransactions>(new Map());
  let pendingLatencyPolicy = $state(false);
  // P1-C:app 軌程序選擇器(needsRebind / 程序死亡重綁)
  let pickerOpen = $state(false);

  // P1-B:本軌命令序列化 + latest-wins(連點 mute/bypass/dests 不會用 stale
  // props 互蓋);錯誤進 err 顯示,engine 權威 status event 會把實際值帶回
  const mq = new MutationQueue((_, e) => (err = friendlyError(String(e)).friendly));

  // placeholder(missing/broken)槽:黯淡顯示 + 重試/重新定位/移除
  const isPh = (s: RackSlot) => s.availability !== undefined && s.availability !== "ok";
  const phLabel = (s: RackSlot) =>
    s.availability === "missing" ? "遺失" : "載入失敗";

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

  async function loadRenderDevices() {
    try {
      const r = await engineCommand("list_render_devices", {});
      renderDevices = (r.devices as RenderDevice[]) ?? [];
    } catch (e) {
      err = String(e);
    }
  }

  // ---- P1-C:app 軌來源 = 程序選擇器(不猜 PID)----
  // needsRebind = session 載入後只有名字、還沒有有效 runtime pid
  const needsRebind = $derived(track.kind === "app" && (track.source === null || track.source.type !== "app" || track.source.pid === 0));

  function pickApp(pid: number, name: string) {
    err = "";
    mq.run(mutKey.track(track.trackId), "source", () =>
      engineCommand("track_set_source", {
        trackId: track.trackId,
        source: { type: "app", pid, name },
      }),
    );
  }
  function clearApp() {
    err = "";
    mq.run(mutKey.track(track.trackId), "source", () =>
      engineCommand("track_set_source", { trackId: track.trackId, source: null }),
    );
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

  /** P1-B:dests 本地疊加 —— 命令在飛時繼續勾選,以「本地最新 + 引擎回報」計算,
   *  使用者最後意圖不被 stale props 蓋掉;失敗 = err + engine 權威 status 對齊 */
  let destsLocal = $state<number[] | null>(null);
  $effect(() => {
    // engine 廣播追上本地值(或本地無疊加)= 清疊加
    if (destsLocal !== null && arraysEqual(track.dests, destsLocal)) destsLocal = null;
  });
  const shownDests = $derived(destsLocal ?? track.dests);
  function toggleDest(destId: number, checked: boolean) {
    err = "";
    const base = shownDests;
    const dests = checked ? [...base, destId] : base.filter((d) => d !== destId);
    destsLocal = dests;
    mq.run(mutKey.track(track.trackId), "dests", () =>
      engineCommand("track_set_dests", { trackId: track.trackId, dests }),
    );
  }
  function arraysEqual(a: number[], b: number[]): boolean {
    return a.length === b.length && a.every((v, i) => v === b[i]);
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

  function setMute(mute: boolean) {
    err = "";
    mq.run(mutKey.track(track.trackId), "mute", () =>
      engineCommand("track_set", { trackId: track.trackId, mute }),
    );
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

  /** P2-N:刪除前確認 + 影響清單。不做假 Undo —— plugin instance 的內部 state
   *  無法安全還原;刪除快照(整軌 JSON)留在 console,是日後 command-snapshot
   *  Undo 的資料基礎(見 contracts/undo-snapshot-design.md)。 */
  function removeTrack() {
    if (track.systemRole) return; // 系統輸出不可刪(engine 端權威擋)
    const downstream = tracks.filter((t) => t.dests.includes(track.trackId));
    const impact = [
      `刪除軌道「${track.name}」(${track.kind})`,
      track.plugins.length > 0 ? `移除 ${track.plugins.length} 個 plugin(含其參數與狀態)` : null,
      downstream.length > 0
        ? `${downstream.length} 條軌道對此軌的路由會一併移除(${downstream.map((t) => t.name).join("、")})`
        : null,
    ].filter((x): x is string => x !== null);
    pendingDelete = async () => {
      err = "";
      try {
        // 刪除快照(未來 Undo 的還原材料;不做恢復 UI)
        console.info("[undo-snapshot] track_remove", JSON.stringify(track));
        await engineCommand("track_remove", { trackId: track.trackId });
      } catch (e) {
        err = friendlyError(String(e)).friendly;
      }
    };
    confirmBox = { title: "刪除軌道?", impact, confirmLabel: "刪除" };
  }

  function removePlugin(id: number) {
    const slot = track.plugins.find((s) => s.instanceId === id);
    const impact = [
      `從「${track.name}」移除 plugin「${slot?.name ?? id}」`,
      "其參數與內部狀態一併消失(無法直接還原;可重新加入後重套 preset)",
    ];
    pendingDelete = async () => {
      err = "";
      try {
        if (slot) console.info("[undo-snapshot] remove_plugin", JSON.stringify(slot));
        await engineCommand("remove_plugin", { instanceId: id });
      } catch (e) {
        err = friendlyError(String(e)).friendly;
      }
    };
    confirmBox = { title: "移除 plugin?", impact, confirmLabel: "移除" };
  }

  function answerConfirm(yes: boolean) {
    const go = pendingDelete;
    confirmBox = null;
    pendingDelete = null;
    if (yes && go) void go();
  }

  // ---- VST 鏈 ----

  function bypass(slot: RackSlot) {
    err = "";
    mq.run(mutKey.plugin(slot.instanceId), "bypass", () =>
      engineCommand("set_bypass", {
        instanceId: slot.instanceId,
        bypassed: !slot.bypassed,
      }),
    );
  }

  async function monitorBypass(slot: RackSlot) {
    const request = beginMonitorBypass(
      monitorBypassTransactions,
      slot.instanceId,
      slot.monitorBypassed ?? false,
    );
    if (!request.command) return;
    err = "";
    monitorBypassTransactions = request.transactions;
    let outcome: "confirmed" | "rejected" = "confirmed";
    try {
      await engineCommand("set_monitor_bypass", request.command);
    } catch (e) {
      outcome = "rejected";
      err = friendlyError(String(e)).friendly;
    } finally {
      monitorBypassTransactions = finishMonitorBypass(
        monitorBypassTransactions,
        slot.instanceId,
        outcome,
      ).transactions;
    }
  }

  async function setLatencyPolicy(
    policy: "fullPdc" | "lowLatency",
    select: HTMLSelectElement,
  ) {
    if (pendingLatencyPolicy) return;
    err = "";
    const previous = track.latencyPolicy ?? "fullPdc";
    pendingLatencyPolicy = true;
    try {
      await engineCommand("track_set_latency_policy", { trackId: track.trackId, policy });
    } catch (e) {
      select.value = previous;
      err = friendlyError(String(e)).friendly;
    } finally {
      pendingLatencyPolicy = false;
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

  // ---- placeholder 回收:重試載入(原路徑)/重新定位(挑新檔,同 instanceId 原位)----
  async function retryPlugin(slot: RackSlot) {
    err = "";
    try {
      await engineCommand("retry_plugin", { instanceId: slot.instanceId });
    } catch (e) {
      err = String(e);
    }
  }
  async function relocatePlugin(slot: RackSlot) {
    err = "";
    try {
      const p = await openFile({
        title: "重新定位 plugin",
        multiple: false,
        directory: false,
        filters: [{ name: "VST3", extensions: ["vst3"] }],
      });
      if (!p) return;
      await engineCommand("retry_plugin", { instanceId: slot.instanceId, path: p });
    } catch (e) {
      err = String(e);
    }
  }

  // VST box 高度:預設自適應內容,拖底部把手拉長;雙擊把手還原自適應。
  // 拉過的高度存 localStorage(key 綁 trackId;session 還原同序 → id 穩定),重開保持
  let vstBox = $state<HTMLDivElement | null>(null);
  // trackId 在此元件生命週期不變(App 以 trackId 為 key each),取初始值即可
  // svelte-ignore state_referenced_locally
  const hKey = `rmix.vsth.${track.trackId}`;
  function readH(): number | null {
    try {
      const v = Number(localStorage.getItem(hKey));
      return Number.isFinite(v) && v >= 72 ? v : null;
    } catch {
      return null;
    }
  }
  let boxH = $state<number | null>(readH());

  function onGripDown(e: PointerEvent) {
    const box = vstBox;
    const col = box?.parentElement;
    if (!box || !col) return;
    e.preventDefault();
    const startY = e.clientY;
    const startH = box.offsetHeight;
    const move = (ev: PointerEvent) => {
      const max = col.clientHeight - 12; // 扣把手 + 間隙
      boxH = Math.max(72, Math.min(startH + ev.clientY - startY, max));
    };
    const up = () => {
      window.removeEventListener("pointermove", move);
      window.removeEventListener("pointerup", up);
      try {
        if (boxH !== null) localStorage.setItem(hKey, String(boxH));
      } catch {} // 隱私模式等存不了就算了,下次自適應
    };
    window.addEventListener("pointermove", move);
    window.addEventListener("pointerup", up);
  }
  function onGripDbl() {
    boxH = null;
    try {
      localStorage.removeItem(hKey);
    } catch {}
  }

  async function addPlugin(path: string, classId: string) {
    err = "";
    try {
      await engineCommand("add_plugin", { trackId: track.trackId, path, classId });
      scanDlg?.close(); // registry 共用,不清(別條軌直接用)
    } catch (e) {
      err = friendlyError(String(e)).friendly;
    }
  }

  // ---- P2-M:右鍵選單(移到最前/最後/上移/下移;拖曳之外的可發現路徑)----

  // 同帶群組(輸入 vs 輸出);move 語意 = 帶內重排
  const lanePeers = $derived(
    tracks.filter((t) => (track.kind === "output") === (t.kind === "output")),
  );
  function laneMove(to: "first" | "last" | "up" | "down") {
    const i = lanePeers.findIndex((t) => t.trackId === track.trackId);
    if (i < 0) return;
    let pos: number;
    if (to === "first") pos = 0;
    else if (to === "last") pos = lanePeers.length;
    else if (to === "up") {
      if (i === 0) return;
      pos = i - 1;
    } else {
      if (i >= lanePeers.length - 1) return;
      pos = i + 2; // 插到下一條之後(erase 補回後 = i+1)
    }
    const reordered = reorderLane(
      tracks.map((t) => t.trackId),
      lanePeers.map((t) => t.trackId),
      track.trackId,
      pos,
    );
    const masterIdx = reordered.indexOf(track.trackId);
    err = "";
    mq.run(mutKey.track(track.trackId), "move", () =>
      engineCommand("track_move", { trackId: track.trackId, newIndex: masterIdx }),
    );
  }
  function trackMenu(e: MouseEvent) {
    openMenu(e.clientX, e.clientY, `軌道「${track.name}」排序`, [
      { label: "上移", disabled: lanePeers[0]?.trackId === track.trackId, run: () => laneMove("up") },
      {
        label: "下移",
        disabled: lanePeers[lanePeers.length - 1]?.trackId === track.trackId,
        run: () => laneMove("down"),
      },
      { label: "移到最前", disabled: lanePeers[0]?.trackId === track.trackId, run: () => laneMove("first") },
      {
        label: "移到最後",
        disabled: lanePeers[lanePeers.length - 1]?.trackId === track.trackId,
        run: () => laneMove("last"),
      },
    ]);
  }
  function plugMove(slot: RackSlot, to: "first" | "last" | "up" | "down") {
    const chain = track.plugins;
    const i = chain.findIndex((s) => s.instanceId === slot.instanceId);
    if (i < 0) return;
    let ni = i;
    if (to === "first") ni = 0;
    else if (to === "last") ni = chain.length - 1;
    else if (to === "up") {
      if (i === 0) return;
      ni = i - 1;
    } else {
      if (i >= chain.length - 1) return;
      ni = i + 1;
    }
    if (ni === i) return;
    err = "";
    mq.run(mutKey.plugin(slot.instanceId), "move", () =>
      engineCommand("move_plugin", { instanceId: slot.instanceId, newIndex: ni }),
    );
  }
  function plugMenu(e: MouseEvent, slot: RackSlot) {
    const chain = track.plugins;
    const i = chain.findIndex((s) => s.instanceId === slot.instanceId);
    openMenu(e.clientX, e.clientY, `Plugin「${slot.name}」操作`, [
      { label: "編輯", disabled: isPh(slot), run: () => void openEditor(slot) },
      { label: "上移", disabled: i <= 0, run: () => plugMove(slot, "up") },
      { label: "下移", disabled: i >= chain.length - 1, run: () => plugMove(slot, "down") },
      { label: "移到最前", disabled: i <= 0, run: () => plugMove(slot, "first") },
      { label: "移到最後", disabled: i >= chain.length - 1, run: () => plugMove(slot, "last") },
      // 右鍵選單也提供與 row 控制相同的 Monitor Bypass action。
      ...(latencyEnabled
        ? [{ label: slot.monitorBypassed ? "取消 Monitor Bypass" : "Monitor Bypass", run: () => monitorBypass(slot) }]
        : []),
    ]);
  }
</script>

<div
  class="strip"
  class:out={isOutput}
  class:dropbefore={dropBefore}
  class:dropafter={dropAfter}
  class:dragging={dragging}
  class:unbound={needsRebind}
  draggable="true"
  data-track-id={track.trackId}
  role="listitem"
  aria-label="軌道 {track.name}"
  data-tooltip="拖曳軌道空白區調整順序；雙擊軌道名稱可重新命名；按右鍵開啟排序選單。"
  oncontextmenu={(e) => {
    if ((e.target as HTMLElement).closest("input, select, button, dialog")) return;
    e.preventDefault();
    trackMenu(e);
  }}
>
  <div class="head">
    <input
      type="color"
      class="swatch"
      value={cssColor(track.color)}
      data-tooltip="設定此軌道的識別色。"
      aria-label="軌道 {track.name} 的顏色"
      onchange={(e) => setColor(e.currentTarget.value)}
    />
    {#if editing}
      <input
        class="nameedit"
        bind:this={nameInput}
        bind:value={draft}
        draggable="false"
        aria-label="軌道名稱(Enter 套用、Esc 取消)"
        onkeydown={(e) => {
          if (e.key === "Enter") commitName();
          else if (e.key === "Escape") editing = false;
        }}
        onblur={commitName}
        ondblclick={(e) => e.stopPropagation()}
      />
    {:else}
      <!-- P2-P:語意控制(非無語意 span);雙擊/Enter 進入改名 -->
      <button
        class="name"
        data-tooltip="{track.name} — 雙擊重新命名；鍵盤操作時按 Enter。"
        aria-label="軌道名稱:{track.name}(雙擊改名)"
        ondblclick={startEdit}
        onclick={(e) => e.detail === 0 && startEdit()}
        >{track.name}</button
      >
    {/if}
    <span class="badge">{track.kind}</span>
    {#if latencyEnabled && isOutput && track.latencyPolicy === "lowLatency"}
      <span
        class="badge latency-low"
        data-tooltip="Low-Latency Output 不加入 Compensation Delay，因此不保證平行輸入路徑同步。"
        >LL</span
      >
    {/if}
    <span style="flex:1"></span>
    {#if track.systemRole}
      <!-- 系統輸出:每 session 恰好一條 monitor/stream,不可刪(engine 也擋) -->
      <span
        class="sysbadge"
        data-tooltip="系統{track.systemRole === "monitor" ? "監聽" : "串流"}輸出軌：可重新命名並調整裝置與路由；為維持固定輸出角色，無法刪除。"
        >系統</span
      >
    {:else}
      <button
        class="mini danger del"
        onclick={removeTrack}
        aria-label="刪除軌道 {track.name}(會先確認)"
        data-tooltip="刪除此軌道；執行前會要求確認。"
        >×</button
      >
    {/if}
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
      {#if needsRebind}
        <!-- P1-C:session 恢復後未綁定(engine 不猜 PID)→ 明確選擇程式 -->
        <span
          class="dim unboundtxt"
          data-tooltip={track.error ?? "Session 已恢復，但尚未指定捕捉程序。請手動重新綁定；引擎不會依名稱自動推測 PID。"}
          >未綁定程序</span
        >
        <button class="mini rebind" onclick={() => (pickerOpen = true)}
          >選擇程式…</button
        >
      {:else}
        <span
          class="boundname"
          data-tooltip={`目前捕捉 ${track.source?.name ?? "應用程式"}（PID ${track.source?.pid ?? 0}）。`}
          >{track.source?.name ?? `PID ${track.source?.pid}`}</span
        >
        <button
          class="mini"
          onclick={() => (pickerOpen = true)}
          data-tooltip="重新選擇此軌道要捕捉的程序；清單僅顯示目前正在輸出音訊的應用程式。"
          >更換</button
        >
        <button
          class="mini danger"
          onclick={clearApp}
          data-tooltip="解除目前的程序綁定；軌道與 plugin chain 會保留，並停止輸出音訊。"
          >解除</button
        >
      {/if}
    {:else if track.kind === "fx"}
      <span
        class="lbl dim"
        data-tooltip="FX 軌不直接擷取應用程式；請將上游軌道的輸出路由至此軌，作為 insert 效果鏈。"
        >insert · 無輸入</span
      >
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
        data-tooltip={track.systemRole === "monitor"
          ? "指定監聽輸出的 ASIO channel pair。"
          : "指定串流輸出的 WASAPI 裝置，例如 VB-CABLE 等虛擬音訊端點。"}
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
      {#if latencyEnabled}
        <select
          class="latency-policy"
          value={track.latencyPolicy ?? "fullPdc"}
          disabled={pendingLatencyPolicy}
          onchange={(e) => void setLatencyPolicy(
            e.currentTarget.value as "fullPdc" | "lowLatency",
            e.currentTarget,
          )}
          aria-label="輸出軌 {track.name} 的延遲政策"
          data-tooltip="Full PDC 會對齊匯流分支；Low Latency 不加入 Compensation Delay，且不保證平行路徑同步。"
        >
          <option value="fullPdc">Full PDC</option>
          <option value="lowLatency">Low Latency</option>
        </select>
      {/if}
    {/if}
  </div>

  <button
    class="destsbtn"
    onclick={() => destDlg?.showModal()}
    data-tooltip="設定此軌道的輸出路由；勾選目的地後立即套用。"
  >
    輸出到 ({shownDests.length}){destsLocal !== null ? " …" : ""}
  </button>

  <div class="lower">
  <div class="vstcol">
  <div class="vst" bind:this={vstBox} style={boxH !== null ? `flex:0 0 auto; height:${boxH}px` : ""}>
    <div class="vsthead">
      <span>VST 機架 ({track.plugins.length})</span>
    </div>
    <div class="vstlist">
      {#each track.plugins as s, i (s.instanceId)}
        <div
          class="plug"
          class:placeholder={isPh(s)}
          draggable="true"
          class:dragging={plugDrag === s.instanceId}
          class:dropbefore={plugDropAt === i}
          class:dropafter={plugDropAt === i + 1 && plugDropAt === track.plugins.length}
          role="listitem"
          aria-label="plugin {s.name}"
          ondragstart={(e) => onPlugDragStart(e, s)}
          ondragover={(e) => onPlugDragOver(e, i)}
          ondrop={onPlugDrop}
          ondragend={onPlugDragEnd}
          oncontextmenu={(e) => {
            if ((e.target as HTMLElement).closest("button")) return;
            e.preventDefault();
            plugMenu(e, s);
          }}
          data-tooltip={isPh(s) ? undefined : "拖曳調整 plugin chain 順序；按右鍵開啟操作選單。"}
        >
          {#if isPh(s)}
            <!-- missing/broken:原鏈位保留,不參與 DSP;提供重試/重新定位/移除 -->
            <div class="plugtitle">
              <span class="phmark" data-tooltip={`Plugin 載入失敗：${s.loadError ?? "原因未提供"}`}>⚠</span>
              <span
                class="plugname phname"
                data-tooltip={`Plugin 檔案：\n${s.pluginPath}\n\n載入錯誤：${s.loadError ?? "原因未提供"}`}
              >
                <span class="phwhy">{phLabel(s)}</span>
                {s.name || basename(s.pluginPath)}
                {#if s.loadError}<span class="pherr">{s.loadError}</span>{/if}
              </span>
            </div>
            <div class="plugactions placeholder-actions">
              <button
                class="mini"
                onclick={() => retryPlugin(s)}
                data-tooltip="使用原始檔案路徑重新載入此 plugin。"
                >重試</button
              >
              <button
                class="mini"
                onclick={() => relocatePlugin(s)}
                data-tooltip="指定替代的 plugin 檔案，並嘗試恢復此插槽。"
                >定位</button
              >
              <button
                class="mini plugicon danger del"
                onclick={() => removePlugin(s.instanceId)}
                aria-label="移除 plugin {s.name}(會先確認)"
                data-tooltip="從效果鏈移除此 plugin；執行前會要求確認。">×</button
              >
            </div>
          {:else}
            <span
              class="plugname"
              role="button"
              tabindex="0"
              data-tooltip="{s.name} — 單擊開啟 plugin 操作介面；按右鍵可開啟操作選單。"
              onclick={() => openEditor(s)}
              onkeydown={(e) => {
                if (e.key === "Enter" || e.key === " ") {
                  e.preventDefault();
                  void openEditor(s);
                }
              }}>{s.name}</span
            >
            <div class="plugactions">
              <button
                class="mini plugicon power"
                class:off={s.bypassed}
                aria-pressed={!s.bypassed}
                aria-label={s.bypassed ? `${s.name} bypass 中(點此啟用)` : `${s.name} 啟用中(點此 bypass)`}
                onclick={() => bypass(s)}
                data-tooltip={s.bypassed
                  ? "此 plugin 目前為 bypass；按下可恢復處理。"
                  : "此 plugin 目前正在處理音訊；按下可切換為 bypass。"}
              >
                <img class="picon" src={s.bypassed ? powerOff : powerOn} alt="" draggable="false" />
              </button>
              {#if latencyEnabled}
                <button
                  class="mini plugicon monitor-bypass"
                  class:on={s.monitorBypassed}
                  class:pending={isMonitorBypassPending(monitorBypassTransactions, s.instanceId)}
                  disabled={isMonitorBypassPending(monitorBypassTransactions, s.instanceId)}
                  onclick={() => void monitorBypass(s)}
                  aria-pressed={s.monitorBypassed ?? false}
                  aria-label={s.monitorBypassed
                    ? `取消 ${s.name} 的 Monitor Bypass`
                    : `啟用 ${s.name} 的 Monitor Bypass`}
                  data-tooltip={isMonitorBypassPending(monitorBypassTransactions, s.instanceId)
                    ? "正在建立或同步 Monitor Shadow；engine 確認前不改變目前狀態。"
                    : s.monitorBypassed
                    ? "Monitor Bypass 已啟用：Low-Latency Outputs 略過此 plugin；右鍵選單也可取消。"
                    : "只讓 Low-Latency Outputs 略過此 plugin；Stream 的完整處理不受影響，右鍵選單也可切換。"}
                  >{isMonitorBypassPending(monitorBypassTransactions, s.instanceId) ? "…" : "M"}</button
                >
              {/if}
              <button
                class="mini plugicon danger del"
                onclick={() => removePlugin(s.instanceId)}
                aria-label="移除 plugin {s.name}(會先確認)"
                data-tooltip="從效果鏈移除此 plugin；執行前會要求確認。">×</button
              >
            </div>
          {/if}
        </div>
      {:else}
        <span class="dim">無插件</span>
      {/each}
    </div>
    <div class="vstfoot">
      <button
        class="mini add"
        onclick={() => scanDlg?.showModal()}
        data-tooltip="開啟共用的 VST plugin 清單；不會自動重新掃描。"
        >＋ 加入</button
      >
    </div>
  </div>
  <!-- P2-P:resize grip = button(可聚焦、Enter = 還原高度;拖曳調整) -->
  <button
    type="button"
    class="vstgrip"
    aria-label="調整插件清單高度(拖曳;雙擊或 Enter 還原)"
    onpointerdown={(e) => {
      // button 的 pointerdown 預設行為(焦點/後續 click)不影響拖曳
      onGripDown(e);
    }}
    ondblclick={onGripDbl}
    data-tooltip="拖曳調整 plugin 區域高度；雙擊或按 Enter 還原預設高度。"
  ></button>
  </div>

  <dialog bind:this={scanDlg} class="scanlistdlg">
    <div class="cardhead dialog-head">
      <span class="dialog-title">VST 插件列表 — 加入「{track.name}」</span>
      <button
        class="dialog-close"
        type="button"
        aria-label="關閉 plugin 選擇器"
        onclick={() => scanDlg?.close()}
        data-tooltip="關閉 plugin 選擇器，不加入任何項目。">×</button
      >
    </div>
    {#if scanRunning}
      <div class="scanlive">
        <span class="dim"
          >掃描中{scanProgress
            ? ` ${scanProgress.done}/${scanProgress.total}`
            : "…"}(背景執行,不擋操作)</span
        >
        <button class="mini" onclick={onCancelScan}>取消</button>
      </div>
    {:else if scanNotice}
      <p class="err mono">{scanNotice}</p>
    {/if}
    {#if scanModules.length === 0 && !scanRunning}
      <p class="dim">尚無 VST 清單 — 請按頂欄「掃描 VST」</p>
    {:else}
      <div class="scanlist">
        {#each scanModules as m (m.path)}
          <div class="mod">
            <div class="modpath mono" data-tooltip={`Plugin 模組：\n${m.path}`}>{basename(m.path)}</div>
            <div class="classes">
              {#each m.classes as c (c.uid)}
                <button
                  class="mini"
                  onclick={() => addPlugin(m.path, c.uid)}
                  data-tooltip={`${c.vendor || "未知廠牌"} · ${c.version || "版本未提供"}`}
                  >{c.name}</button
                >
              {/each}
            </div>
          </div>
        {/each}
      </div>
      {#if scanFailed.length > 0}
        <details class="quarantine">
          <summary class="dim">無法載入({scanFailed.length})— 已隔離</summary>
          {#each scanFailed as f (f.path)}
            <div class="modpath mono" data-tooltip={`掃描失敗：${f.error}\n${f.path}`}>{basename(f.path)}:{f.error}</div>
          {/each}
        </details>
      {/if}
    {/if}
  </dialog>

  <dialog bind:this={destDlg} class="destlistdlg">
    <div class="cardhead dialog-head">
      <span class="dialog-title">輸出到 — 「{track.name}」</span>
      <button
        class="dialog-close"
        type="button"
        aria-label="關閉輸出路由設定"
        onclick={() => destDlg?.close()}
        data-tooltip="關閉輸出路由設定。">×</button
      >
    </div>
    <div class="destlist">
      {#each tracks.filter((t) => t.trackId !== track.trackId) as t (t.trackId)}
        <label class="dest">
          <input
            type="checkbox"
            checked={shownDests.includes(t.trackId)}
            onchange={(e) => toggleDest(t.trackId, e.currentTarget.checked)}
          />
          <span class="dot" style="background:{cssColor(t.color)}" aria-hidden="true"></span>
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
        aria-pressed={track.mute}
        aria-label={track.mute ? `靜音中(點此取消)` : `靜音 ${track.name}`}
        onclick={() => setMute(!track.mute)}
        data-tooltip={track.mute ? "此軌目前已靜音；按下可取消靜音。" : "將此軌靜音。"}
        >{track.mute ? "M✓" : "M"}</button
      >
      <input
        type="range"
        min="0"
        max="1.5"
        step="0.01"
        value={shownGain}
        aria-label="{track.name} 音量({Math.round(shownGain * 100)}%;滾輪微調,每格 2%;Ctrl+點擊 = 恢復 100%)"
        oninput={onGainInput}
        onchange={onGainChange}
        onclick={onGainClick}
        onwheel={onGainWheel}
        onpointerdown={onFaderDown}
        onpointermove={onFaderMove}
        onpointerup={onFaderUp}
        onpointercancel={onFaderUp}
        data-tooltip="調整軌道音量；滾輪每格微調 2%；Ctrl + 按一下還原為 100%。目前為 {Math.round(shownGain * 100)}%。"
      />
      <span class="gain mono">{Math.round(shownGain * 100)}%</span>
    </div>
    <div class="meterwrap">
      {#if metered}
        <MeterCanvas strip={stripOfTrack(track.trackId, strips)} />
      {:else}
        <!-- P1-H:telemetry 預算外 = 錶不可用(非靜音);明確顯示狀態 -->
        <div
          class="nometer"
          data-tooltip="已達即時電平錶顯示上限（64 軌）；音訊處理不受影響，但此軌不顯示電平。"
          ><span>無<br />錶</span></div
        >
      {/if}
    </div>
  </div>
  </div>

  <div class="colorbar" style="background:{cssColor(track.color)}"></div>

  {#if err || track.error}
    <!-- P1-O:錯誤文字可選取複製;tooltip 帶完整原文 -->
    <p class="err mono" data-tooltip={`完整錯誤訊息：\n${(err || track.error) ?? ""}`} role="alert">{err || track.error}</p>
  {/if}

  {#if pickerOpen}
    <AppPicker
      trackName={track.name}
      savedName={track.source?.name ?? null}
      onPick={pickApp}
      onClose={() => (pickerOpen = false)}
    />
  {/if}
  <ConfirmDialog confirm={confirmBox} onAnswer={answerConfirm} />

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
    user-select: none; /* 拖曳區禁選(全域已開放選取;錯誤文字 .err 例外) */
  }
  .strip .err {
    user-select: text;
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
    /* P2-P:改名入口改為 button(語意控制);視覺維持纯文字 */
    background: none;
    border: none;
    padding: 0;
    font: inherit;
    font-size: 13px;
    font-weight: 600;
    color: var(--text);
    white-space: nowrap;
    overflow: hidden;
    text-overflow: ellipsis;
    min-width: 0;
    flex: 1;
    text-align: left;
  }
  .name:hover {
    color: var(--accent);
    border: none;
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
  /* VST 常駐 box:預設自適應內容(超出即內捲),拖底部把手可拉長 */
  .vst {
    flex: 0 1 auto;
    min-height: 72px;
    max-height: 100%;
    display: flex;
    flex-direction: column;
    font-size: 12px;
    background: var(--bg);
    border: 1px solid var(--border);
    border-radius: 6px;
    overflow: hidden;
  }
  .vsthead {
    flex: none;
    display: flex;
    align-items: center;
    gap: 6px;
    padding: 4px 8px;
    color: var(--text-dim);
    background: var(--bg-raised);
    border-bottom: 1px solid var(--border);
    user-select: none;
  }
  .vstlist {
    flex: 1 1 auto;
    min-height: 0;
    overflow-y: auto;
    padding: 4px;
    display: flex;
    flex-direction: column;
    gap: 2px;
  }
  .vstfoot {
    flex: none;
    display: flex;
    padding: 4px 6px;
    border-top: 1px solid var(--border);
  }
  /* 底部高度把手 */
  .vstgrip {
    flex: none;
    position: relative;
    height: 7px;
    border-radius: 4px;
    background: var(--bg-raised);
    border: 1px solid var(--border);
    cursor: ns-resize;
    user-select: none;
    touch-action: none;
  }
  .vstgrip::after {
    content: "";
    position: absolute;
    left: 50%;
    top: 50%;
    width: 28px;
    height: 2px;
    transform: translate(-50%, -50%);
    border-radius: 1px;
    background: var(--border);
  }
  .vstgrip:hover {
    border-color: var(--accent);
  }
  .vstgrip:hover::after {
    background: var(--accent);
  }
  .plug {
    display: flex;
    flex-direction: column;
    align-items: stretch;
    gap: 1px;
    padding: 3px 4px;
    background: var(--bg-raised);
    border: 1px solid var(--border);
    border-radius: 6px;
  }
  .plugtitle {
    display: flex;
    align-items: flex-start;
    min-width: 0;
  }
  .plugactions {
    display: flex;
    align-items: center;
    gap: 4px;
    height: 22px;
    line-height: 1;
  }
  .placeholder-actions {
    height: auto;
    min-height: 22px;
  }
  /* placeholder(missing/broken):整列黯淡、警示色標記,功能按鈕照常 */
  .plug.placeholder {
    opacity: 0.55;
    background: color-mix(in srgb, var(--warn) 8%, transparent);
    border-left: 2px solid var(--warn);
  }
  .phmark {
    color: var(--warn);
    flex-shrink: 0;
    font-size: 11px;
  }
  .phname {
    display: flex;
    flex-direction: column;
    gap: 0;
  }
  .phwhy {
    color: var(--warn);
    font-size: 10px;
    font-weight: 700;
  }
  .pherr {
    color: var(--text-dim);
    font-size: 10px;
    white-space: nowrap;
    overflow: hidden;
    text-overflow: ellipsis;
  }
  .sysbadge {
    font-size: 10px;
    color: var(--text-dim);
    border: 1px dashed var(--border);
    border-radius: 4px;
    padding: 0 5px;
    flex-shrink: 0;
  }
  .scanlive {
    display: flex;
    align-items: center;
    gap: 8px;
  }
  .quarantine {
    font-size: 11px;
    display: flex;
    flex-direction: column;
    gap: 2px;
  }
  .quarantine summary {
    cursor: pointer;
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
    justify-content: center;
  }
  .plug .mini.plugicon {
    width: 22px;
    min-width: 22px;
    height: 22px;
    min-height: 22px;
    padding: 0;
    background: transparent;
    border: 0;
    border-radius: 0;
  }
  .plug .mini.plugicon:hover {
    opacity: 0.75;
  }
  .plug .mini.plugicon:focus-visible {
    outline: 1px solid var(--accent);
    outline-offset: -1px;
  }
  .picon {
    width: 12px;
    height: 12px;
    pointer-events: none;
  }
  .plugname {
    color: var(--text);
    width: 100%;
    padding: 0 2px;
    cursor: default;
    font-size: 12px;
    line-height: 18px;
    white-space: nowrap;
    overflow: hidden;
    text-overflow: ellipsis;
    min-width: 0;
  }
  .plugname:hover {
    color: var(--accent);
  }
  .plug:not(.placeholder) .plugname {
    cursor: pointer;
  }
  /* P1-C:needsRebind 黯淡提示(軌道保留、安全靜音) */
  .strip.unbound {
    opacity: 0.75;
  }
  .unboundtxt {
    font-size: 12px;
    flex: 1;
    min-width: 0;
    white-space: nowrap;
    overflow: hidden;
    text-overflow: ellipsis;
  }
  .boundname {
    font-size: 12px;
    flex: 1;
    min-width: 0;
    white-space: nowrap;
    overflow: hidden;
    text-overflow: ellipsis;
  }
  .rebind {
    border-color: var(--warn);
    color: var(--warn);
  }
  /* P1-H:telemetry 預算外的「無錶」狀態(非靜音) */
  .nometer {
    flex: 1;
    display: flex;
    align-items: center;
    justify-content: center;
    background: repeating-linear-gradient(
      45deg,
      var(--bg) 0 6px,
      var(--bg-raised) 6px 12px
    );
    border: 1px solid var(--border);
    border-radius: 4px;
    color: var(--text-dim);
    font-size: 10px;
    text-align: center;
    line-height: 1.3;
    user-select: none;
  }
  .mini {
    padding: 1px 6px;
    font-size: 11px;
    line-height: 1.4;
  }
  /* P2-M:破壞性/常誤點操作的 hit target 擴大 */
  .mini.del {
    min-width: 24px;
    min-height: 22px;
    padding: 0 6px;
    font-size: 13px;
  }
  .mute {
    min-width: 30px;
    min-height: 26px;
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
    gap: 2px;
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
    /* P1-K:矮視窗不裁掉關閉鈕 —— 85vh 上限 + 內容捲動 */
    max-height: 85vh;
    overflow-y: auto;
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
  .scanlistdlg p {
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
