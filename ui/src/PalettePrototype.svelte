<script lang="ts">
  // THROWAWAY: five palette studies on /?prototype=palette&variant=A.
  // Keep geometry fixed to compare colour, not layout. No engine calls or persistence.
  import { onMount } from "svelte";

  // Neutral surfaces stay separate from interaction, status and track identity colours.
  const themes = [
    { key: "A", name: "日常灰", english: "NEUTRAL / BLUE / SAGE", note: "純灰機架，灰藍只標示選取；鼠尾草綠留給電平，暖杏與灰紫只做音軌小標記。", scheme: "dark", colors: ["#181818", "#222222", "#282828", "#303030", "#393939", "#454545", "#E9E8E4", "#B8B8B5", "#A5B9CA", "#A0B6A1", "#D4B58C", "#DC9B97", "#C5AC98", "#B2ABC2"] },
    { key: "B", name: "柔霧灰", english: "SOFT GREY / JADE / CLAY", note: "柔和中灰，不帶綠色底調；玉灰綠選取、淡青灰電平，再用陶土與霧藍輕輕區分音軌。", scheme: "dark", colors: ["#202020", "#292929", "#303030", "#383838", "#414141", "#505050", "#EFEDEA", "#C4C2BF", "#A8C2B8", "#A5BABC", "#D9BC92", "#DEA19B", "#C8A396", "#A8B3CB"] },
    { key: "C", name: "暖紙灰", english: "WARM GREY / SLATE / OCHRE", note: "底色僅有極輕微暖意；石板藍負責互動，灰綠顯示訊號，赭米與灰粉點到為止。", scheme: "dark", colors: ["#1D1C1B", "#272625", "#2E2D2B", "#363532", "#403E3B", "#4C4A46", "#EDEAE4", "#C0BCB5", "#A8BAC8", "#ACB7A0", "#D6B785", "#DE9C96", "#C3B08B", "#C0A5AE"] },
    { key: "D", name: "墨黑留白", english: "INK / LINEN / DUSTY BLUE", note: "接近黑白的低彩度介面；亞麻白作焦點，僅保留灰藍訊號與兩個很小的暖色音軌標記。", scheme: "dark", colors: ["#121212", "#1B1B1B", "#232323", "#2B2B2B", "#353535", "#3F3F3F", "#E5E4E1", "#B2B1AE", "#CDC5B5", "#9FAFBD", "#D0B58D", "#D89B97", "#BBA995", "#B5A3A8"] },
    { key: "E", name: "白紙編輯", english: "PAPER / INK BLUE / OLIVE", note: "白與淺灰保持中性；墨藍只標示互動，橄欖灰用於電平，陶土與灰紫用於細小識別。", scheme: "light", colors: ["#E9E9E7", "#FAFAF8", "#F0F0ED", "#E5E5E1", "#DADAD5", "#B4B4AE", "#30312F", "#5F605B", "#496579", "#60735D", "#87652F", "#A24843", "#946C59", "#7F718D"] },
  ];
  const tokenNames = ["flat-0", "flat-1", "flat-2", "flat-3", "flat-4", "border", "text", "text-dim", "accent", "ok", "warn", "err", "track-a", "track-b"];
  let index = $state(Math.max(0, themes.findIndex(t => t.key === new URLSearchParams(location.search).get("variant"))));
  const theme = $derived(themes[index]);
  let details = $state(false);
  let tracks = $state([
    { name: "人聲", source: "Audio · Input 1 / 2", plugins: ["Pro-Q 3", "LA-2A Compressor"], gain: -3, mute: false, level: 74 },
    { name: "音樂", source: "App · Spotify", plugins: ["SSL Bus Compressor"], gain: -8, mute: false, level: 61 },
    { name: "空間效果", source: "FX · Stereo Bus", plugins: ["Valhalla VintageVerb"], gain: -12, mute: false, level: 46 },
    { name: "監聽", source: "Output · Speaker 1 / 2", plugins: ["Pro-L 2"], gain: 0, mute: false, level: 82 },
    { name: "串流", source: "Output · Virtual Cable", plugins: ["Limiter"], gain: -1, mute: false, level: 68 },
  ]);

  function choose(next: number) {
    index = (next + themes.length) % themes.length;
    const url = new URL(location.href);
    url.searchParams.set("variant", themes[index].key);
    history.replaceState(null, "", url);
  }
  function keyboard(event: KeyboardEvent) {
    if (event.defaultPrevented || event.altKey || event.ctrlKey || event.metaKey || event.shiftKey) return;
    if (event.target instanceof Element && event.target.closest("input, textarea, select, button, [contenteditable], [role='slider']")) return;
    if (event.key === "ArrowLeft" || event.key === "ArrowRight") {
      event.preventDefault();
      choose(index + (event.key === "ArrowRight" ? 1 : -1));
    }
  }
  onMount(() => { choose(index); });
  $effect(() => {
    const root = document.documentElement;
    tokenNames.forEach((name, i) => root.style.setProperty(`--${name}`, theme.colors[i]));
    root.style.colorScheme = theme.scheme;
    return () => {
      tokenNames.forEach(name => root.style.removeProperty(`--${name}`));
      root.style.removeProperty("color-scheme");
    };
  });
