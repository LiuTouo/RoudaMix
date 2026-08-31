<script lang="ts">
  // P2-N:破壞操作確認。不做事後 Undo(plugin instance state 無法安全還原 ——
  // 刪前確認列影響 + 刪除快照留在 caller,未來 Undo 以此為基礎)。
  let {
    confirm,
    onAnswer,
  }: {
    confirm: {
      title: string;
      impact: string[]; // 影響清單(逐條列給使用者看)
      confirmLabel: string;
    } | null;
    onAnswer: (yes: boolean) => void;
  } = $props();

  let dlg = $state<HTMLDialogElement | null>(null);
  $effect(() => {
    if (confirm) {
      if (!dlg?.open) dlg?.showModal();
    } else if (dlg?.open) {
      dlg.close();
    }
  });
</script>

<svelte:window onkeydown={(e) => e.key === "Escape" && confirm && onAnswer(false)} />

{#if confirm}
  <dialog bind:this={dlg} class="confirmdlg" onclose={() => onAnswer(false)}>
    <p class="q">{confirm.title}</p>
    {#if confirm.impact.length > 0}
      <ul class="impact">
        {#each confirm.impact as line, i (i)}
          <li>{line}</li>
        {/each}
      </ul>
    {/if}
    <div class="row">
      <span style="flex:1"></span>
      <button onclick={() => onAnswer(false)}>取消</button>
      <button class="danger" onclick={() => onAnswer(true)}>{confirm.confirmLabel}</button>
    </div>
  </dialog>
{/if}

<style>
  .confirmdlg {
    background: var(--bg-panel);
    color: var(--text);
    border: 1px solid var(--border);
    border-radius: 8px;
    padding: 16px 18px;
    width: min(400px, 90vw);
  }
  .confirmdlg[open] {
    display: flex;
    flex-direction: column;
    gap: 12px;
    position: fixed;
    top: 50%;
    left: 50%;
    transform: translate(-50%, -50%);
    margin: 0;
  }
  .confirmdlg::backdrop {
    background: rgb(0 0 0 / 0.5);
  }
  .q {
    margin: 0;
    font-size: 14px;
    font-weight: 600;
  }
  .impact {
    margin: 0;
    padding-left: 20px;
    color: var(--text-dim);
    font-size: 12.5px;
    display: flex;
    flex-direction: column;
    gap: 3px;
  }
  .row {
    display: flex;
    gap: 8px;
    justify-content: flex-end;
  }
  button.danger {
    background: var(--warn);
    color: #14161a;
    border-color: var(--warn);
    font-weight: 600;
  }
</style>
