<script lang="ts">
  import { onMount } from 'svelte';
  import { workflowMotion } from './workflowMotion';
  import { content, type Locale } from './content';
  import MotionPointer from './MotionPointer.svelte';
  import { heroMotion } from './heroMotion';
  let { progress, locale, covered = false, overviewPhase }: { progress: number; locale: Locale; covered?: boolean; overviewPhase?: number } = $props();
  const m = $derived(overviewPhase === undefined ? workflowMotion(progress) : heroMotion(overviewPhase));
  const t = $derived(content[locale]);
  const zh = $derived(locale === 'zh');
  const signalBars = Array.from({ length: 37 }, (_, i) => 12 + Math.abs(Math.sin(i * 2.7) * Math.cos(i * 0.63)) * 62);
  let root: HTMLDivElement;
  let sceneVisible = $state(false);
  onMount(() => {
    let visible = false;
    const activity = () => { sceneVisible = visible && !document.hidden; };
    const observer = new IntersectionObserver(([entry]) => { visible = entry.isIntersecting; activity(); });
    observer.observe(root);
    document.addEventListener('visibilitychange', activity);
    return () => { observer.disconnect(); document.removeEventListener('visibilitychange', activity); };
  });
</script>

<div bind:this={root} class="scene animated-workflow" class:is-active={sceneVisible && !covered} class:started={m.stage > 0} data-stage={m.stage}
  style={`--signal-offset:${-progress * 1800}px; --launch-scale:${0.82 + m.launch * 0.18}; --launch-rotate:${(1 - m.launch) * -5}deg; --focus:${m.focus}; --monitor-route:${m.monitorRoute}; --stream-route:${m.streamRoute}; --bypass:${m.bypassMix}; --input-reveal:${m.inputReveal}`}>
  <div class="signal-orbit" aria-hidden="true"></div>
  <div class="source-cloud" aria-hidden="true">
    {#each t.signal as source, i}<span class={`source source-${i}`} style={`opacity:${0.65 + m.sources[i] * 0.35};margin-top:${(1 - m.sources[i]) * -24}px`}><i></i>{source}</span>{/each}
    <svg viewBox="0 0 600 90" preserveAspectRatio="none">
      {#each ['M80 0 V20 Q80 45 150 45 H260 Q300 45 300 85','M300 0 V85','M520 0 V20 Q520 45 450 45 H340 Q300 45 300 85'] as path, i}
        <path class="route-base" d={path} />
        <path d={path} pathLength="1" style={`stroke-dasharray:1;stroke-dashoffset:${1 - m.sources[i]}`} />
        <circle class="source-packet signal-packet" r="3" style={`offset-path:path('${path}');animation-delay:${i * -0.6}s;opacity:${0.65 + m.sources[i] * 0.35}`} />
      {/each}
    </svg>
  </div>
  <div class="workflow-instruction" aria-hidden="true">
    <span class="instruction-step">0{m.stage + 1}</span>
    <span>{m.stage === 0 ? (zh ? '把聲音帶進工作區' : 'Bring your sound into the workspace')
      : m.stage === 1 ? (m.input ? (zh ? '麥克風已進入 Audio 軌' : 'Microphone connected to the Audio track') : (zh ? '選擇裝置，再加入麥克風' : 'Select a device, then add your microphone'))
      : m.stage === 2 ? (m.plugin ? (zh ? '混響已加入效果鏈' : 'Reverb is now in the effect chain') : (zh ? '打開清單，加入已安裝的 VST3' : 'Open the list and add your installed VST3'))
      : m.stage === 3 ? (m.routed ? (zh ? '兩路連線完成' : 'Both destinations connected') : (zh ? '先接監聽，再接串流' : 'Connect the monitor, then the stream'))
      : m.stage === 4 ? (m.bypass ? (zh ? '青色略過混響，紫色保留效果' : 'Cyan skips reverb. Purple keeps it.') : (zh ? '按 M，改變監聽的處理路徑' : 'Press M to change the monitor path'))
      : m.stage === 5 ? (m.obs ? (zh ? '訊號已到達 OBS' : 'Audio is reaching OBS') : (zh ? '將 CABLE Output 設為 OBS 的輸入' : 'Choose CABLE Output as the OBS input'))
      : (m.voice ? (zh ? '同一份串流混音，也能用來聊天' : 'Use the same stream mix for voice chat') : (zh ? '在 Discord 選擇相同輸入裝置' : 'Choose the same input in Discord'))}</span>
  </div>
  <div class="mix-window">
    <div class="window-bar"><span class="window-brand"><img src={`${import.meta.env.BASE_URL}brand.png`} alt="" /> RoudaMix</span><span class="window-session">Creator session</span><span class="window-dots" aria-hidden="true">— &nbsp; □ &nbsp; ×</span></div>
    <div class="workspace">
      <div class="workspace-sidebar" aria-hidden="true"><span class="sidebar-active">≋</span><span>＋</span><span>·</span><span class="sidebar-bottom">01</span></div>
      <div class="mix-main">
        <div class="workspace-heading"><span>{zh ? '音訊工作區' : 'Audio workspace'}</span><span class="engine-status"><i></i> 48 kHz</span></div>
        <div class="settings-card" class:highlight={m.stage === 1}>
          <span class="tiny-label">{zh ? '設定 / 音訊 / Session' : 'Settings / Audio / Session'}</span>
          <span>{m.deviceSelected > 0.5 ? (zh ? '系統音訊 (WASAPI)' : 'System audio (WASAPI)') : (zh ? '選擇音訊裝置' : 'Choose an audio device')} <span class="chevron">⌄</span></span>
          <div class="device-menu demonstration-menu" style={`opacity:${m.deviceMenu};transform:translateY(${(1 - m.deviceMenu) * -12}px) scale(${0.96 + m.deviceMenu * 0.04});visibility:${m.deviceMenu > 0 ? 'visible' : 'hidden'}`} aria-hidden="true">
            <span>ASIO</span><strong class:selected={m.deviceSelected > 0.1}>{zh ? '系統音訊 (WASAPI)' : 'System audio (WASAPI)'}<b>✓</b><MotionPointer phase={m.devicePointer} label={zh ? '選擇裝置' : 'Select device'} /></strong>
          </div>
        </div>
        <div class="track" class:highlight={m.stage === 1} style={`--track-enter:${m.inputReveal}`}>
          <div class="track-label"><span class="track-number mono">01</span><strong style={`opacity:${0.35 + m.inputReveal * 0.65}`}>{t.signal[0]}</strong><span class="badge">Audio</span></div>
          <div class="track-body"><span class="input-label">{m.input ? (zh ? '輸入 · 麥克風' : 'Input · Microphone') : (zh ? '＋ Audio 軌 → 選擇麥克風' : '+ Audio track → Select microphone')}</span>
            <div class="waveform" class:receiving={m.input} aria-hidden="true">{#each signalBars as height, i}<i style={`height:${height * (0.15 + m.inputReveal * 0.85)}%;--bar-duration:${0.6 + (i % 7) * 0.12}s;animation-delay:${i * -0.07}s`}></i>{/each}</div><span class="mono level">{m.input ? '−12.4 dB' : '−∞ dB'}</span>
          </div>
          <MotionPointer phase={m.inputPointer} label={zh ? '加入麥克風' : 'Add microphone'} />
        </div>
        <div class="plugin-slot" class:highlight={m.stage === 2} class:loaded={m.plugin}>
          <span class="empty-plugin" style={`opacity:${1 - m.pluginInsert}`}>＋ {zh ? '加入 VST3 外掛' : 'Add VST3 plugin'}</span>
          <div class="plugin-installed" style={`opacity:${m.pluginInsert};transform:translateY(${(1 - m.pluginInsert) * -32}px) scale(${0.9 + m.pluginInsert * 0.1})`} aria-hidden={!m.plugin}>
            <span class="plugin-power" aria-hidden="true">⏻</span><div><strong>{zh ? '混響' : 'Reverb'}</strong><span class="tiny-label">VST3 / {zh ? '自行安裝的外掛' : 'Separately installed plugin'}</span></div>
            <span class="bypass-chip" class:enabled={m.bypass}><span class="bypass-switch"><i style={`transform:translateX(${m.bypassMix * 12}px)`}></i></span>M <span>Monitor Bypass {m.bypass ? 'ON' : 'OFF'}</span></span>
          </div>
          <div class="plugin-picker-illustration demonstration-menu" style={`opacity:${m.pluginMenu};transform:translateY(${(1 - m.pluginMenu) * 20}px) scale(${0.94 + m.pluginMenu * 0.06});visibility:${m.pluginMenu > 0 ? 'visible' : 'hidden'}`} aria-hidden="true">
            <span class="tiny-label">{zh ? '外掛清單 / 已安裝' : 'Plugin list / Installed'}</span><div><strong>{zh ? '混響 / Reverb' : 'Reverb'}</strong><span>VST3</span><b>{zh ? '加入 →' : 'Add →'}</b><MotionPointer phase={m.pluginPointer} label={zh ? '加入外掛' : 'Add plugin'} /></div>
          </div>
          <MotionPointer phase={m.bypassPointer} label={zh ? '啟用 Monitor Bypass' : 'Enable Monitor Bypass'} />
        </div>
        <div class="routing" class:connected={m.monitorRoute > 0} class:explaining-bypass={m.stage >= 4} aria-hidden="true">
          <svg viewBox="0 0 500 110" preserveAspectRatio="none">
            <path class="route-base" d="M250 0 V40 Q250 58 215 58 H150 Q120 58 120 80 V110 M250 40 Q250 58 285 58 H350 Q380 58 380 80 V110" />
            <path class="draw-route monitor-path" pathLength="1" d="M250 0 V40 Q250 58 215 58 H150 Q120 58 120 80 V110" style={`stroke-dashoffset:${1 - m.monitorRoute};opacity:${1 - m.bypassMix}`} />
            <path class="draw-route bypass-path" pathLength="1" d="M250 0 H120 Q55 0 55 55 T120 110" style={`stroke-dashoffset:${1 - m.bypassMix}`} />
            <path class="draw-route stream-path" pathLength="1" d="M250 0 V40 Q250 58 285 58 H350 Q380 58 380 80 V110" style={`stroke-dashoffset:${1 - m.streamRoute}`} />
            {#each [0, 1, 2] as i}
              <circle class="signal-packet monitor-packet" r="3" style={`offset-path:path('M250 0 V40 Q250 58 215 58 H150 Q120 58 120 80 V110');animation-delay:${i * -0.65}s;opacity:${m.monitorRoute * (1 - m.bypassMix)}`} />
              <circle class="signal-packet bypass-packet" r="3" style={`offset-path:path('M250 0 H120 Q55 0 55 55 T120 110');animation-delay:${i * -0.65}s;opacity:${m.bypassMix}`} />
              <circle class="signal-packet stream-packet" r="3" style={`offset-path:path('M250 0 V40 Q250 58 285 58 H350 Q380 58 380 80 V110');animation-delay:${i * -0.65}s;opacity:${m.streamRoute}`} />
            {/each}
          </svg>
          <span class="flow-effect" style={`opacity:${m.stage >= 4 ? 1 : 0}`}>Reverb</span><span class="flow-bypass" style={`opacity:${m.bypassMix}`}>Bypass</span>
        </div>
        <div class="outputs" class:highlight={m.stage === 3 || m.stage === 4}>
          <div class="output monitor" style={`opacity:${0.3 + m.monitorRoute * 0.7};transform:translateY(${(1 - m.monitorRoute) * 8}px)`}>
            <span class="tiny-label">01 / {t.monitor}</span><strong>{t.headphones}</strong><span class="policy">Low Latency</span><div class="meter" class:meter-running={m.monitorRoute === 1}><i style={`width:${m.monitorRoute * 78}%`}></i></div><span class="output-result">{m.bypass ? t.dry : m.monitorRoute === 1 ? t.wet : '—'}</span>
            <MotionPointer phase={m.monitorPointer} label={zh ? '連到監聽' : 'Connect monitor'} />
          </div>
          <div class="output stream" style={`opacity:${0.3 + m.streamRoute * 0.7};transform:translateY(${(1 - m.streamRoute) * 8}px)`}>
            <span class="tiny-label">02 / {t.stream}</span><strong>{m.streamRoute > 0.5 ? 'CABLE Input' : '—'}</strong><span class="policy">Full PDC</span><div class="meter" class:meter-running={m.streamRoute === 1}><i style={`width:${m.streamRoute * 86}%`}></i></div><span class="output-result">{m.streamRoute === 1 ? t.wet : '—'}</span>
            <MotionPointer phase={m.streamPointer} label={zh ? '連到串流' : 'Connect stream'} />
          </div>
        </div>
        <div class="window-footer"><span><i></i> {m.routed ? '2 routes connected' : 'Creator session'}</span><span>VST3 HOST</span></div>
      </div>
    </div>
  </div>
  <div class="external-route" aria-hidden="true" style={`opacity:${m.obsReveal}`}>
    <svg viewBox="0 0 200 80" preserveAspectRatio="none"><path class="route-base" d="M40 0 V20 Q40 40 80 40 H140 Q170 40 170 65 V80"/><path class="draw-route stream-path" pathLength="1" d="M40 0 V20 Q40 40 80 40 H140 Q170 40 170 65 V80" style={`stroke-dashoffset:${1 - m.cableRoute}`}/><circle class="signal-packet stream-packet" r="3" style={`offset-path:path('M40 0 V20 Q40 40 80 40 H140 Q170 40 170 65 V80');opacity:${m.obs ? 1 : 0}`}/></svg><span>VB-CABLE</span>
  </div>
  <div class="destination-app" class:visible={m.obsReveal > 0} class:discord={m.voiceReveal > 0.5} style={`opacity:${m.obsReveal};transform:translate(${(1 - m.obsReveal) * 48 + Math.sin(m.voiceReveal * Math.PI) * 24}px,${(1 - m.obsReveal) * 20}px)`}>
    <div class="app-monogram" aria-hidden="true"><span style={`opacity:${1 - m.voiceReveal}`}>OBS</span><span style={`opacity:${m.voiceReveal}`}>D</span></div>
    <div class="app-device"><strong class="app-title"><span style={`opacity:${1 - m.voiceReveal}`} aria-hidden={m.voiceReveal > 0.5}>OBS Studio</span><span style={`opacity:${m.voiceReveal}`} aria-hidden={m.voiceReveal <= 0.5}>Discord</span></strong><span>{m.voiceReveal > 0.5 ? (zh ? '輸入裝置' : 'Input device') : (zh ? '音訊輸入擷取' : 'Audio Input Capture')}</span><b>{m.obs && (m.stage < 6 || m.voice) ? 'CABLE Output' : (zh ? '選擇裝置…' : 'Choose device…')}</b></div>
    <div class="destination-meter" class:meter-running={m.obs && (m.stage < 6 || m.voice)} aria-hidden="true">{#each [40,70,55,90,60] as h, i}<i style={`height:${m.obs && (m.stage < 6 || m.voice) ? h : 0}%;animation-delay:${i * -0.13}s`}></i>{/each}</div>
    <div class="app-device-menu demonstration-menu" aria-hidden="true" style={`opacity:${m.obsMenu};transform:translateY(${(1 - m.obsMenu) * 12}px);visibility:${m.obsMenu > 0 ? 'visible' : 'hidden'}`}><span>{zh ? '裝置' : 'Device'}</span><strong>CABLE Output <b>✓</b><MotionPointer phase={m.obsPointer} label={zh ? '選取輸入裝置' : 'Select input device'} /></strong></div>
    <MotionPointer phase={m.voicePointer} label={zh ? '選取 CABLE Output' : 'Select CABLE Output'} />
    <span class="connection-confirmation" style={`opacity:${m.stage < 6 ? (m.obs ? m.cableRoute : 0) : (m.voice ? m.voiceRoute : 0)}`}>{zh ? '已收到串流混音' : 'Receiving stream mix'}</span>
  </div>
  <div class="scene-caption">{t.demo}</div>
</div>