</script>

<svelte:window onkeydown={keyboard} />
<svelte:head><title>RoudaMix · 幾何配色 Prototype</title></svelte:head>

<div class="palette-prototype">
  <header class="prototype-header">
    <div class="brand"><span class="brand-mark" aria-hidden="true">◧</span> RoudaMix <span class="edition">COLOR STUDIES / 02</span></div>
    <span class="simulation">視覺原型 · 模擬資料 · 不連接音訊引擎</span>
  </header>

  <main>
    <section class="intro">
      <div><div class="eyebrow">NEUTRAL FIRST · ACCENTS WITH PURPOSE</div><h1>大部分是灰。色彩，只在需要的地方。</h1><p>中性面板與細線條，搭配少量灰藍、灰綠與暖色標記。不是換一種主題色，而是讓不同顏色各司其職。</p></div>
      <div class="geometry" aria-hidden="true"><i></i><i></i><i></i></div>
    </section>

    <nav class="theme-grid" aria-label="配色方案">
      {#each themes as option, i}
        <button class="theme-card" class:selected={index === i} aria-pressed={index === i} onclick={() => choose(i)}>
          <span class="theme-heading"><b>{option.key} / {option.name}</b><span>{index === i ? "使用中" : option.scheme === "light" ? "淺色" : "深色"}</span></span>
          <span class="mini-palette" aria-hidden="true">{#each [1, 3, 8, 9, 12, 13] as c}<i style:background={option.colors[c]}></i>{/each}</span>
          <small>{option.english}</small>
        </button>
      {/each}
    </nav>

    <section class="mix-console" aria-label="主程式模擬機架">
      <header class="console-bar"><strong><span class="status-dot"></span> Studio Session <span class="sample-label">DEMO</span></strong><span class="mono">48 000 Hz · buf 256 · load 12% · xrun 0</span><button onclick={() => details = !details} aria-expanded={details}>色票與狀態 {details ? "−" : "+"}</button></header>
      <div class="group-labels"><span>01 — 輸入 / INPUTS</span><span>02 — 輸出 / OUTPUTS</span></div>
      <div class="rack">
        {#each tracks as track, i}
          <article class="channel" style:--track-mark={theme.colors[[12, 8, 13, 7, 7][i]]}>
            <header class="channel-name"><span class="channel-number">0{i + 1}</span><strong>{track.name}</strong><span class="track-kind">{i > 2 ? "OUT" : "IN"}</span></header>
            <div class="routing"><span class="label">{i > 2 ? "輸出裝置" : "音訊來源"}</span><div class="route-value">{track.source}</div><span class="label">{i > 2 ? "延遲補償" : "傳送至"}</span><div class="route-tags">{i > 2 ? "Full PDC" : "監聽 ＋ 串流"}<span>↗</span></div></div>
            <section class="plugin-rack" aria-label={`${track.name} 效果器`}><div class="section-label">INSERT FX <span>{track.plugins.length.toString().padStart(2, "0")}</span></div>{#each track.plugins as plugin}<div class="plugin"><span class="plugin-led"></span><span>{plugin}</span><span class="plugin-symbol" aria-hidden="true">≡</span></div>{/each}<div class="unused-slot">— 空插槽 —</div></section>
            <div class="fader-panel"><div class="gain-row"><label for={`gain-${i}`}>GAIN</label><output for={`gain-${i}`}>{track.gain > 0 ? "+" : ""}{track.gain.toFixed(1)} <small>dB</small></output></div><input id={`gain-${i}`} type="range" min="-60" max="12" step="0.5" bind:value={track.gain} /><div class="gain-scale"><span>−60</span><span>0</span><span>+12</span></div><button class="mute" class:muted={track.mute} aria-pressed={track.mute} onclick={() => track.mute = !track.mute}>{track.mute ? "靜音中 / MUTED" : "靜音 / MUTE"}</button></div>
            <div class="meter-area"><div class="section-label">模擬電平 <span>{track.mute ? "−∞" : `${(track.gain - 9).toFixed(1)}`} dB</span></div><div class="meter"><i style:width={`${track.mute ? 0 : Math.max(0, Math.min(100, track.level + track.gain))}%`}></i></div><div class="meter"><i style:width={`${track.mute ? 0 : Math.max(0, Math.min(100, track.level + track.gain - 4))}%`}></i></div><div class="gain-scale"><span>−60</span><span>−24</span><span>−12</span><span>0</span></div></div>
            <footer class="channel-footer"><span class="status-dot"></span>{track.mute ? "MUTED" : "SIGNAL READY"}<span>STEREO</span></footer>
          </article>
        {/each}
      </div>
    </section>

    <section class="theme-story" aria-live="polite"><div><span class="eyebrow">{theme.english}</span><h2>{theme.key} / {theme.name}</h2><p>{theme.note}</p></div><div class="material-chips">{#each [["面板", 1], ["選取", 8], ["電平", 9], ["音軌 A", 12], ["音軌 B", 13]] as [label, c]}<div><i style:background={theme.colors[Number(c)]}></i><span>{label}</span><code>{theme.colors[Number(c)]}</code></div>{/each}</div></section>
    {#if details}
      <section class="token-panel"><h2>完整主題 tokens / 互動狀態</h2><div class="state-samples"><button class="primary">主要操作</button><button>一般操作</button><button disabled>停用操作</button><span class="success">● 正常</span><span class="warning">△ 注意</span><span class="error">× 錯誤</span></div><div class="token-grid">{#each tokenNames as name, i}<div><i style:background={theme.colors[i]}></i><code>--{name}</code><code>{theme.colors[i]}</code></div>{/each}</div></section>
    {/if}
    <p class="prototype-note">PROTOTYPE ONLY — 推桿與靜音僅供視覺試用；不儲存任何設定。確定配色後再套用至正式主程式。</p>
  </main>
  <nav class="prototype-switcher" aria-label="Prototype 切換器"><button aria-label="上一組配色" onclick={() => choose(index - 1)}>←</button><div aria-live="polite"><small>PALETTE PROTOTYPE · {index + 1} / 5</small><strong>{theme.key} — {theme.name}</strong></div><button aria-label="下一組配色" onclick={() => choose(index + 1)}>→</button></nav>
</div>

<style>
  .palette-prototype { min-height: 100%; background: var(--bg); color: var(--text); }
  .prototype-header { padding: 18px 28px; display: flex; align-items: center; justify-content: space-between; gap: 16px; border-bottom: 1px solid var(--border); }
  .brand { display: flex; align-items: center; gap: 10px; font-size: 18px; font-weight: 600; letter-spacing: -.4px; }
  .brand-mark { color: var(--text-dim); font-size: 27px; }
  .edition, .simulation, .sample-label, .label, .section-label, .eyebrow, .channel-number, .track-kind, .gain-scale, .channel-footer, .prototype-note { color: var(--text-dim); font-size: 11px; }
  .edition { margin-left: 15px; letter-spacing: 1.5px; }
  main { max-width: 1550px; margin: auto; padding: 30px 28px 120px; }
  .intro { display: flex; align-items: center; justify-content: space-between; gap: 20px; margin-bottom: 28px; }
  .eyebrow { letter-spacing: 2px; font-family: var(--mono); }
  h1 { font-size: clamp(22px, 2.5vw, 32px); font-weight: 500; letter-spacing: 1px; margin: 10px 0; }
  p { color: var(--text-dim); margin: 8px 0; line-height: 1.7; }
  .geometry { display: flex; flex: 0 0 auto; }
  .geometry i { width: 35px; height: 35px; background: var(--flat-2); border: 1px solid var(--border); }
  .geometry i:nth-child(2) { background: transparent; transform: rotate(45deg) scale(.7); }
  .geometry i:last-child { background: var(--flat-3); }
  .theme-grid { display: grid; grid-template-columns: repeat(5, minmax(0, 1fr)); gap: 10px; margin-bottom: 26px; }
  :global(#app .palette-prototype .theme-card) { padding: 13px; text-align: left; background: var(--flat-1); min-width: 0; }
  :global(#app .palette-prototype .theme-card.selected) { border-color: var(--accent); outline: 1px solid var(--accent); outline-offset: -1px; }
  .theme-heading { display: flex; justify-content: space-between; gap: 6px; }
  .theme-heading b { font-weight: 500; }
  .theme-heading > span { color: var(--text-dim); font-size: 11px; }
  .mini-palette { display: flex; height: 24px; margin: 12px 0 9px; border: 1px solid var(--border); }
  .mini-palette i { flex: 0 0 7%; }
  .mini-palette i:nth-child(-n+2) { flex: 1; }
  .theme-card small { font: 10px var(--mono); color: var(--text-dim); letter-spacing: .4px; }
  .mix-console { border: 1px solid var(--border); background: var(--flat-0); }
  .console-bar { background: var(--flat-1); padding: 12px 16px; display: flex; align-items: center; gap: 14px; flex-wrap: wrap; border-bottom: 1px solid var(--border); }
  .console-bar strong { display: flex; align-items: center; gap: 9px; font-weight: 500; }
  .console-bar .mono { margin-left: auto; color: var(--text-dim); font-size: 11px; }
  .console-bar button { min-height: 36px; }
  .sample-label { border: 1px solid var(--border); padding: 1px 4px; font-size: 9px; }
  .status-dot, .plugin-led { display: inline-block; width: 5px; height: 5px; background: var(--ok); flex-shrink: 0; }
  .group-labels { display: grid; grid-template-columns: 3fr 2fr; gap: 12px; padding: 14px 14px 8px; color: var(--text-dim); font: 10px var(--mono); letter-spacing: 1.4px; }
  .rack { display: grid; grid-template-columns: repeat(5, minmax(0, 1fr)); gap: 10px; padding: 0 12px 12px; }
  .channel { border: 1px solid var(--border); background: var(--flat-1); min-width: 0; }
  .channel-name { display: flex; align-items: center; gap: 8px; padding: 13px 10px; border-bottom: 1px solid var(--border); }
  .channel-name::before { content: ""; width: 3px; height: 13px; background: var(--track-mark); flex-shrink: 0; }
  .channel-name strong { font-weight: 500; font-size: 14px; }
  .channel-number { font-family: var(--mono); }
  .track-kind { margin-left: auto; font-size: 10px; }
  .routing { padding: 11px; display: grid; gap: 6px; }
  .label { font-size: 10px; }
  .route-value, .route-tags { padding: 7px; background: var(--flat-2); border: 1px solid var(--border); font-size: 11px; overflow-wrap: anywhere; }
  .route-tags { display: flex; justify-content: space-between; }
  .route-tags > span { color: var(--text-dim); }
  .plugin-rack { padding: 11px; min-height: 184px; background: var(--flat-2); border-block: 1px solid var(--border); }
  .section-label { display: flex; justify-content: space-between; gap: 5px; font: 10px var(--mono); margin-bottom: 10px; }
  .plugin { background: var(--flat-3); border: 1px solid var(--border); padding: 10px 7px; display: flex; align-items: center; gap: 7px; margin-bottom: 7px; font-size: 11px; }
  .plugin-symbol { margin-left: auto; color: var(--text-dim); }
  .unused-slot { border: 1px dashed var(--border); padding: 9px; text-align: center; color: var(--text-dim); font-size: 10px; }
  .fader-panel { padding: 14px 11px; }
  .gain-row { display: flex; align-items: baseline; justify-content: space-between; margin-bottom: 12px; }
  .gain-row label { font: 10px var(--mono); color: var(--text-dim); }
  output { font: 21px var(--mono); }
  output small { font-size: 10px; color: var(--text-dim); }
  input[type="range"] { width: 100%; height: 24px; margin: 0; appearance: none; background: transparent; cursor: pointer; }
  :global(#app .palette-prototype input[type="range"]::-webkit-slider-runnable-track) { height: 4px; background: var(--border); border: 0; }
  :global(#app .palette-prototype input[type="range"]::-webkit-slider-thumb) { appearance: none; width: 8px; height: 16px; margin-top: -6px; background: var(--text-dim); border: 0; }
  :global(#app .palette-prototype input[type="range"]:focus-visible::-webkit-slider-thumb) { background: var(--accent); }
  input[type="range"]::-moz-range-track { height: 4px; background: var(--border); }
  input[type="range"]::-moz-range-thumb { width: 8px; height: 16px; border: 0; border-radius: 0; background: var(--text-dim); }
  .gain-scale { display: flex; justify-content: space-between; font: 9px var(--mono); }
  .mute { width: 100%; min-height: 36px; margin-top: 15px; font: 10px var(--mono); letter-spacing: 1px; }
  :global(#app .palette-prototype .mute.muted) { background: var(--warn); color: var(--flat-0); border-color: var(--warn); }
  .meter-area { padding: 12px 11px; background: var(--flat-2); border-top: 1px solid var(--border); }
  .meter { height: 7px; background: var(--flat-0); margin-bottom: 4px; }
  .meter i { display: block; height: 100%; background: repeating-linear-gradient(90deg, transparent 0 5px, var(--flat-2) 5px 7px), var(--ok); }
  .channel-footer { padding: 10px; display: flex; align-items: center; gap: 6px; font: 8px var(--mono); }
  .channel-footer > span:last-child { margin-left: auto; }
  .theme-story { margin-top: 24px; display: flex; justify-content: space-between; align-items: center; gap: 25px; }
  h2 { font-size: 18px; font-weight: 500; margin: 6px 0; }
  .material-chips { display: flex; gap: 12px; }
  .material-chips > div { display: grid; gap: 5px; font-size: 10px; }
  .material-chips i { height: 28px; border: 1px solid var(--border); }
  .material-chips code { color: var(--text-dim); font-size: 10px; }
  .token-panel { margin-top: 20px; padding: 20px; border: 1px solid var(--border); background: var(--flat-1); }
  .state-samples { display: flex; align-items: center; gap: 14px; flex-wrap: wrap; margin: 15px 0; }
  .state-samples button { min-height: 36px; }
  :global(#app .palette-prototype button.primary:hover) { background: var(--accent); filter: brightness(1.08); }
  .success { color: var(--ok); } .warning { color: var(--warn); } .error { color: var(--err); }
  .token-grid { display: grid; grid-template-columns: repeat(3, 1fr); gap: 8px 20px; }
  .token-grid > div { display: flex; align-items: center; gap: 10px; font-size: 11px; }
  .token-grid i { width: 18px; height: 18px; border: 1px solid var(--border); }
  .token-grid code:last-child { margin-left: auto; color: var(--text-dim); }
  .prototype-note { margin-top: 22px; font-size: 10px; letter-spacing: .4px; }
  .prototype-switcher { position: fixed; bottom: 20px; left: 50%; transform: translateX(-50%); z-index: 100; display: flex; align-items: center; gap: 22px; padding: 8px; background: var(--flat-1); border: 1px solid var(--text-dim); color: var(--text); }
  .prototype-switcher div { min-width: 185px; display: grid; text-align: center; gap: 3px; }
  .prototype-switcher small { font: 9px var(--mono); letter-spacing: 1.3px; }
  .prototype-switcher strong { font-size: 13px; font-weight: 600; }
  :global(#app .palette-prototype .prototype-switcher button) { width: 44px; height: 44px; color: var(--text); background: var(--flat-3); border-color: var(--border); font-size: 18px; }
  @media (max-width: 1000px) { .edition { display: none; } .theme-grid { grid-template-columns: repeat(3, 1fr); } .rack { grid-template-columns: repeat(3, minmax(0, 1fr)); } .group-labels { display: none; } .rack { padding-top: 12px; } .theme-story { align-items: flex-start; flex-direction: column; } }
  @media (max-width: 600px) { main { padding: 22px 14px 125px; } .prototype-header { padding: 12px 14px; flex-wrap: wrap; } .simulation { font-size: 10px; } .geometry { display: none; } .theme-grid { grid-template-columns: repeat(2, minmax(0, 1fr)); } .rack { grid-template-columns: repeat(2, minmax(0, 1fr)); } .console-bar .mono { margin-left: 0; } .token-grid { grid-template-columns: 1fr; } .material-chips { gap: 8px; flex-wrap: wrap; } .prototype-switcher { gap: 8px; max-width: calc(100% - 20px); } .channel-name { gap: 4px; } .channel-footer { font-size: 7px; } }
  @media (max-width: 360px) { .rack { grid-template-columns: 1fr; } }
</style>
