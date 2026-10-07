<script lang="ts">
  // DEV data harness for the adopted C layout. App and styles are shared with production.
  // Only the dedicated DEV transport may mount this view; no real audio or file mutations.
  import { onMount } from 'svelte';
  import App from './App.svelte';
  import './console-prototype.css';

  const enabled = import.meta.env.DEV && import.meta.env.VITE_CONSOLE_PREVIEW;
  const channels = { audio: '#628FB5', app: '#8276A6', fx: '#69958C', output: '#A18A64' };
  let ready = $state(false);
  let error = $state('');
  let details = $state(false);
  let previewState = $state<ReturnType<typeof import('./lib/consolePrototypeTauri').inspectPreview> | null>(null);

  onMount(() => {
    if (!enabled) return;
    const url = new URL(location.href);
    url.searchParams.set('variant', 'C');
    history.replaceState(null, '', url);
    let cancelled = false;
    let stop: (() => void) | undefined;
    const changed = (event: Event) => { previewState = (event as CustomEvent).detail; };
    window.addEventListener('prototype-state', changed);
    void import('./lib/consolePrototypeTauri').then(api => {
      if (cancelled) return;
      api.setPreviewPalette(channels);
      previewState = api.inspectPreview();
      stop = api.startPreviewMeters();
      ready = true;
    }).catch(reason => { error = String(reason); });
    return () => { cancelled = true; stop?.(); window.removeEventListener('prototype-state', changed); };
  });
</script>

<svelte:head><title>RoudaMix · C 極簡操作</title></svelte:head>

{#if enabled}
  <div class="console-prototype" data-header-variant="C">
    <div class="project-app">
      {#if ready}<App />{:else}<p class="preview-loading" role="status">{error || '正在載入隔離的音訊控制台…'}</p>{/if}
    </div>
    <footer class="study-rail">
      <span class="study-identity">C / 極簡操作</span>
      <span class="study-notice" role="status">{previewState?.lastAction || '模擬資料 · 不連接真實音訊引擎'}</span>
      <button onclick={() => details = !details} aria-expanded={details} aria-controls="console-study-details">{details ? '關閉規格' : '規格 / 狀態'}</button>
    </footer>
    {#if details}
      <section id="console-study-details" class="study-details" aria-label="控制台設計規格與模擬狀態">
        <p>C 已套用正式介面：軌名只留底部識別色帶，上方保留來源、路由、側鏈、低延遲與 M/B。</p>
        <p>此處直接使用正式 App 與 console.css，維持左側推桿／音頻錶、右側全高 VST 機架；只有資料及測試工具列不同。</p>
        <p>檔案、VST 編輯器、硬體與系統操作維持隔離。</p>
        <pre>{JSON.stringify({ channels, ...previewState }, null, 2)}</pre>
      </section>
    {/if}
  </div>
{:else}
  <section class="console-preview-gate"><h1>C 極簡操作 · 隔離預覽</h1><p>請使用專用伺服器；此入口不會呼叫真實音訊與檔案操作。</p><pre>npm --prefix ui run prototype:console</pre><a href="http://127.0.0.1:5190/?prototype=console&variant=C">開啟控制台預覽 →</a></section>
{/if}
