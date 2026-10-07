// 暫用比較工具；樣式切換只更改根節點屬性，不重新掛載 App。
import { inspectPreview, previewScenario } from './tauri';

const variants = [
  { key: 'original', name: '現行樣式', description: '目前主程式的外觀，作為比較基準' },
  { key: 'A', name: '直角色塊', description: '零圓角 · 純色色塊 · 以明暗區分層級' },
  { key: 'B', name: '細框幾何', description: '2px 圓角 · 細框界定區域 · 清楚的控制邊界' },
  { key: 'C', name: '連續分區', description: '連續底色 · 水平分隔 · 弱化獨立卡片' },
];

export function installStyleSwitcher() {
  const toolbar = document.createElement('aside');
  toolbar.className = 'prototype-toolbar';
  toolbar.setAttribute('data-ui-tool', '');
  toolbar.setAttribute('popover', 'manual');
  toolbar.setAttribute('aria-label', '風格預覽工具');
  toolbar.innerHTML = `
    <div class="prototype-row">
      <span class="prototype-mark">幾何扁平 / 原型</span>
      <button type="button" data-step="-1" aria-label="上一個風格" data-tooltip="上一個風格；切換列取得焦點時也可使用左方向鍵。">←</button>
      <div class="prototype-variants" role="group" aria-label="預覽版本"></div>
      <button type="button" data-step="1" aria-label="下一個風格" data-tooltip="下一個風格；切換列取得焦點時也可使用右方向鍵。">→</button>
      <button type="button" class="prototype-collapse" aria-expanded="true" data-tooltip="收合比較工具，減少遮擋。">收合</button>
    </div>
    <div class="prototype-expanded">
      <p class="prototype-description"></p>
      <div class="prototype-levels" aria-label="明暗層級：由底至上逐級變亮">
        <span>背景</span><span>面板</span><span>功能區</span><span>控制項／浮層</span><span>浮層內控制</span>
      </div>
      <p class="prototype-notice" role="status">固定範例 · 操作僅影響記憶體 · 重新整理可重設</p>
      <div class="prototype-scenarios"><button type="button" data-scenario="warning">顯示警示</button><button type="button" data-scenario="close">模擬關閉</button></div>
      <details class="prototype-state"><summary>查看模擬狀態</summary><pre></pre></details>
    </div>`;
  const group = toolbar.querySelector('.prototype-variants')!;
  const description = toolbar.querySelector('.prototype-description')!;
  const notice = toolbar.querySelector('.prototype-notice')!;
  const pre = toolbar.querySelector('pre')!;
  const details = toolbar.querySelector('details')!;
  const collapse = toolbar.querySelector<HTMLButtonElement>('.prototype-collapse')!;
  let current = 'A';
  let collapsed = false;

  function showState() {
    const state = { variant: current, ...inspectPreview() };
    pre.textContent = JSON.stringify(state, null, 2);
    notice.textContent = state.lastAction;
  }

  function select(key: string, updateUrl = true) {
    const variant = variants.find((v) => v.key === key) ?? variants[1];
    current = variant.key;
    document.documentElement.dataset.prototypeVariant = current;
    document.documentElement.dataset.uiStyle = 'c';
    document.documentElement.toggleAttribute('data-flat-preview', current !== 'original');
    if (updateUrl) {
      const url = new URL(location.href);
      url.searchParams.set('variant', current);
      history.replaceState(null, '', url);
    }
    for (const button of group.querySelectorAll('button')) {
      button.setAttribute('aria-pressed', String(button.dataset.variant === current));
    }
    description.textContent = `${variant.key === 'original' ? '' : variant.key + ' · '}${variant.name} — ${variant.description}`;
    toolbar.setAttribute('aria-label', `風格預覽工具：${variant.name}`);
    showState();
    console.info('[RoudaMix prototype]', { variant: current, ...inspectPreview() });
  }

  function step(delta: number) {
    const index = variants.findIndex((v) => v.key === current);
    select(variants[(index + delta + variants.length) % variants.length].key);
  }

  for (const variant of variants) {
    const button = document.createElement('button');
    button.type = 'button';
    button.dataset.variant = variant.key;
    button.dataset.tooltip = `${variant.name}：${variant.description}`;
    button.textContent = variant.key === 'original' ? '現行樣式' : `${variant.key} ${variant.name}`;
    button.onclick = () => select(variant.key);
    group.append(button);
  }
  for (const button of toolbar.querySelectorAll<HTMLButtonElement>('[data-step]')) {
    button.onclick = () => step(Number(button.dataset.step));
  }
  for (const button of toolbar.querySelectorAll<HTMLButtonElement>('[data-scenario]')) {
    button.onclick = () => previewScenario(button.dataset.scenario as 'close' | 'warning');
  }
  collapse.onclick = () => {
    collapsed = !collapsed;
    toolbar.classList.toggle('is-collapsed', collapsed);
    collapse.textContent = collapsed ? '展開' : '收合';
    collapse.setAttribute('aria-expanded', String(!collapsed));
    collapse.dataset.tooltip = collapsed ? '展開風格比較工具。' : '收合比較工具，減少遮擋。';
    if (collapsed) details.open = false;
  };
  // 點比較鈕不搶走來源欄位焦點，避免 DestinationSelect 的 focusin 關閉浮層。
  toolbar.onpointerdown = (event) => {
    if ((event.target as HTMLElement).closest('button')) event.preventDefault();
  };
  toolbar.onkeydown = (event) => {
    if (event.altKey || event.ctrlKey || event.metaKey || event.shiftKey ||
      (event.target as HTMLElement).closest('input, textarea, select, [contenteditable], pre')) return;
    if (event.key === 'ArrowLeft' || event.key === 'ArrowRight') {
      event.preventDefault();
      event.stopPropagation();
      step(event.key === 'ArrowLeft' ? -1 : 1);
    }
  };

  // 放到作用中的 modal 內以避開 inert，再進 top layer；固定位置不參與彈窗排版。
  function attachToActiveDialog() {
    const dialogs = [...document.querySelectorAll<HTMLDialogElement>('dialog:modal')];
    const parent = document.querySelector('.destination-popup:popover-open') ?? dialogs.at(-1) ?? document.body;
    if (toolbar.parentElement !== parent) {
      if (toolbar.matches(':popover-open')) toolbar.hidePopover();
      parent.append(toolbar);
      toolbar.showPopover();
    }
  }
  attachToActiveDialog();
  const observer = new MutationObserver(attachToActiveDialog);
  observer.observe(document.body, { subtree: true, childList: true, attributes: true, attributeFilter: ['open'] });
  document.addEventListener('toggle', attachToActiveDialog, true);
  const onPopState = () => select(new URL(location.href).searchParams.get('variant') ?? 'A', false);
  window.addEventListener('popstate', onPopState);
  window.addEventListener('prototype-state', showState);
  select(new URL(location.href).searchParams.get('variant') ?? 'A');

  return () => {
    observer.disconnect();
    document.removeEventListener('toggle', attachToActiveDialog, true);
    window.removeEventListener('popstate', onPopState);
    window.removeEventListener('prototype-state', showState);
    toolbar.remove();
    delete document.documentElement.dataset.prototypeVariant;
    document.documentElement.removeAttribute('data-flat-preview');
  };
}
