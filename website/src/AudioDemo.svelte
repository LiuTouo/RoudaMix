<script lang="ts">
  import { onMount } from 'svelte';
  import { DualAudio } from './audio';
  import { publication } from './config';
  import { content, type Locale } from './content';
  let { locale, active, suspended = false }: { locale: Locale; active: boolean; suspended?: boolean } = $props();
  const player = new DualAudio();
  let root: HTMLDivElement;
  let visible = $state(false);
  let playing = $state(false);
  let busy = $state(false);
  let error = $state(false);
  let selected = $state<0 | 1>(0);
  let position = $state(0);
  let request = 0;
  const t = $derived(content[locale]);

  function pauseDemo() {
    ++request;
    player.pause();
    playing = false;
    busy = false;
  }
  async function toggleDemo() {
    if (playing || busy) { pauseDemo(); return; }
    if (!publication.audio) return;
    const current = ++request;
    busy = true;
    error = false;
    try {
      await player.play(publication.audio);
      if (current === request) playing = player.playing;
    } catch {
      if (current === request) error = true;
    } finally {
      if (current === request) busy = false;
    }
  }
  $effect(() => { if (!active || !visible || suspended) pauseDemo(); });
  onMount(() => {
    const observer = new IntersectionObserver(([entry]) => { visible = entry.isIntersecting; });
    observer.observe(root);
    const timer = window.setInterval(() => { position = Math.max(0, player.position); playing = player.playing; }, 100);
    const hide = () => { if (document.hidden) pauseDemo(); };
    document.addEventListener('visibilitychange', hide);
    window.addEventListener('pagehide', pauseDemo);
    window.addEventListener('roudamix:navigate', pauseDemo);
    return () => { ++request; observer.disconnect(); clearInterval(timer); player.destroy(); document.removeEventListener('visibilitychange', hide); window.removeEventListener('pagehide', pauseDemo); window.removeEventListener('roudamix:navigate', pauseDemo); };
  });
</script>

<div class="audio-demo" bind:this={root}>
  {#if publication.audio}
    <div class="audio-controls">
      <button class="button small" disabled={suspended} onclick={toggleDemo}>{busy ? t.loading : playing ? t.pause : t.play}</button>
      <button disabled={suspended} aria-pressed={selected === 0} onclick={() => { selected = 0; player.select(0); }}>{t.monitor}</button>
      <button disabled={suspended} aria-pressed={selected === 1} onclick={() => { selected = 1; player.select(1); }}>{t.stream}</button>
    </div>
    <progress aria-label={t.position} value={position} max={player.duration || 1}></progress>
    <span class="mono">{position.toFixed(1)} s</span>
    {#if error}<p role="status">{t.audioError}</p>{/if}
  {:else}
    <p class="audio-pending">{t.audioSoon}</p>
  {/if}
</div>
