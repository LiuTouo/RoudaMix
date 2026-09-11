// 暫用比較工具；樣式切換只更改根節點屬性，不重新掛載 App。
import { inspectPreview } from './tauri';

const variants = [
  { key: 'original', name: '原版', description: '目前工作區的原始外觀' },
  { key: 'C', name: '內嵌式機架・基準', description: '保留已選方向／原版配色／內框分隔' },
  { key: 'C1', name: '連續清單', description: '共用凹槽／細分隔線／輕量操作列' },
  { key: 'C2', name: '分段機架', description: '獨立內嵌插槽／操作區分組／小圓角' },
  { key: 'C3', name: '平整內框', description: '淺內框／平整插件列／整合標題區' },
];

export function installStyleSwitcher() {
  const toolbar = document.createElement('aside');
  toolbar.className = 'prototype-toolbar';
  toolbar.setAttribute('data-ui-tool', '');
  toolbar.setAttribute('popover', 'manual');
  toolbar.setAttribute('aria-label', '風格預覽工具');
  toolbar.innerHTML = `
    <div class="prototype-row">
      <span class="prototype-mark">風格預覽</span>
      <button type="button" data-step="-1" aria-label="上一個風格" data-tooltip="上一個風格；切換列取得焦點時也可使用左方向鍵。">←</button>
      <div class="prototype-variants" role="group" aria-label="預覽版本"></div>
      <button type="button" data-step="1" aria-label="下一個風格" data-tooltip="下一個風格；切換列取得焦點時也可使用右方向鍵。">→</button>
      <button type="button" class="prototype-collapse" aria-expanded="true" data-tooltip="收合比較工具，減少遮擋。">收合</button>
    </div>
    <div class="prototype-expanded">
      <p class="prototype-description"></p>
      <p class="prototype-notice" role="status">固定範例 · 操作僅影響記憶體 · 重新整理可重設</p>
      <details class="prototype-state"><summary>查看模擬狀態</summary><pre></pre></details>
    </div>`;
  const group = toolbar.querySelector('.prototype-variants')!;
  const description = toolbar.querySelector('.prototype-description')!;
  const notice = toolbar.querySelector('.prototype-notice')!;
  const pre = toolbar.querySelector('pre')!;
  const details = toolbar.querySelector('details')!;
  const collapse = toolbar.querySelector<HTMLButtonElement>('.prototype-collapse')!;
  let current = 'C';
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
    if (current.startsWith('C')) document.documentElement.dataset.uiStyle = 'c';
    else delete document.documentElement.dataset.uiStyle;
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
    button.textContent = variant.key === 'original' ? '原版' : `${variant.key} ${variant.name}`;
    button.onclick = () => select(variant.key);
    group.append(button);
  }
  for (const button of toolbar.querySelectorAll<HTMLButtonElement>('[data-step]')) {
    button.onclick = () => step(Number(button.dataset.step));
  }
  collapse.onclick = () => {
    collapsed = !collapsed;
    toolbar.classList.toggle('is-collapsed', collapsed);
    collapse.textContent = collapsed ? '展開' : '收合';
    collapse.setAttribute('aria-expanded', String(!collapsed));
    collapse.dataset.tooltip = collapsed ? '展開風格比較工具。' : '收合比較工具，減少遮擋。';
    if (collapsed) details.open = false;
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
    const parent = dialogs.at(-1) ?? document.body;
    if (toolbar.parentElement !== parent) {
      if (toolbar.matches(':popover-open')) toolbar.hidePopover();
      parent.append(toolbar);
      toolbar.showPopover();
    }
  }
  attachToActiveDialog();
  const observer = new MutationObserver(attachToActiveDialog);
  observer.observe(document.body, { subtree: true, childList: true, attributes: true, attributeFilter: ['open'] });
  const onPopState = () => select(new URL(location.href).searchParams.get('variant') ?? 'C', false);
  window.addEventListener('popstate', onPopState);
  window.addEventListener('prototype-state', showState);
  select(new URL(location.href).searchParams.get('variant') ?? 'C');

  return () => {
    observer.disconnect();
    window.removeEventListener('popstate', onPopState);
    window.removeEventListener('prototype-state', showState);
    toolbar.remove();
    delete document.documentElement.dataset.prototypeVariant;
  };
}
