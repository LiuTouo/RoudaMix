// THROWAWAY：插件清單版面原型。variant ≠ original 時接管 PluginPicker 彈窗，
// 以同一份 buildPluginCatalog/browsePlugins 資料渲染 A–E 五種幾何；加入僅模擬。
import {
  buildPluginCatalog, browsePlugins,
  type CatalogPlugin, type PluginGroup, type PluginGrouping,
} from '../lib/pluginCatalog';
import { inspectPreview, previewNotice } from './tauri';

type SortKey = 'name' | 'source' | 'vendor' | 'type';

const state = {
  variant: 'original',
  query: '',
  grouping: 'name' as PluginGrouping,
  sort: { key: 'name' as SortKey, dir: 1 },
  selectedKey: '',
};
let dialog: HTMLDialogElement | null = null;
let trackName = '目前軌道';
// 模擬隔離清單：shim 永遠掃描成功，此處固定樣本讓各版面能評估隔離區塊的位置。
const simulatedFailures = [
  { path: 'C:/Prototype/VST3/broken-dll.vst3', error: '模擬：載入時存取違規' },
  { path: 'C:/Prototype/VST3/wrong-abi.vst3', error: '模擬：ABI 不相容' },
];

// 效果類型 → 固定色彩（B 色條／色點、C 卡片色帶用）。
const TYPE_COLORS: Record<string, string> = {
  EQ: '#4da3ff', Dynamics: '#ffb454', Reverb: '#3ddc84', Delay: '#b18cff',
  Distortion: '#ff5c5c', Modulation: '#ff7ad9', Spatial: '#4dd8e6', Mastering: '#ffd257',
  Restoration: '#7ee2a8', Analyzer: '#f5e642', 'Channel Strip': '#9aa7b8', Filter: '#6f8dff',
  'Pitch Shift': '#c99cff', Guitar: '#c98a5e', Bass: '#4f74e3', Drums: '#ff8f6b',
  Vocals: '#e66bcf', Generator: '#a8e05f', Microphone: '#b3b95c', Network: '#8fa3b8',
  Tools: '#98a2ad', 樂器: '#5fd38a', Surround: '#74c7ec', 未分類: '#6b7480',
};
function typeColor(plugin: CatalogPlugin): string {
  return TYPE_COLORS[plugin.categories.find((c) => TYPE_COLORS[c]) ?? ''] ?? '#6b7480';
}

const collator = new Intl.Collator('zh-TW', { numeric: true, sensitivity: 'base' });
function sortValue(plugin: CatalogPlugin, key: SortKey): string {
  return key === 'type' ? plugin.categories.join(' ') : plugin[key];
}
function sorted(list: CatalogPlugin[]): CatalogPlugin[] {
  const { key, dir } = state.sort;
  return [...list].sort((a, b) =>
    dir * collator.compare(sortValue(a, key), sortValue(b, key)) ||
    collator.compare(a.name, b.name));
}

function el<K extends keyof HTMLElementTagNameMap>(
  tag: K, className?: string, text?: string,
): HTMLElementTagNameMap[K] {
  const node = document.createElement(tag);
  if (className) node.className = className;
  if (text !== undefined) node.textContent = text;
  return node;
}
function tooltip(node: HTMLElement, text: string) {
  node.dataset.tooltip = text;
  return node;
}

// ---- 資料 ------------------------------------------------------------------

function catalogData(): CatalogPlugin[] {
  return buildPluginCatalog(inspectPreview().snapshot.lastScan ?? []);
}

// ---- 共用片段 ---------------------------------------------------------------

function groupHeader(group: PluginGroup) {
  if (!group.name) return null;
  const head = el('h2', undefined, group.name);
  head.append(el('span', undefined, String(group.plugins.length)));
  return head;
}

function sectionFor(group: PluginGroup, list: HTMLElement) {
  const section = el('section');
  if (group.name) section.setAttribute('aria-label', group.name);
  const head = groupHeader(group);
  if (head) section.append(head);
  section.append(list);
  return section;
}

function quarantineBlock() {
  const details = el('details', 'plist-quarantine');
  details.append(el('summary', undefined, `無法載入 (${simulatedFailures.length}) — 已隔離（模擬）`));
  for (const failure of simulatedFailures) {
    details.append(tooltip(el('p', undefined, `${failure.path}：${failure.error}`),
      `掃描失敗：${failure.error}\n${failure.path}`));
  }
  return details;
}

function simulatePick(plugin: CatalogPlugin) {
  previewNotice(`已模擬加入「${plugin.name}」→「${trackName}」（版面預覽不會掛載插件）`);
  closeDialog();
}

