<script lang="ts">
  // THROWAWAY: three Inputs / Outputs container studies on /?prototype=containers&variant=A/B/C.
  // Standalone rendering avoids App / TrackStrip's native mutations; all controls are in-memory.
  import PrototypeSwitcher from './lib/PrototypeSwitcher.svelte';
  import { trackIcons } from './lib/icons';

  const variants = [
    { key: 'A', name: '左右雙容器', note: '輸入與輸出各自收進內凹底盤。保留完整直立音軌，最接近參考圖的容納感。' },
    { key: 'B', name: '共用機架分區', note: '一道外框收起整個混音台；Inputs / Outputs 以分艙標題與中央隔板保持邊界。' },
    { key: 'C', name: '上下分區', note: '先看來源，再看輸出。兩層獨立底盤，側邊標題與較短的直立音軌讓路由方向更清楚。' },
  ];
  let variant = $state(new URLSearchParams(location.search).get('variant') || 'A');
  const current = $derived(variants.find(option => option.key === variant) || variants[0]);
  const initialTracks = [
    { id: 'mic', name: '人聲', kind: 'AUDIO', icon: trackIcons.mic, color: '#628FB5', source: '麥克風 · Input 1', output: false, gain: -3, mute: false, bypass: false, monitor: true, stream: true, level: -15, plugins: ['Pro-Q 3', 'LA-2A Compressor'], latency: '64 spl' },
    { id: 'music', name: '音樂', kind: 'APP', icon: trackIcons.app, color: '#8276A6', source: 'Spotify · Stereo', output: false, gain: -8, mute: false, bypass: false, monitor: true, stream: true, level: -12, plugins: ['SSL Bus Compressor'], latency: '32 spl' },
    { id: 'desktop', name: '桌面', kind: 'AUDIO', icon: trackIcons.audio, color: '#628FB5', source: 'Desktop · Loopback', output: false, gain: -6, mute: true, bypass: false, monitor: false, stream: true, level: -19, plugins: [], latency: '0 spl' },
    { id: 'fx', name: '空間效果', kind: 'FX', icon: trackIcons.fx, color: '#69958C', source: 'FX Bus · 人聲傳送', output: false, gain: -12, mute: false, bypass: false, monitor: true, stream: true, level: -22, plugins: ['VintageVerb'], latency: '0 spl' },
    { id: 'monitor', name: '監聽', kind: 'MONITOR', icon: trackIcons.monitor, color: '#A18A64', source: 'Headphones · Out 1 / 2', output: true, gain: -3, mute: false, bypass: false, monitor: false, stream: false, level: -9, plugins: ['Pro-Q 3'], latency: '低延遲' },
    { id: 'stream', name: '串流', kind: 'STREAM', icon: trackIcons.stream, color: '#A18A64', source: 'Virtual Cable · Stereo', output: true, gain: -1, mute: false, bypass: false, monitor: false, stream: false, level: -11, plugins: ['Pro-L 2'], latency: '完整路徑對齊' },
  ];
  type DemoTrack = typeof initialTracks[number];
  let tracks = $state(structuredClone(initialTracks));
  const inputs = $derived(tracks.filter(track => !track.output));
  const outputs = $derived(tracks.filter(track => track.output));
</script>

<svelte:head><title>RoudaMix · Inputs / Outputs 容器原型</title></svelte:head>

