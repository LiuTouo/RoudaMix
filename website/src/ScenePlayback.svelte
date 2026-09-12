<script lang="ts">
  import { onMount, untrack } from 'svelte';
  import Scene from './Scene.svelte';
  import { storyState } from './story';
  import type { Locale } from './content';

  let { progress, locale, covered = false, onphase }: { progress: number; locale: Locale; covered?: boolean; onphase?: (phase: number) => void } = $props();
  let phase = $state(0);
  let presentation = $state<HTMLDivElement>();
  let previousStage = -1;
  let manualUntil = 0;
  let scrubbing = false;
  let scrubTarget = 0;
  let visible = false;
  let sceneAnimation: Animation | undefined;
  const raw = $derived(storyState(progress));
  $effect(() => { onphase?.(phase); });

  $effect(() => {
    const next = storyState(progress);
    const node = presentation;
    untrack(() => {
      if (next.stage !== previousStage) {
        const direction = next.stage >= previousStage ? 1 : -1;
        const manual = performance.now() < manualUntil;
        phase = manual ? next.local : 0;
        scrubTarget = phase;
        if (!manual) scrubbing = false;
        previousStage = next.stage;
        sceneAnimation?.cancel();
        if (node) {
          sceneAnimation = node.animate([
            { transform: `translate3d(0,${direction * 110}px,0) scale(.955) rotateX(${direction * 4}deg)`, opacity: 0.12, offset: 0 },
            { transform: `translate3d(0,${direction * -4}px,0) scale(1) rotateX(0deg)`, opacity: 1, offset: 0.82 },
            { transform: 'translate3d(0,0,0) scale(1) rotateX(0deg)', opacity: 1, offset: 1 },
          ], { duration: 960, easing: 'cubic-bezier(.22,1,.36,1)' });
        }
      } else if (scrubbing) {
        scrubTarget = next.local;
      }
    });
  });

  onMount(() => {
    let frame = 0;
    let last = performance.now();
    const tick = (now: number) => {
      const delta = Math.min(500, now - last);
      last = now;
      if (visible && !covered && !document.hidden) {
        if (scrubbing) {
          const gap = scrubTarget - phase;
          // Lag behind the scroll for a smooth feel, then settle exactly on target.
          if (Math.abs(gap) < 0.01) phase = scrubTarget;
          else phase += gap * (1 - Math.exp(-delta / 110));
          if (now > manualUntil && phase === scrubTarget) scrubbing = false;
        } else {
          phase = Math.min(0.98, phase + delta / 6800);
        }
      }
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
    const scrub = () => { scrubbing = true; manualUntil = performance.now() + 450; };
    const arrive = () => { phase = 0; scrubTarget = 0; scrubbing = false; manualUntil = 0; };
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

<div class="scene-presentation" bind:this={presentation} data-phase={phase.toFixed(3)}>
  <Scene progress={(raw.stage + phase) / 7} {locale} {covered} overviewPhase={raw.stage === 0 ? phase : undefined} />
  <div class="playback-toolbar">
    <span class="playback-status"><i></i>{locale === 'zh' ? '操作示範' : 'Workflow demo'}</span>
    <span class="playback-progress" aria-hidden="true" style={`transform:scaleX(${phase})`}></span>
  </div>
</div>
