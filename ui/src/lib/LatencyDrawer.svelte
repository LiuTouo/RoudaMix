<script lang="ts">
  import { untrack } from "svelte";
  import { engineCommand } from "./protocol-commands.generated";
  import { errorText } from "./errors";
  import { samplesToMs } from "./format";
  import type { EngineStatus, LatencyReport, MetersFrame, RackSlot, Track } from "./types";

  let { open, status, meters, onClose }: { open: boolean; status: EngineStatus | null; meters: MetersFrame | null; onClose: () => void } =
    $props();
  let report = $state<LatencyReport | null>(null);
  let loading = $state(false);
  let error = $state("");
  let loadedGeneration = -1;
  let sortBy = $state<"chain" | "latency" | "load">("chain");
  let loadView = $state<Record<string, { ewma: number; peak: number }>>({});
  const loadHistory = new Map<string, number[]>();

  const rate = $derived(status?.sampleRate ?? 0);
  const ms = (samples: number | null | undefined) => samplesToMs(samples, rate);
  const trackName = (id: number) =>
    (report?.tracks ?? status?.tracks ?? []).find((track) => track.trackId === id)?.name ?? `#${id}`;
  const loadKey = (instanceId: number, variant: number) => `${instanceId}:${variant}`;
  const loadText = (instanceId: number, variant: number) => {
    const value = loadView[loadKey(instanceId, variant)];
    return value ? `${(value.ewma * 100).toFixed(1)}% / peak ${(value.peak * 100).toFixed(1)}%` : "—";
  };
  const stateText = (state: RackSlot["runtimeState"], fallback: string) => {
    if (state === "preparing") return "Preparing";
    if (state === "degraded") return "Degraded";
    if (state === "suspended") return "Suspended";
    return fallback;
  };
  const affectedOutputs = (trackId: number) => {
    if (!report) return "—";
    const byId = new Map(report.tracks.map((track) => [track.trackId, track]));
    const names = new Set<string>();
    const seen = new Set<number>();
    const visit = (id: number) => {
      if (seen.has(id)) return;
      seen.add(id);
      const track = byId.get(id);
      if (!track) return;
      if (track.kind === "output") names.add(track.name);
      for (const dest of track.dests) visit(dest);
    };
    visit(trackId);
    return names.size ? [...names].join("、") : "未連接輸出";
  };
  const sortedPlugins = (track: Track) => {
    if (sortBy === "chain") return track.plugins;
    return [...track.plugins].sort((a, b) => {
      if (sortBy === "latency")
        return (b.effectiveLatencySamples ?? -1) - (a.effectiveLatencySamples ?? -1);
      return (loadView[loadKey(b.instanceId, 0)]?.ewma ?? -1) -
             (loadView[loadKey(a.instanceId, 0)]?.ewma ?? -1);
    });
  };

  async function refresh(generation: number) {
    loading = true;
    error = "";
    try {
      const result = await engineCommand("get_latency_report", {});
      report = result.report;
      loadedGeneration = report.generation;
    } catch (e) {
      error = errorText(e);
      loadedGeneration = generation;
    } finally {
      loading = false;
    }
  }

  $effect(() => {
    const generation = status?.latencyGeneration ?? 0;
    if (open && generation !== loadedGeneration && !loading) void refresh(generation);
  });
  $effect(() => {
    if (!meters?.pluginLoads) return;
    const next = { ...untrack(() => loadView) };
    for (const load of meters.pluginLoads) {
      const key = loadKey(load.instanceId, load.variant);
      const previous = next[key]?.ewma ?? load.processLoad;
      const ewma = previous + (load.processLoad - previous) / 30;
      const history = loadHistory.get(key) ?? [];
      history.push(load.processLoad);
      if (history.length > 150) history.shift();
      loadHistory.set(key, history);
      next[key] = { ewma, peak: Math.max(...history) };
    }
    loadView = next;
  });
</script>

{#if open}
  <aside class="latency-drawer" aria-label="Plugin 延遲與 Process Load 明細">
    <div class="drawer-head">
      <strong>Plugin 延遲與 Process Load</strong>
      <button class="dialog-close" aria-label="關閉延遲明細" data-tooltip="關閉延遲與效能明細。" onclick={onClose}>×</button>
    </div>
    {#if loading && !report}<p class="dim">載入中…</p>{/if}
    {#if error}<p class="err">{error}</p>{/if}
    {#if report}
      {#if !report.ok}<p class="err">PDC plan 無法套用：{report.error}</p>{/if}
      <section class="latency-output-grid">
        {#each report.outputs as output (output.trackId)}
          <article class="latency-output">
            <strong>{trackName(output.trackId)}</strong>
            <span>Total Plugin Delay：{ms(output.totalPluginDelaySamples)} ms</span>
            <span>Compensation Delay：{ms(output.compensationDelaySamples)} ms</span>
            <small
              data-tooltip={output.synchronized
                ? "Full PDC 會在每個匯流點加入 Compensation Delay，使平行路徑 sample-exact 對齊。"
                : "Low-Latency Output 不加入 Compensation Delay，因此平行路徑不保證同步。"}
              >{output.synchronized ? "Full PDC · sample-exact" : "Low Latency · 不保證路徑同步"}</small
            >
          </article>
        {/each}
      </section>
      <label class="drawer-sort">
        排序
        <select
          bind:value={sortBy}
          aria-label="Plugin 延遲明細排序"
          data-tooltip="排序只改變各 Track 內的 instance 順序，不拆散 Track 群組。"
        >
          <option value="chain">Chain 順序</option>
          <option value="latency">Plugin Latency</option>
          <option value="load">Process Load</option>
        </select>
      </label>
      <div class="latency-table" role="table" aria-label="Plugin instance 延遲明細">
        <div class="latency-row latency-header" role="row">
          <span>Track / Plugin</span><span>Latency</span><span>Process Load</span><span>狀態</span>
        </div>
        {#each report.tracks as track (track.trackId)}
          {#each sortedPlugins(track) as plugin (plugin.instanceId)}
            <div class="latency-row" role="row">
              <span>{track.name} / {plugin.name}<small class="latency-path">輸出：{affectedOutputs(track.trackId)}</small></span>
              <span>{ms(plugin.latencySamples)} ms</span>
              <span>{!status?.running || plugin.bypassed || plugin.availability !== "ok" || plugin.runtimeState === "suspended"
                ? "—"
                : loadText(plugin.instanceId, 0)}</span>
              <span>{stateText(plugin.runtimeState, plugin.bypassed ? "Bypass" : plugin.monitorBypassed ? "Monitor Bypass" : "Active")}</span>
            </div>
            {#if plugin.monitorLatencySamples !== null && plugin.monitorLatencySamples !== undefined}
              <div class="latency-row shadow" role="row">
                <span>{track.name} / {plugin.name} · Monitor Shadow</span>
                <span>{ms(plugin.monitorLatencySamples)} ms</span>
                <span>{!status?.running || plugin.monitorState !== "active" ? "—" : loadText(plugin.instanceId, 1)}</span>
                <span>{stateText(plugin.monitorState, "Shadow")}</span>
              </div>
            {/if}
          {/each}
        {/each}
      </div>
    {/if}
  </aside>
{/if}
