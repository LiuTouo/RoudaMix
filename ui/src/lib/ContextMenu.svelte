<script lang="ts">
  // P2-M:右鍵選單(移到最前/最後/上移/下移)。原生 contextmenu 不全域攔 —— 只在
  // 有選單項的元素上開;空白處/錯誤文字的右鍵照常(可複製)。
  // 進場動畫用純 CSS @keyframes(.ctx 樣式),不用 Svelte transition —— 後者走 WAAPI,
  // jsdom 測試環境沒有 element.animate。
  let {
    menu,
    onClose,
  }: {
    menu: {
      x: number;
      y: number;
      label: string;
      items: Array<{ label: string; disabled?: boolean; run: () => void }>;
    } | null;
    onClose: () => void;
  } = $props();

  let dlg = $state<HTMLDialogElement | null>(null);
  $effect(() => {
    const dialog = dlg;
    if (!menu || !dialog) return;
    const opener = document.activeElement;
    if (!dialog.open) dialog.showModal();
    dialog.querySelector<HTMLButtonElement>('button:not(:disabled)')?.focus();
    return () => {
      const restore = dialog.contains(document.activeElement) || document.activeElement === document.body;
      dialog.close();
      if (restore && opener instanceof HTMLElement && opener.isConnected) opener.focus();
    };
  });

  function onMenuKeydown(event: KeyboardEvent) {
    if (event.key === "Tab") {
      dlg?.close();
      onClose();
      return;
    }
    if (event.altKey || event.ctrlKey || event.metaKey ||
        !["ArrowDown", "ArrowUp", "Home", "End"].includes(event.key)) return;
    event.preventDefault();
    const items = [...(dlg?.querySelectorAll<HTMLButtonElement>('button:not(:disabled)') ?? [])];
    if (!items.length) return;
    const index = items.indexOf(document.activeElement as HTMLButtonElement);
    const next = event.key === "Home" ? 0 : event.key === "End" ? items.length - 1
      : index < 0 ? (event.key === "ArrowUp" ? items.length - 1 : 0)
      : (index + (event.key === "ArrowDown" ? 1 : -1) + items.length) % items.length;
    items[next].focus();
  }

  function fire(item: { run: () => void }) {
    onClose();
    item.run();
  }
</script>

{#if menu}
  <dialog
    bind:this={dlg}
    class="ctxdlg"
    aria-label={menu.label}
    tabindex="-1"
    style:left={Math.min(menu.x, window.innerWidth - 170) + "px"}
    style:top={Math.min(menu.y, window.innerHeight - 40 - (menu.items.length + 1) * 30) + "px"}
    onclose={onClose}
    oncancel={(event) => { event.preventDefault(); onClose(); }}
    onmousedown={(e) => e.target === dlg && onClose()}
  >
    <!-- svelte-ignore a11y_no_noninteractive_element_interactions -->
    <!-- 選單本體 140ms 微下上浮淡入(純 CSS,見 .ctx);離場瞬間(桌面選單慣例)。
         一律播放,不連動 OS 動畫設定(ADR 0007) -->
    <div class="ctx" role="menu" tabindex="-1" aria-label={menu.label} onkeydown={onMenuKeydown}>
      {#each menu.items as item, i (i)}
        <button
          class="ctxitem"
          role="menuitem"
          tabindex="-1"
          disabled={item.disabled}
          onclick={() => fire(item)}
          >{item.label}</button
        >
      {/each}
    </div>
  </dialog>
{/if}

<style>
  .ctxdlg {
    background: none;
    border: none;
    padding: 0;
    margin: 0;
    overflow: visible;
    position: fixed;
    transform: none;
    inset: auto;
    max-width: none;
    max-height: none;
  }
  .ctxdlg[open] {
    display: block;
  }
  /* 所有項目皆 disabled 時 showModal 會把焦點留在 dialog；避免全域焦點框
     蓋過圓角選單外觀。可操作的 .ctxitem 仍保留標準藍色焦點框。 */
  .ctxdlg:focus-visible {
    outline: none;
  }
  .ctxdlg::backdrop {
    background: transparent; /* 不遮:點外關閉由 mousedown 處理 */
  }
  .ctx {
    min-width: 140px;
    display: flex;
    flex-direction: column;
    background: var(--bg-raised);
    border: 1px solid var(--border);
    border-radius: 6px;
    padding: 4px;
    box-shadow: 0 8px 24px rgb(0 0 0 / 0.55);
    /* 進場:微下上浮淡入;離場瞬間。一律播放,不連動 OS 動畫設定(ADR 0007) */
    animation: ctx-in var(--motion-dur-2) var(--motion-ease-out);
  }
  @keyframes ctx-in {
    from {
      opacity: 0;
      translate: 0 4px;
    }
  }
  .ctxitem {
    background: none;
    border: none;
    text-align: left;
    padding: 5px 10px;
    font-size: 13px;
    border-radius: 4px;
  }
  .ctxitem:hover:not(:disabled) {
    background: var(--bg);
    border-color: transparent;
  }
</style>
