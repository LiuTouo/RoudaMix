<script lang="ts">
  import { onMount } from "svelte";
  import { getVersion } from "@tauri-apps/api/app";
  import { invoke } from "@tauri-apps/api/core";
  import { listen } from "@tauri-apps/api/event";
  import { checkRelease, type ReleaseUpdate } from "./updates";

  let { visible, autoCheck, onShow, onPreference, onAvailable }: {
    visible: boolean;
    autoCheck: boolean | undefined;
    onShow: () => void;
    onPreference: (enabled: boolean) => Promise<boolean>;
    onAvailable: (version: string) => void;
  } = $props();
  let version = $state("");
  let checking = $state(false);
  let saving = $state(false);
  let opening = $state(false);
  let message = $state("尚未檢查更新");
  let error = $state("");
  let update = $state<ReleaseUpdate | null>(null);
  let startupHandled = false;
  let disposed = false;
  let versionPromise: Promise<string> | null = null;

  function loadVersion() {
    versionPromise ??= getVersion().then((value) => (version = value)).catch((reason) => {
      versionPromise = null;
      throw reason;
    });
    return versionPromise;
  }

  async function check(automatic = false) {
    if (checking) return;
    checking = true;
    error = "";
    update = null;
    message = "正在檢查更新…";
    try {
      const result = await checkRelease(await loadVersion());
      if (disposed) return;
      update = result;
      message = result.available ? `有新版本 v${result.version} 可供下載` : "目前已是最新版本";
      if (automatic && result.available) onAvailable(result.version);
    } catch (reason) {
      if (disposed) return;
      message = "檢查更新失敗，可稍後重試";
      error = reason instanceof Error ? reason.message : String(reason);
    } finally {
      checking = false;
    }
  }

  async function changePreference(event: Event) {
    const input = event.currentTarget as HTMLInputElement;
    if (saving) return;
    saving = true;
    const requested = input.checked;
    try {
      if (!await onPreference(requested)) input.checked = autoCheck ?? false;
    } finally {
      saving = false;
    }
  }

  async function openDownload() {
    opening = true;
    error = "";
    try {
      await invoke("open_release_page");
    } catch {
      error = "無法開啟瀏覽器，請稍後重試";
    } finally {
      opening = false;
    }
  }

  $effect(() => {
    if (!startupHandled && autoCheck !== undefined && version) {
      startupHandled = true;
      if (autoCheck) void check(true);
    }
  });

  onMount(() => {
    disposed = false;
    let unlisten: (() => void) | undefined;
    void listen<boolean>("tray-about-requested", (event) => {
      onShow();
      if (event.payload) void check();
    }).then((cleanup) => {
      if (disposed) cleanup();
      else unlisten = cleanup;
    }).catch(() => { error = "系統匣更新事件無法連線，仍可手動檢查更新"; });
    void loadVersion().catch(() => { error = "無法讀取程式版本，請按檢查更新重試"; });
    return () => { disposed = true; unlisten?.(); };
  });
</script>

<section hidden={!visible} aria-label="關於與更新">
  <h2>RoudaMix</h2>
  <p class="version">版本 {version ? `v${version}` : "讀取中…"}</p>
  <div class="update-status" aria-live="polite" aria-busy={checking}>
    <p>{message}</p>
    {#if error}<p class="error" role="alert">{error}</p>{/if}
  </div>
  <div class="actions">
    <button disabled={checking} onclick={() => check()}>{checking ? "檢查中…" : "檢查更新"}</button>
    {#if update?.available}
      <button class="primary" disabled={opening} onclick={openDownload}>{opening ? "開啟中…" : "前往下載新版"}</button>
    {/if}
  </div>
  <label class="preference">
    <input type="checkbox" checked={autoCheck ?? false} disabled={saving || autoCheck === undefined}
      onchange={changePreference} />
    啟動時自動檢查更新
  </label>
  <p class="hint">有新版本時提示下載，由你決定何時安裝。</p>
</section>

<style>
  section { padding: 4px 0; }
  section[hidden] { display: none; }
  h2 { margin: 0 0 6px; font-size: 20px; }
  .version { color: var(--text-dim); font-family: var(--mono); }
  .update-status { margin: 16px 0; padding: 12px; background: var(--bg); border: 1px solid var(--border); border-radius: 4px; }
  .update-status p { margin: 0; }
  .update-status .error { margin-top: 8px; color: var(--err); overflow-wrap: anywhere; }
  .actions { display: flex; gap: 8px; flex-wrap: wrap; }
  .preference { display: flex; align-items: center; gap: 8px; margin-top: 20px; min-height: 32px; }
  .hint { color: var(--text-dim); font-size: 12px; margin: 4px 0; }
</style>
