<script lang="ts">
  // P2-M:右鍵選單(移到最前/最後/上移/下移)。原生 contextmenu 不全域攔 —— 只在
  // 有選單項的元素上開;空白處/錯誤文字的右鍵照常(可複製)。
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
    if (menu) {
      if (!dlg?.open) dlg?.showModal();
    } else if (dlg?.open) {
      dlg.close();
    }
  });

  function fire(item: { run: () => void }) {
    onClose();
    item.run();
  }
</script>

<svelte:window
  onkeydown={(e) => e.key === "Escape" && onClose()}
/>

{#if menu}
  <dialog
    bind:this={dlg}
    class="ctxdlg"
    style:left={Math.min(menu.x, window.innerWidth - 170) + "px"}
    style:top={Math.min(menu.y, window.innerHeight - 40 - (menu.items.length + 1) * 30) + "px"}
    onclose={onClose}
    onmousedown={(e) => e.target === dlg && onClose()}
  >
    <!-- svelte-ignore a11y_no_noninteractive_element_interactions -->
    <div class="ctx" role="menu" aria-label={menu.label}>
      {#each menu.items as item, i (i)}
        <button
          class="ctxitem"
          role="menuitem"
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
