<script lang="ts">
  // 軌條:輸入/輸出軌共用。由上而下 = 名稱列 → 來源/輸出裝置 → 目的地多選 →
  // VST 展開列表(電源=bypass、雙擊=原生 GUI、上→下=訊號序)→ 推桿+靜音 →
  // 錶 → 底部顏色條。engine 溝通自含(ipc 直呼),App 只餵狀態。
  import MeterCanvas from "./MeterCanvas.svelte";
  import { engineCommand } from "./ipc";
  import { cssColor, parseColor, stripOfTrack } from "./tracks";
  import type {
    AudioApp,
    DeviceInfo,
    MeterStrip,
    RackSlot,
    ScanModule,
    Track,
  } from "./types";

  let {
    track,
    tracks,
    devices,
    selectedDeviceKey,
    strips,
  }: {
    track: Track;
    tracks: Track[];
    devices: DeviceInfo[];
    selectedDeviceKey: string;
    strips: MeterStrip[] | undefined;
  } = $props();

  let err = $state("");
  // 掃描(各軌自含:開了才掃,清單不共用)
  let scanning = $state(false);
  let scanned = $state(false);
  let modules = $state<ScanModule[]>([]);
  // app 程序清單(focus 時拉一次,保持常新)
  let apps = $state<AudioApp[]>([]);

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
      } else {
        await engineCommand("track_set_output", {
          trackId: track.trackId,
          output: { type: "asioOut", channel: Number(value) },
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

  async function setGain(v: number) {
    err = "";
    try {
      await engineCommand("track_set", { trackId: track.trackId, gain: v });
    } catch (e) {
      err = String(e);
    }
  }

  async function setMute(mute: boolean) {
    err = "";
    try {
      await engineCommand("track_set", { trackId: track.trackId, mute });
    } catch (e) {
      err = String(e);
    }
  }

  async function moveTrack(delta: number) {
    // 同 kind 群組內上下移(track_move newIndex = 群組內位置)
    const group = tracks.filter((t) => t.kind === track.kind);
    const i = group.findIndex((t) => t.trackId === track.trackId);
    const ni = i + delta;
    if (ni < 0 || ni >= group.length) return;
    err = "";
    try {
      await engineCommand("track_move", { trackId: track.trackId, newIndex: ni });
    } catch (e) {
      err = String(e);
    }
  }

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

  async function movePlugin(id: number, delta: number) {
    const i = track.plugins.findIndex((s) => s.instanceId === id);
    const ni = i + delta;
    if (i < 0 || ni < 0 || ni >= track.plugins.length) return;
    err = "";
    try {
      await engineCommand("move_plugin", { instanceId: id, newIndex: ni });
    } catch (e) {
      err = String(e);
    }
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
      scanned = true;
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
      scanned = false;
    } catch (e) {
      err = String(e);
    }
  }
</script>

<div class="strip" class:out={isOutput}>
  <div class="head">
    <input
      type="color"
      class="swatch"
      value={cssColor(track.color)}
      title="軌道顏色"
      onchange={(e) => setColor(e.currentTarget.value)}
    />
    <span class="name" title={track.name}>{track.name}</span>
    <span class="badge">{track.kind}</span>
    <span style="flex:1"></span>
    <button class="mini" onclick={() => moveTrack(-1)} title="上移">▲</button>
    <button class="mini" onclick={() => moveTrack(1)} title="下移">▼</button>
    <button class="mini danger" onclick={removeTrack} title="刪除軌道">×</button>
  </div>

  <div class="row">
    {#if track.kind === "audio"}
      <span class="lbl">輸入</span>
      <select
        value={track.source?.type === "asioIn" ? String(track.source.channel) : ""}
        onchange={(e) => setSource(e.currentTarget.value)}
        disabled={!dev}
      >
        <option value="">(無)</option>
        {#each pairOptions(dev?.inputNames ?? [], "in") as o (o.value)}
          <option value={o.value}>{o.label}</option>
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
          <option value={a.pid}>{a.name}</option>
        {/each}
      </select>
    {:else if track.kind === "fx"}
      <span class="lbl dim" title="FX 軌:上游軌把輸出指到這裡(insert 型)">insert · 無輸入</span>
    {:else}
      <span class="lbl">輸出裝置</span>
      <select
        value={track.output?.type === "asioOut" ? String(track.output.channel) : ""}
        onchange={(e) => setOutput(e.currentTarget.value)}
        disabled={!dev}
      >
        <option value="">(無)</option>
        {#each pairOptions(dev?.outputNames ?? [], "out") as o (o.value)}
          <option value={o.value}>{o.label}</option>
        {/each}
      </select>
    {/if}
  </div>

  <details class="dests">
    <summary>輸出到 ({track.dests.length})</summary>
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
  </details>

  <details class="vst">
    <summary>VST ({track.plugins.length})</summary>
    {#each track.plugins as s, i (s.instanceId)}
      <div class="plug">
        <button
          class="mini power"
          class:off={s.bypassed}
          onclick={() => bypass(s)}
          title={s.bypassed ? "Bypassed(點此啟用)" : "啟用中(點此 Bypass)"}
          >⏻</button
        >
        <button
          class="plugname"
          title="雙擊開啟 plugin 原生 GUI"
          ondblclick={() => openEditor(s)}
          onclick={(e) => {
            if (e.detail === 1) void e; // 單擊不動作(雙擊才開)
          }}
          >{s.name}</button
        >
        <span style="flex:1"></span>
        <span class="idx mono">{i + 1}</span>
        <button class="mini" onclick={() => movePlugin(s.instanceId, -1)} title="上移">▲</button>
        <button class="mini" onclick={() => movePlugin(s.instanceId, 1)} title="下移">▼</button>
        <button class="mini danger" onclick={() => removePlugin(s.instanceId)} title="移除">×</button>
      </div>
    {:else}
      <span class="dim">無插件</span>
    {/each}
    {#if !scanning}
      <button class="mini add" onclick={scan}>＋ 掃描加入</button>
    {:else}
      <span class="dim">掃描中…</span>
    {/if}
    {#if scanned && modules.length > 0}
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
    {:else if scanned && modules.length === 0}
      <span class="dim">找不到 VST3</span>
    {/if}
  </details>

  <div class="row fader">
    <button class="mini mute" class:on={track.mute} onclick={() => setMute(!track.mute)} title="靜音"
      >M</button
    >
    <input
      type="range"
      min="0"
      max="1.5"
      step="0.01"
      value={track.gain}
      onchange={(e) => setGain(Number(e.currentTarget.value))}
      title={`音量 ${Math.round(track.gain * 100)}%`}
    />
    <span class="gain mono">{Math.round(track.gain * 100)}%</span>
  </div>

  <MeterCanvas strip={stripOfTrack(track.trackId, strips)} height={26} />

  <div class="colorbar" style="background:{cssColor(track.color)}"></div>

  {#if err || track.error}
    <p class="err mono">{err || track.error}</p>
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
    width: 100%;
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
  .dests {
    display: flex;
    flex-direction: column;
    gap: 2px;
  }
  .dest {
    display: flex;
    align-items: center;
    gap: 6px;
    padding-left: 10px;
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
  }
  .plugname {
    background: none;
    border: none;
    color: var(--text);
    padding: 2px 4px;
    text-align: left;
    cursor: pointer;
    font-size: 12px;
    white-space: nowrap;
    overflow: hidden;
    text-overflow: ellipsis;
    max-width: 150px;
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
  .fader input[type="range"] {
    flex: 1;
    min-width: 0;
    accent-color: var(--accent);
  }
  .gain {
    width: 38px;
    text-align: right;
    font-size: 11px;
    color: var(--text-dim);
  }
  .colorbar {
    height: 4px;
    border-radius: 2px;
    margin: 0 -10px; /* 吃掉 padding,通欄 */
  }
  .scanlist {
    display: flex;
    flex-direction: column;
    gap: 4px;
    max-height: 160px;
    overflow-y: auto;
    padding-left: 10px;
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
</style>