{#snippet channel(track: DemoTrack)}
  <article class="study-channel" style:--track-color={track.color} aria-label={`${track.name} 模擬音軌`}>
    <header class="identity">
      <img src={track.icon.src} alt="" width="17" height="17" />
      <strong>{track.name}</strong><output for={`gain-${track.id}`}>{track.gain.toFixed(1)} <small>dB</small></output>
    </header>
    <div class="channel-controls">
      <div class="source-line"><span>{track.output ? '裝置' : '來源'}</span><span>{track.source}</span></div>
      <div class="route-line">
        {#if track.output}
          <span class="policy-label">{track.latency}</span><span class="route-count">OUTPUT</span>
        {:else}
          <span class="control-label">路由</span>
          <button class="route-toggle" aria-label={`${track.name} 傳送至監聽（模擬）`} aria-pressed={track.monitor} onclick={() => track.monitor = !track.monitor}><i aria-hidden="true"></i>監聽</button>
          <button class="route-toggle" aria-label={`${track.name} 傳送至串流（模擬）`} aria-pressed={track.stream} onclick={() => track.stream = !track.stream}><i aria-hidden="true"></i>串流</button>
        {/if}
      </div>
      <div class="channel-switches"><span class="kind-label">{track.kind}</span><button class="mute-toggle" aria-label={`${track.name} 靜音（模擬）`} data-tooltip="Mute · 僅變更模擬狀態" aria-pressed={track.mute} onclick={() => track.mute = !track.mute}>M</button><button class="bypass-toggle" aria-label={`${track.name} 效果器旁通（模擬）`} data-tooltip="Bypass · 僅變更模擬狀態" aria-pressed={track.bypass} onclick={() => track.bypass = !track.bypass}>B</button></div>
    </div>
    <div class="channel-body">
      <div class="level-section">
        <div class="vertical-controls">
          <div class="meter-pair" role="img" aria-label={`${track.name} 示意電平 ${track.mute ? '靜音' : `${track.level + track.gain} dBFS`}`}>
            {#each [0, 3] as offset}<div class="meter-slot"><i style:height={`${track.mute ? 0 : Math.max(0, Math.min(100, (60 + track.level + track.gain - offset) / 60 * 100))}%`}></i></div>{/each}
          </div>
          <div class="meter-scale" aria-hidden="true"><span>0</span><span>−12</span><span>−24</span><span>−48</span><span>−60</span></div>
          <input id={`gain-${track.id}`} class="vertical-fader" type="range" min="-60" max="12" step="0.5" bind:value={track.gain} aria-label={`${track.name} 增益（模擬）`} aria-valuetext={`${track.gain.toFixed(1)} dB`} />
        </div>
        <span class="meter-caption">{track.mute ? 'MUTED' : 'dBFS · 示意'}</span>
      </div>
      <section class="plugin-column" aria-label={`${track.name} 效果器示意`}>
        <div class="plugin-heading">INSERT FX <span>{String(track.plugins.length).padStart(2, '0')}</span></div>
        {#each track.plugins as plugin}<div class="plugin-row" class:bypassed={track.bypass}><i aria-hidden="true"></i><span>{plugin}</span></div>{/each}
        {#if track.plugins.length === 0}<div class="empty-plugin">無效果器</div>{/if}
        <div class="chain-state">{track.bypass ? 'BYPASSED' : 'CHAIN LATENCY'}<span>{track.bypass ? '0 spl' : track.output ? (track.id === 'stream' ? '128 spl' : '0 spl') : track.latency}</span></div>
      </section>
    </div>
    <footer class="channel-footer"><span>{track.mute ? '靜音中' : '模擬訊號'}</span><span>{track.output ? track.kind : 'STEREO'}</span></footer>
  </article>
{/snippet}

{#snippet group(output: boolean)}
  <section class="container-well" class:output-well={output} aria-label={output ? 'Outputs 輸出容器' : 'Inputs 輸入容器'}>
    <header class="well-heading"><div><span class="section-number">{output ? '02' : '01'}</span><h2>{output ? 'Outputs' : 'Inputs'}</h2><span class="count">{output ? outputs.length : inputs.length}</span></div><p>{output ? '輸出 / 監聽與串流' : '輸入 / 音訊、應用程式與 FX'}</p></header>
    <!-- svelte-ignore a11y_no_noninteractive_tabindex (Scrollable track lanes need keyboard access.) -->
    <div class="channel-lane" tabindex="0" role="region" aria-label={output ? '輸出音軌（窄螢幕可橫向捲動）' : '輸入音軌（窄螢幕可橫向捲動）'}>
      {#each output ? outputs : inputs as track (track.id)}{@render channel(track)}{/each}
    </div>
  </section>
{/snippet}

<div class="container-prototype">
  <header class="app-header"><div class="brand"><svg width="22" height="22" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" aria-hidden="true"><path d="M4 8v8M9 4v16M14 7v10M19 2v20" /></svg><strong>RoudaMix</strong></div><span class="session">Studio Session <span class="demo-label">DEMO</span></span><div class="telemetry">48 000 Hz <span>buf 256</span><span>Plugin load 12%</span></div><span class="mock-badge">MOCK · 不連接音訊</span></header>
  <main>
    <section class="study-heading"><div><div class="eyebrow">MIXER / CONTAINER STUDIES</div><h1>{current.key}<span>/</span>{current.name}</h1><p>{current.note}</p></div><button class="reset" onclick={() => tracks = structuredClone(initialTracks)}>重設模擬狀態</button></section>
    {#if current.key === 'A'}
      <div class="split-layout" data-variant="A">{@render group(false)}{@render group(true)}</div>
    {:else if current.key === 'B'}
      <section class="shared-rack" data-variant="B" aria-label="共用混音機架">
        <header class="rack-heading"><span><i aria-hidden="true"></i>ROUDAMIX / SIGNAL RACK</span><span>4 INPUTS → 2 OUTPUTS</span></header>
        <div class="rack-partitions">{@render group(false)}{@render group(true)}</div>
        <footer class="rack-footer"><span>INPUT STAGE</span><span>OUTPUT STAGE · MONITOR / STREAM</span></footer>
      </section>
    {:else}
      <div class="stacked-layout" data-variant="C">{@render group(false)}<div class="flow-label" aria-hidden="true">↓ ROUTING / 監聽 + 串流</div>{@render group(true)}</div>
    {/if}
    <div class="study-footnote"><span>模擬資料 · 電平為靜態示意，非即時量測</span><span>增益 / M / B / 路由可試用 · 切換保留狀態 · 重新整理即重設</span></div>
    <details class="state-panel"><summary>檢視完整模擬狀態 / {current.key}</summary><p>所有控制僅改變記憶體資料；不模擬音訊處理、路由加總或插件行為，不讀寫設定與檔案。</p><pre>{JSON.stringify({ variant: current.key, mode: 'mock / in-memory', tracks }, (key, value) => key === 'icon' ? undefined : value, 2)}</pre></details>
  </main>
  <PrototypeSwitcher {variants} bind:value={variant} />
</div>

<style>
  .container-prototype { min-height: 100%; background: var(--flat-1); color: var(--text); font-family: 'Segoe UI', system-ui, sans-serif; font-size: 12px; }
  .app-header { min-height: 46px; padding: 8px 20px; display: flex; gap: 24px; align-items: center; flex-wrap: wrap; background: var(--flat-0); border-bottom: 1px solid var(--border); }
  .brand { display: flex; gap: 9px; align-items: center; font-size: 16px; letter-spacing: -.4px; }
  .brand svg { color: var(--accent); }
  .session { border-left: 1px solid var(--border); padding-left: 24px; }
  .demo-label { margin-left: 7px; font: 9px var(--mono); color: var(--text-dim); border: 1px solid var(--border); padding: 2px 4px; }
  .telemetry { display: flex; gap: 18px; margin-left: auto; font: 11px var(--mono); color: var(--text-dim); }
  .mock-badge { color: var(--warn); font: 10px var(--mono); letter-spacing: .3px; }
  main { padding: 24px 20px 110px; max-width: 1900px; margin: auto; }
  .study-heading { display: flex; justify-content: space-between; align-items: center; gap: 20px; margin-bottom: 22px; }
  .eyebrow { font: 10px var(--mono); letter-spacing: 1.8px; color: var(--text-dim); }
  h1 { display: flex; align-items: center; gap: 12px; margin: 6px 0; font-size: 23px; font-weight: 500; letter-spacing: .5px; }
  h1 > span { font-weight: 300; color: var(--border); }
  .study-heading p { margin: 0; color: var(--text-dim); font-size: 12px; line-height: 1.7; }
  .reset { flex-shrink: 0; min-height: 34px; }
  .split-layout { display: grid; grid-template-columns: minmax(0, 2fr) minmax(0, 1fr); gap: 20px; }
  :global(#app .container-prototype .container-well) { min-width: 0; padding: 0 12px 12px; border: 1px solid #080a0c; border-radius: 12px; background: #090b0d; box-shadow: inset 0 3px 9px #000a, 0 1px 0 #ffffff0d; }
  .well-heading { padding: 14px 4px 12px; }
  .well-heading > div { display: flex; align-items: center; gap: 9px; }
  .section-number { font: 10px var(--mono); color: var(--text-dim); }
  h2 { margin: 0; font-size: 14px; font-weight: 600; letter-spacing: .3px; }
  .count { font: 10px var(--mono); color: var(--text-dim); border: 1px solid var(--border); padding: 1px 5px; }
  .well-heading p { margin: 5px 0 0 24px; font-size: 10px; color: var(--text-dim); }
  .channel-lane { display: flex; align-items: stretch; gap: 8px; overflow-x: auto; padding-bottom: 4px; scrollbar-width: thin; scrollbar-color: var(--border) transparent; }
  :global(#app .container-prototype .study-channel) { flex: 1 0 196px; min-width: 196px; padding: 8px 8px 0; background: var(--flat-4); border: 1px solid var(--border); border-radius: 5px; }
  :global(#app .container-prototype .identity) { display: flex; align-items: center; gap: 6px; min-height: 32px; padding: 4px 6px; background: color-mix(in srgb, var(--track-color) 28%, var(--flat-2)); border-radius: 2px; }
  .identity strong { font-size: 12px; font-weight: 500; white-space: nowrap; }
  .identity img { flex-shrink: 0; }
  .identity output { margin-left: auto; font: 11px var(--mono); white-space: nowrap; }
  .identity small { font-size: 9px; color: var(--text-dim); }
  .channel-controls { padding: 7px 0 6px; }
  .source-line { display: flex; gap: 8px; align-items: center; min-height: 26px; }
  .source-line > span:first-child, .control-label { color: var(--text-dim); font-size: 10px; flex-shrink: 0; }
  .source-line > span:last-child { font-size: 11px; overflow-wrap: anywhere; }
  .route-line { display: flex; gap: 6px; align-items: center; min-height: 28px; }
  :global(#app .container-prototype .route-toggle) { display: flex; align-items: center; gap: 4px; padding: 3px; min-height: 26px; background: transparent; border-color: transparent; font-size: 11px; }
  .route-toggle i { width: 6px; height: 6px; border: 1px solid var(--text-dim); }
  :global(#app .container-prototype .route-toggle i) { border-radius: 50%; }
  .route-toggle[aria-pressed='true'] i { background: var(--ok); border-color: var(--ok); }
  .policy-label { font-size: 11px; color: var(--text); }
  .route-count { margin-left: auto; color: var(--text-dim); font: 9px var(--mono); }
  .channel-switches { display: flex; gap: 5px; align-items: center; }
  .kind-label { margin-right: auto; font: 9px var(--mono); letter-spacing: 1px; color: var(--text-dim); }
  :global(#app .container-prototype .channel-switches button) { min-width: 29px; min-height: 26px; padding: 3px 7px; background: var(--flat-0); font: 11px var(--mono); }
  :global(#app .container-prototype .mute-toggle[aria-pressed='true']) { border-color: var(--err); color: #f0abab; background: #382022; }
  :global(#app .container-prototype .bypass-toggle[aria-pressed='true']) { border-color: var(--warn); color: var(--warn); background: #332a1b; }
  .channel-body { display: flex; gap: 10px; padding-top: 10px; border-top: 1px solid var(--border); }
  .level-section { flex: 0 0 67px; }
  .vertical-controls { display: flex; height: 292px; align-items: stretch; gap: 5px; }
  .meter-pair { display: flex; gap: 2px; width: 17px; padding: 6px 2px; background: var(--flat-0); }
  .meter-slot { flex: 1; position: relative; background: #14211e; overflow: hidden; }
  .meter-slot i { position: absolute; bottom: 0; width: 100%; background: repeating-linear-gradient(to top, transparent 0 3px, var(--flat-0) 3px 5px), var(--ok); }
  .meter-scale { display: flex; flex-direction: column; justify-content: space-between; color: var(--text-dim); width: 21px; padding: 4px 0; font: 8px var(--mono); }
  .vertical-fader { writing-mode: vertical-lr; direction: rtl; appearance: none; width: 22px; min-width: 22px; height: 100%; margin: 0; background: transparent; cursor: ns-resize; }
  :global(#app .container-prototype .vertical-fader::-webkit-slider-runnable-track) { width: 4px; background: var(--flat-0); border: 0; border-radius: 2px; }
  :global(#app .container-prototype .vertical-fader::-webkit-slider-thumb) { appearance: none; width: 20px; height: 24px; margin-left: -8px; border: 1px solid #adb5bd; border-radius: 3px; background: #89929b; box-shadow: none; }
  .vertical-fader::-moz-range-track { width: 4px; background: var(--flat-0); border: 0; }
  .vertical-fader::-moz-range-thumb { width: 20px; height: 24px; border: 1px solid #adb5bd; border-radius: 3px; background: #89929b; }
  .meter-caption { display: block; font: 8px var(--mono); margin: 9px 0; color: var(--text-dim); text-align: center; }
  .plugin-column { flex: 1; min-width: 0; }
  .plugin-heading { display: flex; justify-content: space-between; gap: 4px; font: 9px var(--mono); letter-spacing: .5px; color: var(--text-dim); margin: 4px 0 10px; }
  .plugin-row { display: flex; gap: 6px; align-items: baseline; min-height: 30px; padding: 6px 0; font-size: 11px; border-bottom: 1px solid var(--border); overflow-wrap: anywhere; }
  .plugin-row i { width: 5px; height: 5px; flex-shrink: 0; background: var(--ok); }
  :global(#app .container-prototype .plugin-row i) { border-radius: 50%; }
  .plugin-row.bypassed { color: var(--text-dim); }
  .plugin-row.bypassed i { background: var(--warn); }
  .empty-plugin { padding: 8px 0; font-size: 11px; color: var(--text-dim); border-bottom: 1px solid var(--border); }
  .chain-state { display: grid; gap: 5px; font: 8px var(--mono); color: var(--text-dim); margin-top: 14px; }
  .chain-state span { font-size: 10px; }
  .channel-footer { display: flex; justify-content: space-between; padding: 8px 0; border-top: 1px solid var(--border); color: var(--text-dim); font: 9px var(--mono); }
  .study-footnote { display: flex; justify-content: space-between; flex-wrap: wrap; gap: 6px; margin: 14px 0 18px; font-size: 10px; color: var(--text-dim); }
  .state-panel { border-top: 1px solid var(--border); color: var(--text-dim); padding-top: 10px; }
  summary { cursor: pointer; width: fit-content; padding: 5px 0; font-size: 11px; }
  .state-panel p { font-size: 11px; }
  pre { max-height: 320px; overflow: auto; background: var(--flat-0); padding: 14px; font: 11px/1.6 var(--mono); }
  :global(#app .container-prototype :is(button, input, summary, [tabindex]):focus-visible) { outline: 2px solid var(--accent); outline-offset: 2px; }
  :global(#app .container-prototype button:hover) { border-color: var(--text-dim); }
  :global(#app .container-prototype .shared-rack) { padding: 0 12px; background: #090b0d; border: 1px solid #080a0c; border-radius: 10px; box-shadow: inset 0 3px 9px #000a, 0 1px 0 #ffffff0d; }
  .rack-heading, .rack-footer { display: flex; justify-content: space-between; gap: 12px; padding: 12px 5px; color: var(--text-dim); font: 9px var(--mono); letter-spacing: 1px; }
  .rack-heading { border-bottom: 1px solid var(--border); }
  .rack-heading > span:first-child { display: flex; align-items: center; gap: 8px; color: var(--text); }
  .rack-heading i { width: 5px; height: 5px; background: var(--ok); }
  .rack-partitions { display: grid; grid-template-columns: minmax(0, 2fr) minmax(0, 1fr); }
  :global(#app .container-prototype .shared-rack .container-well) { border: 0; border-radius: 0; padding: 0 12px 8px 0; background: transparent; box-shadow: none; }
  :global(#app .container-prototype .shared-rack .output-well) { padding: 0 0 8px 12px; border-left: 1px solid var(--border); }
  .rack-footer { border-top: 1px solid var(--border); }
  .stacked-layout { display: grid; }
  :global(#app .container-prototype .stacked-layout .container-well) { display: grid; grid-template-columns: 155px minmax(0, 1fr); padding: 12px; }
  .stacked-layout .well-heading { padding: 12px 8px; }
  .stacked-layout .well-heading > div { flex-wrap: wrap; }
  .stacked-layout .well-heading p { margin: 8px 0 0; max-width: 110px; line-height: 1.8; }
  :global(#app .container-prototype .stacked-layout .study-channel) { flex: 0 0 230px; }
  .stacked-layout .vertical-controls { height: 148px; }
  .flow-label { padding: 9px 0 9px 178px; color: var(--text-dim); font: 9px var(--mono); letter-spacing: .8px; }
  @media (max-width: 900px) {
    main { padding: 20px 12px 105px; }
    .app-header { gap: 12px; padding: 10px 12px; }
    .session { padding-left: 12px; }
    .telemetry { display: none; }
    .mock-badge { margin-left: auto; }
    .split-layout { grid-template-columns: minmax(0, 1fr) minmax(0, 1fr); gap: 10px; }
    .rack-partitions { grid-template-columns: minmax(0, 1fr) minmax(0, 1fr); }
    :global(#app .container-prototype .stacked-layout .container-well) { grid-template-columns: 120px minmax(0, 1fr); }
    .flow-label { padding-left: 144px; }
  }
  @media (max-width: 560px) {
    .session { display: none; }
    .study-heading { align-items: flex-start; flex-direction: column; gap: 10px; }
    .study-heading h1 { font-size: 21px; }
    .split-layout, .rack-partitions { grid-template-columns: minmax(0, 1fr); gap: 14px; }
    :global(#app .container-prototype .shared-rack .container-well) { padding: 0 0 8px; }
    :global(#app .container-prototype .shared-rack .output-well) { border-left: 0; border-top: 1px solid var(--border); }
    :global(#app .container-prototype .stacked-layout .container-well) { grid-template-columns: minmax(0, 1fr); }
    .stacked-layout .well-heading p { max-width: none; margin: 4px 0; }
    .flow-label { padding-left: 16px; }
    .rack-heading, .rack-footer { flex-wrap: wrap; }
  }
</style>