// ---- Variant A／E：表格幾何 --------------------------------------------------

function tableHeader(sortable: boolean) {
  const head = el('div', 'plist-thead');
  const columns: Array<{ key?: SortKey; label: string; className: string }> = [
    { key: 'name', label: '插件名稱', className: 'c-name' },
    { key: 'source', label: '來源', className: 'c-source' },
    { key: 'vendor', label: '廠牌', className: 'c-vendor' },
    { key: 'type', label: '類型', className: 'c-type' },
  ];
  for (const column of columns) {
    if (!sortable || !column.key) {
      head.append(el('span', column.className, column.label));
      continue;
    }
    const active = state.sort.key === column.key;
    const button = el('button', `c-name-sort ${column.className}`,
      `${column.label}${active ? (state.sort.dir === 1 ? ' ▲' : ' ▼') : ''}`);
    button.type = 'button';
    button.setAttribute('aria-label', `依${column.label}${active && state.sort.dir === -1 ? '（目前：遞減）' : '排序'}`);
    tooltip(button, `依${column.label}排序；再按一次反向。`);
    button.onclick = () => {
      state.sort = active
        ? { key: column.key!, dir: -state.sort.dir }
        : { key: column.key!, dir: 1 };
      renderResults();
    };
    head.append(button);
  }
  head.append(el('span', 'c-add', ''));
  return head;
}

function tableRow(plugin: CatalogPlugin) {
  const row = el('button', 'plist-trow');
  row.type = 'button';
  tooltip(row, `${plugin.path}\n${plugin.vendor} · ${plugin.version}`);
  row.onclick = () => simulatePick(plugin);
  const name = el('span', 'c-name');
  name.append(el('strong', undefined, plugin.name));
  const source = el('span', 'c-source', plugin.source);
  const vendor = el('span', 'c-vendor', plugin.vendor);
  const type = el('span', 'c-type', plugin.categories.join(' / '));
  const add = el('span', 'c-add', '加入');
  row.append(name, source, vendor, type, add);
  return row;
}

function renderTable(groups: PluginGroup[], sortable: boolean) {
  const wrap = el('div', sortable ? 'plist-tableA is-sortable' : 'plist-tableA');
  wrap.append(tableHeader(sortable));
  for (const group of groups) {
    const body = el('ul', 'plist-tbody');
    for (const plugin of sortable ? sorted(group.plugins) : group.plugins) {
      const item = el('li');
      item.append(tableRow(plugin));
      body.append(item);
    }
    wrap.append(sectionFor(group, body));
  }
  wrap.append(quarantineBlock());
  return wrap;
}

// ---- Variant B：色彩編碼緊湊列 ------------------------------------------------

function renderColorRows(groups: PluginGroup[]) {
  const wrap = el('div', 'plist-colorrows');
  for (const group of groups) {
    const body = el('ul');
    for (const plugin of group.plugins) {
      const color = typeColor(plugin);
      const row = el('button', 'plist-crow');
      row.type = 'button';
      row.style.setProperty('--type-color', color);
      tooltip(row, `${plugin.path}\n${plugin.vendor} · ${plugin.version}`);
      row.onclick = () => simulatePick(plugin);
      const dot = el('i', 'c-dot');
      dot.setAttribute('aria-hidden', 'true');
      const identity = el('span', 'c-identity');
      identity.append(el('strong', undefined, plugin.name),
        el('span', 'c-source', plugin.source));
      const meta = el('span', 'c-meta');
      meta.append(el('span', 'c-vendor', plugin.vendor));
      for (const category of plugin.categories) {
        meta.append(el('span', 'c-chip', category));
      }
      const add = el('span', 'c-add', '加入');
      row.append(dot, identity, meta, add);
      const item = el('li');
      item.append(row);
      body.append(item);
    }
    wrap.append(sectionFor(group, body));
  }
  wrap.append(quarantineBlock());
  return wrap;
}

// ---- Variant C：卡片網格 -----------------------------------------------------

