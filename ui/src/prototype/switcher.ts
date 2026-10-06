// 暫用比較工具；版本切換只更換插件清單版面，不重新掛載 App。
import { applyCatalogSize, inspectCatalogSize, inspectPreview } from './tauri';

const variants = [
  { key: 'original', name: '原版', description: '目前 PluginPicker 的原始清單，作為比較基準' },
  { key: 'A', name: '密集表格', description: '欄位對齊＋斑馬紋，同屏項目數最高' },
  { key: 'B', name: '色彩編碼緊湊列', description: '效果類型色條＋色點，單項辨識最強' },
  { key: 'C', name: '卡片網格', description: '2–3 欄卡片，掃視最快、密度最低' },
  { key: 'D', name: '主從兩欄', description: '左側極簡清單＋右側詳情窗' },
  { key: 'E', name: '可排序表格', description: '同 A 的表格幾何，欄頭可點擊排序' },
];
const sizes = [50, 200, 500];

export function installStyleSwitcher() {
  const toolbar = document.createElement('aside');
  toolbar.className = 'prototype-toolbar';
  toolbar.setAttribute('data-ui-tool', '');
  toolbar.setAttribute('popover', 'manual');
  toolbar.setAttribute('aria-label', '插件清單版面預覽');
  toolbar.innerHTML = `
    <div class="prototype-row">
      <span class="prototype-mark">清單版面</span>
      <button type="button" data-step="-1" aria-label="上一個版面" data-tooltip="上一個版面；切換列取得焦點時也可使用左方向鍵。">←</button>
      <div class="prototype-variants" role="group" aria-label="預覽版本"></div>
      <button type="button" data-step="1" aria-label="下一個版面" data-tooltip="下一個版面；切換列取得焦點時也可使用右方向鍵。">→</button>
      <span class="prototype-mark" aria-hidden="true">規模</span>
      <div class="prototype-scales" role="group" aria-label="模擬目錄規模"></div>
      <button type="button" class="prototype-collapse" aria-expanded="true" data-tooltip="收合比較工具，減少遮擋。">收合</button>
    </div>
    <div class="prototype-expanded">
      <p class="prototype-description"></p>
      <p class="prototype-notice" role="status">固定範例 · 操作僅影響記憶體 · 重新整理可重設</p>
      <details class="prototype-state"><summary>查看模擬狀態</summary><pre></pre></details>
    </div>`;
  const group = toolbar.querySelector('.prototype-variants')!;
  const scales = toolbar.querySelector('.prototype-scales')!;
  const description = toolbar.querySelector('.prototype-description')!;
  const notice = toolbar.querySelector('.prototype-notice')!;
  const pre = toolbar.querySelector('pre')!;
  const details = toolbar.querySelector('details')!;
  const collapse = toolbar.querySelector<HTMLButtonElement>('.prototype-collapse')!;
  let current = 'original';
  let collapsed = false;

  function showState() {
    const state = { variant: current, catalogSize: inspectCatalogSize(), ...inspectPreview() };
    pre.textContent = JSON.stringify(state, null, 2);
    notice.textContent = state.lastAction;
  }

  function select(key: string, updateUrl = true) {
    const variant = variants.find((v) => v.key === key) ?? variants[0];
    current = variant.key;
    document.documentElement.dataset.prototypeVariant = current;
    if (updateUrl) {
      const url = new URL(location.href);
      url.searchParams.set('variant', current);
      history.replaceState(null, '', url);
    }
    for (const button of group.querySelectorAll('button')) {
      button.setAttribute('aria-pressed', String(button.dataset.variant === current));
    }
    description.textContent = `${variant.key === 'original' ? '' : variant.key + ' · '}${variant.name} — ${variant.description}`;
    toolbar.setAttribute('aria-label', `插件清單版面預覽：${variant.name}`);
    window.dispatchEvent(new CustomEvent('prototype-variant', { detail: current }));
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
  for (const size of sizes) {
    const button = document.createElement('button');
    button.type = 'button';
    button.dataset.scale = String(size);
    button.dataset.tooltip = `把模擬目錄切換為 ${size} 個插件；所有版面使用同一份資料。`;
    button.textContent = String(size);
    button.onclick = () => {
      applyCatalogSize(size);
      for (const other of scales.querySelectorAll('button')) {
        other.setAttribute('aria-pressed', String(other.dataset.scale === String(size)));
      }
      showState();
    };
    scales.append(button);
  }
  for (const other of scales.querySelectorAll('button')) {
    other.setAttribute('aria-pressed', String(other.dataset.scale === String(inspectCatalogSize())));
  }
  for (const button of toolbar.querySelectorAll<HTMLButtonElement>('[data-step]')) {
    button.onclick = () => step(Number(button.dataset.step));
  }
  collapse.onclick = () => {
    collapsed = !collapsed;
    toolbar.classList.toggle('is-collapsed', collapsed);
    collapse.textContent = collapsed ? '展開' : '收合';
    collapse.setAttribute('aria-expanded', String(!collapsed));
    collapse.dataset.tooltip = collapsed ? '展開版面比較工具。' : '收合比較工具，減少遮擋。';
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
  const onPopState = () => select(new URL(location.href).searchParams.get('variant') ?? 'original', false);
  window.addEventListener('popstate', onPopState);
  window.addEventListener('prototype-state', showState);
  select(new URL(location.href).searchParams.get('variant') ?? 'original');

  return () => {
    observer.disconnect();
    window.removeEventListener('popstate', onPopState);
    window.removeEventListener('prototype-state', showState);
    toolbar.remove();
    delete document.documentElement.dataset.prototypeVariant;
  };
}
