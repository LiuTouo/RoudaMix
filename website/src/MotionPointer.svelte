<script lang="ts">
  import { segment } from './workflowMotion';
  let { phase, label }: { phase: number; label: string } = $props();
  const arrival = $derived(segment(phase, 0, 0.48));
  const press = $derived(segment(phase, 0.48, 0.65) * (1 - segment(phase, 0.65, 0.83)));
  const ripple = $derived(segment(phase, 0.53, 1));
</script>

{#if phase > 0 && phase < 1}
  <span class="demo-pointer" aria-hidden="true" style={`opacity:${segment(phase, 0, 0.15) * (1 - segment(phase, 0.86, 1))}; transform:translate(${32 * (1 - arrival)}px,${24 * (1 - arrival)}px) scale(${1 - press * 0.22})`}>
    <i style={`transform:scale(${0.3 + ripple * 2}); opacity:${1 - ripple}`}></i>
    <svg viewBox="0 0 24 28"><path d="M2 2 20 16 12 17 9 25Z" /></svg>
    <b>{label}</b>
  </span>
{/if}
