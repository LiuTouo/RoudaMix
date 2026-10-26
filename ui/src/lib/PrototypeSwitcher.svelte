<script lang="ts">
  import { onMount } from 'svelte';

  let { variants, value = $bindable('A') }: {
    variants: { key: string; name: string }[];
    value?: string;
  } = $props();
  const index = $derived(Math.max(0, variants.findIndex(option => option.key === value)));

  function choose(next: number) {
    value = variants[(next + variants.length) % variants.length].key;
    const url = new URL(location.href);
    url.searchParams.set('variant', value);
    history.replaceState(null, '', url);
  }
  function keyboard(event: KeyboardEvent) {
    if (event.defaultPrevented || event.altKey || event.ctrlKey || event.metaKey || event.shiftKey) return;
    if (event.target instanceof Element && event.target.closest('input, textarea, select, button, a, [contenteditable], [role="slider"], [tabindex]')) return;
    if (event.key !== 'ArrowLeft' && event.key !== 'ArrowRight') return;
    event.preventDefault();
    choose(index + (event.key === 'ArrowRight' ? 1 : -1));
  }
  function readUrl() {
    const key = new URLSearchParams(location.search).get('variant');
    choose(Math.max(0, variants.findIndex(option => option.key === key)));
  }
  onMount(readUrl);
</script>

<svelte:window onkeydown={keyboard} onpopstate={readUrl} />

{#if import.meta.env.DEV}
  <nav class="prototype-switcher" aria-label="容器原型切換器">
    <button aria-label="上一個方案" data-tooltip="上一個方案（←）" onclick={() => choose(index - 1)}>←</button>
    <div class="variant-label" aria-live="polite"><small>LAYOUT PROTOTYPE · {index + 1} / {variants.length}</small><strong>{value} / {variants[index].name}</strong></div>
    <div class="variant-keys">{#each variants as option, i}<button aria-label={`方案 ${option.key}：${option.name}`} aria-pressed={value === option.key} onclick={() => choose(i)}>{option.key}</button>{/each}</div>
    <button aria-label="下一個方案" data-tooltip="下一個方案（→）" onclick={() => choose(index + 1)}>→</button>
  </nav>
{/if}

<style>
  .prototype-switcher { position: fixed; z-index: 50; bottom: 18px; left: 50%; transform: translateX(-50%); display: flex; align-items: center; gap: 12px; padding: 8px; width: max-content; max-width: calc(100vw - 24px); color: var(--text); background: var(--flat-2); border: 1px solid var(--accent); }
  :global(#app .prototype-switcher) { border-radius: 8px; box-shadow: 0 8px 32px #0009; }
  .variant-label { display: grid; gap: 2px; min-width: 172px; }
  small { font: 9px var(--mono); letter-spacing: 1px; color: var(--text-dim); }
  strong { font-size: 13px; font-weight: 500; }
  .variant-keys { display: flex; gap: 4px; }
  button { width: 36px; min-height: 40px; padding: 0; }
  :global(#app .prototype-switcher button[aria-pressed="true"]) { background: var(--accent); color: var(--flat-0); border-color: var(--accent); }
  button:focus-visible { outline: 2px solid var(--accent); outline-offset: 2px; }
  @media (max-width: 540px) { .variant-keys { display: none; } .prototype-switcher { gap: 8px; bottom: 10px; } }
</style>