function renderCards(groups: PluginGroup[]) {
  const wrap = el('div', 'plist-cards');
  for (const group of groups) {
    const grid = el('div', 'plist-grid');
    for (const plugin of group.plugins) {
      const color = typeColor(plugin);
      const card = el('button', 'plist-card');
      card.type = 'button';
      card.style.setProperty('--type-color', color);
      tooltip(card, `${plugin.path}\n${plugin.vendor} · ${plugin.version}`);
      card.onclick = () => simulatePick(plugin);
      const band = el('span', 'c-band');
      const dot = el('i', 'c-dot');
      dot.setAttribute('aria-hidden', 'true');
      band.append(dot, el('span', undefined, plugin.categories.join(' · ') || '未分類'),
        el('span', 'c-add', '加入'));
      const body = el('span', 'c-body');
      body.append(el('strong', undefined, plugin.name),
        el('span', 'c-meta', `${plugin.vendor} · ${plugin.source}`));
      card.append(band, body);
      grid.append(card);
    }
    wrap.append(sectionFor(group, grid));
  }
  wrap.append(quarantineBlock());
  return wrap;
}

// ---- Variant D：主從兩欄 -----------------------------------------------------

function renderSplit(groups: PluginGroup[]) {
  const matches = groups.flatMap((group) => group.plugins);
  if (!matches.some((plugin) => plugin.key === state.selectedKey)) {
    state.selectedKey = matches[0]?.key ?? '';
  }
  const wrap = el('div', 'plist-split');
  const nav = el('nav', 'plist-nav');
  nav.setAttribute('aria-label', '插件清單');
  const detail = el('section', 'plist-detail');
  detail.setAttribute('aria-label', '插件詳情');
  const drawDetail = () => {
    detail.replaceChildren();
    const plugin = matches.find((item) => item.key === state.selectedKey);
    if (!plugin) return;
    const head = el('h3', undefined, plugin.name);
    const facts = el('dl', 'plist-facts');
    const fact = (label: string, value: string, mono = false) => {
      facts.append(el('dt', undefined, label));
      facts.append(el('dd', mono ? 'is-mono' : undefined, value));
    };
    fact('廠牌', plugin.vendor);
    fact('版本', plugin.version);
    fact('類型', plugin.categories.join('、'));
    fact('來源', plugin.source);
    fact('完整路徑', plugin.path, true);
    const pick = el('button', 'plist-pick', `加入「${trackName}」`);
    pick.type = 'button';
    tooltip(pick, '模擬加入；版面預覽不會真的掛載插件。');
    pick.onclick = () => simulatePick(plugin);
    detail.append(head, facts, pick);
  };
  const syncSelection = () => {
    for (const row of nav.querySelectorAll<HTMLButtonElement>('.plist-drow')) {
      row.setAttribute('aria-current', String(row.dataset.key === state.selectedKey));
    }
    drawDetail();
  };
  for (const group of groups) {
    const body = el('ul');
    for (const plugin of group.plugins) {
      const row = el('button', 'plist-drow');
      row.type = 'button';
      row.dataset.key = plugin.key;
      row.style.setProperty('--type-color', typeColor(plugin));
      tooltip(row, `${plugin.vendor} · ${plugin.categories.join(' / ')}`);
      row.onclick = () => { state.selectedKey = plugin.key; syncSelection(); };
      const dot = el('i', 'c-dot');
      dot.setAttribute('aria-hidden', 'true');
      row.append(dot, el('span', 'c-name', plugin.name));
      const item = el('li');
      item.append(row);
      body.append(item);
    }
    nav.append(sectionFor(group, body));
  }
  wrap.append(nav, detail);
  nav.append(quarantineBlock());
  syncSelection();
  return wrap;
}

// ---- 彈窗骨架 ----------------------------------------------------------------

let resultsBox: HTMLElement | null = null;
let statusText: HTMLElement | null = null;

function renderResults() {
  if (!resultsBox || !statusText) return;
  const catalog = catalogData();
  const { count, groups } = browsePlugins(catalog, state.query, state.grouping);
  statusText.textContent = `${count} / ${catalog.length} 個插件`;
  if (catalog.length === 0) {
    resultsBox.replaceChildren(el('p', 'plist-empty', '尚無 VST 清單 — 請按上方「掃描 VST」'));
    return;
  }
  if (count === 0) {
    resultsBox.replaceChildren(el('p', 'plist-empty', '找不到符合條件的插件，請更換關鍵字或清除搜尋。'));
    return;
  }
  const body = state.variant === 'B' ? renderColorRows(groups)
    : state.variant === 'C' ? renderCards(groups)
    : state.variant === 'D' ? renderSplit(groups)
    : renderTable(groups, state.variant === 'E');
  resultsBox.replaceChildren(body);
}

