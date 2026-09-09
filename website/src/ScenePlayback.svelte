<script lang="ts">
  import { onMount, untrack } from 'svelte';
  import Scene from './Scene.svelte';
  import { storyState } from './story';
  import type { Locale } from './content';

  let { progress, locale, staticView = false, covered = false }: { progress: number; locale: Locale; staticView?: boolean; covered?: boolean } = $props();
  type Mode = 'auto' | 'paused' | 'scrub';
  let mode = $state<Mode>('auto');
  let phase = $state(0);
  let presentation = $state<HTMLDivElement>();
  let previousStage = -1;
  let manualUntil = 0;
  let visible = false;
  let sceneAnimation: Animation | undefined;
  const raw = $derived(storyState(progress));
  const displayed = $derived(staticView || mode === 'scrub' ? raw.local : phase);

  function setMode(next: Mode) {
    if (mode === 'scrub') phase = raw.local;
    mode = next;
    try { localStorage.setItem('roudamix-demo-mode', next); } catch { /* Storage is optional. */ }
  }

  $effect(() => {
    const next = storyState(progress);
    const node = presentation;
    untrack(() => {
      if (next.stage !== previousStage) {
        const direction = next.stage >= previousStage ? 1 : -1;
        phase = mode === 'scrub' || performance.now() < manualUntil ? next.local : 0;
        previousStage = next.stage;
        sceneAnimation?.cancel();
        if (node && !staticView && document.documentElement.dataset.motion !== 'reduced') {
          sceneAnimation = node.animate([
            { transform: `translate3d(0,${direction * 110}px,0) scale(.955) rotateX(${direction * 4}deg)`, opacity: 0.12, offset: 0 },
            { transform: `translate3d(0,${direction * -4}px,0) scale(1) rotateX(0deg)`, opacity: 1, offset: 0.82 },
            { transform: 'translate3d(0,0,0) scale(1) rotateX(0deg)', opacity: 1, offset: 1 },
          ], { duration: 960, easing: 'cubic-bezier(.22,1,.36,1)' });
        }
      } else if (mode === 'paused' || performance.now() < manualUntil) phase = next.local;
    });
  });

  onMount(() => {
    if (staticView) return;
    try {
      const saved = localStorage.getItem('roudamix-demo-mode');
      if (saved === 'scrub' || saved === 'paused') mode = saved;
    } catch { /* Animation does not depend on storage access. */ }
    let frame = 0;
    let last = performance.now();
    const tick = (now: number) => {
      const delta = Math.min(500, now - last);
      last = now;
      if (visible && !covered && !document.hidden && mode === 'auto' && now > manualUntil) phase = Math.min(0.98, phase + delta / 6800);
      frame = requestAnimationFrame(tick);
    };
    const observer = new IntersectionObserver(([entry]) => { visible = entry.isIntersecting; }, { threshold: 0.15 });
    observer.observe(presentation!);
    const fit = () => {
      if (!presentation) return;
      const scene = presentation.querySelector<HTMLElement>('.scene');
      const window = presentation.querySelector<HTMLElement>('.mix-window');
      if (!scene || !window) return;
      const style = getComputedStyle(scene);
      const available = scene.clientHeight - parseFloat(style.paddingTop) - parseFloat(style.paddingBottom);
      const ratio = innerWidth <= 760 ? Math.min(1, Math.max(0.5, available / window.scrollHeight)) : 1;
      presentation.style.setProperty('--window-fit', String(ratio));
    };
    const size = new ResizeObserver(fit);
    size.observe(presentation!);
    size.observe(presentation!.querySelector('.mix-window')!);
    fit();
    const scrub = () => { manualUntil = performance.now() + 450; };
    const arrive = () => { if (mode === 'auto') { phase = 0; manualUntil = 0; } };
    window.addEventListener('wheel', scrub, { passive: true });
    window.addEventListener('touchmove', scrub, { passive: true });
    window.addEventListener('roudamix:chapter-ready', arrive);
    frame = requestAnimationFrame(tick);
    return () => {
      cancelAnimationFrame(frame); observer.disconnect(); size.disconnect(); sceneAnimation?.cancel();
      window.removeEventListener('wheel', scrub); window.removeEventListener('touchmove', scrub); window.removeEventListener('roudamix:chapter-ready', arrive);
    };
  });
</script>

{#if staticView}
  <Scene {progress} {locale} staticView />
{:else}
  <div class="scene-presentation" bind:this={presentation} data-playback={mode} data-phase={displayed.toFixed(3)}>
    <Scene progress={(raw.stage + displayed) / 7} {locale} {covered} paused={mode === 'paused'} overviewPhase={raw.stage === 0 && mode !== 'scrub' ? displayed : undefined} />
    <div class="playback-toolbar">
      <span class="playback-status"><i></i>{locale === 'zh' ? (mode === 'scrub' ? '捲動控制' : '操作示範') : (mode === 'scrub' ? 'Scroll control' : 'Workflow demo')}</span>
      <button class="playback-toggle" aria-label={locale === 'zh' ? (mode === 'paused' ? '播放動畫' : '暫停動畫') : (mode === 'paused' ? 'Play animation' : 'Pause animation')} onclick={() => setMode(mode === 'paused' ? 'auto' : 'paused')}>{locale === 'zh' ? (mode === 'paused' ? '播放' : '暫停') : (mode === 'paused' ? 'Play' : 'Pause')}</button>
      <button class="playback-replay" onclick={() => { setMode('auto'); phase = 0; }}>{locale === 'zh' ? '重播' : 'Replay'}</button>
      <button class="playback-mode" aria-pressed={mode === 'scrub'} onclick={() => setMode(mode === 'scrub' ? 'auto' : 'scrub')}>{locale === 'zh' ? (mode === 'scrub' ? '自動演示' : '隨捲動') : (mode === 'scrub' ? 'Auto demo' : 'Scrub')}</button>
      <span class="playback-progress" aria-hidden="true" style={`transform:scaleX(${displayed})`}></span>
    </div>
  </div>
{/if}