function renderDialog() {
  if (!dialog) return;
  const head = el('div', 'plist-head');
  head.append(el('span', 'plist-title',
    `VST 插件列表 — 加入「${trackName}」`),
  );
  const close = el('button', 'plist-close', '×');
  close.type = 'button';
  close.setAttribute('aria-label', '關閉插件選擇器');
  tooltip(close, '關閉插件選擇器，不加入其他項目。');
  close.onclick = () => closeDialog();
  head.append(close);

  const toolbar = el('div', 'plist-toolbar');
  const field = el('label', 'plist-searchfield');
  field.append(el('span', undefined, '搜尋插件'));
  const search = el('input', 'plist-search') as HTMLInputElement;
  search.type = 'search';
  search.placeholder = '名稱、廠牌或分類，可輸入多個關鍵字';
  search.value = state.query;
  search.setAttribute('aria-label', '搜尋插件');
  search.oninput = () => { state.query = search.value; renderResults(); };
  field.append(search);
  const clear = el('button', 'plist-clear', '清除搜尋');
  clear.type = 'button';
  clear.disabled = !state.query;
  clear.onclick = () => {
    state.query = '';
    search.value = '';
    clear.disabled = true;
    search.focus();
    renderResults();
  };
  const groupingField = el('label', 'plist-grouping');
  groupingField.append(el('span', undefined, '分類方式'));
  const grouping = el('select') as HTMLSelectElement;
  grouping.setAttribute('aria-label', '分類方式');
  for (const [value, label] of [['name', '名稱'], ['vendor', '廠牌'], ['type', '效果類型']] as const) {
    const option = el('option', undefined, label) as HTMLOptionElement;
    option.value = value;
    grouping.append(option);
  }
  grouping.value = state.grouping;
  grouping.onchange = () => { state.grouping = grouping.value as PluginGrouping; renderResults(); };
  groupingField.append(grouping);
  const scan = el('button', 'plist-scan', '掃描 VST');
  scan.type = 'button';
  tooltip(scan, '原型固定使用記憶體目錄，不需重新掃描；請用下方切換列調整規模。');
  scan.onclick = () => previewNotice('原型已載入固定模擬目錄；規模請用切換列的 50/200/500。');
  toolbar.append(field, clear, groupingField, scan);

  const statusline = el('div', 'plist-statusline');
  statusText = el('span');
  statusText.setAttribute('role', 'status');
  statusline.append(statusText);

  resultsBox = el('div', 'plist-results');
  resultsBox.setAttribute('aria-label', '插件搜尋結果');

  dialog.replaceChildren(head, toolbar, statusline, resultsBox);
  renderResults();
}

function resetState() {
  state.query = '';
  state.grouping = 'name';
  state.sort = { key: 'name', dir: 1 };
  state.selectedKey = '';
}

function openDialog() {
  resetState();
  dialog = el('dialog', 'plist');
  dialog.setAttribute('aria-label', `VST 插件列表 — 加入「${trackName}」（版面預覽）`);
  dialog.addEventListener('close', () => { dialog?.remove(); dialog = null; });
  document.body.append(dialog);
  renderDialog();
  dialog.showModal();
  dialog.querySelector<HTMLInputElement>('.plist-search')?.focus();
}

function closeDialog() {
  if (!dialog) return;
  dialog.close(); // 仍開啟時同步觸發 close 事件，由 listener 收尾
  dialog?.remove(); // 已關閉的 dialog.close() 不會再觸發事件，手動補上
  dialog = null;
}

const observer = new MutationObserver(() => {
  if (state.variant === 'original' || dialog?.isConnected) return;
  const real = document.querySelector<HTMLDialogElement>('dialog.plugin-picker');
  if (!real || !real.open) return;
  trackName = /「(.+)」/.exec(real.getAttribute('aria-label') ?? '')?.[1] ?? '目前軌道';
  real.close(); // 觸發正式 onClose，讓 TrackStrip 狀態復位
  openDialog();
});

export function installPluginListPrototype() {
  // 切換列先於本模組安裝；若 URL 已帶 ?variant=，此處直接對齊。
  state.variant = new URL(location.href).searchParams.get('variant') ?? 'original';
  observer.observe(document.body, { childList: true, subtree: true, attributes: true, attributeFilter: ['open'] });
  const onVariant = (event: Event) => {
    state.variant = (event as CustomEvent<string>).detail;
    if (!dialog?.isConnected) return;
    if (state.variant === 'original') closeDialog();
    else renderResults();
  };
  const onState = () => { if (dialog?.isConnected) renderResults(); };
  window.addEventListener('prototype-variant', onVariant);
  window.addEventListener('prototype-state', onState);
  return () => {
    observer.disconnect();
    window.removeEventListener('prototype-variant', onVariant);
    window.removeEventListener('prototype-state', onState);
    closeDialog();
  };
}
