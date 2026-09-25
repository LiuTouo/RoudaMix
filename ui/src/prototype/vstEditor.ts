// THROWAWAY：VST 編輯彈出視窗三種現代化樣式，掛在風格預覽 App 上，以 ?editor= 切換。
// A 裝置面板（扁平・直式滑桿列）／B 機架模組（硬體旋鈕・LCD）／C 玻璃浮動視窗（曲線主視覺・分頁）。
import { inspectPreview } from './tauri';

type Param = { label: string; min: number; max: number; value: number; unit: string; step?: number };

const PARAM_SETS: Record<string, Param[]> = {
  'Studio EQ': [
    { label: '頻率', min: 20, max: 20000, value: 1200, unit: 'Hz' },
    { label: '增益', min: -15, max: 15, value: 3.5, unit: 'dB', step: 0.1 },
    { label: 'Q 值', min: 0.1, max: 8, value: 0.9, unit: '', step: 0.1 },
    { label: '高通', min: 20, max: 500, value: 80, unit: 'Hz' },
    { label: '輸出', min: -12, max: 12, value: 0, unit: 'dB', step: 0.1 },
  ],
  'Voice Compressor': [
    { label: '門檻', min: -60, max: 0, value: -24, unit: 'dB' },
    { label: '比率', min: 1, max: 20, value: 4, unit: ':1', step: 0.1 },
    { label: '攻擊', min: 0.5, max: 100, value: 12, unit: 'ms', step: 0.1 },
    { label: '釋放', min: 10, max: 1000, value: 180, unit: 'ms' },
    { label: '補償', min: 0, max: 24, value: 4, unit: 'dB', step: 0.1 },
    { label: '混音', min: 0, max: 100, value: 100, unit: '%' },
  ],
  'Room Reverb': [
    { label: '空間大小', min: 5, max: 100, value: 42, unit: '%' },
    { label: '衰減', min: 100, max: 8000, value: 1800, unit: 'ms' },
    { label: '前置延遲', min: 0, max: 200, value: 24, unit: 'ms' },
    { label: '濕度', min: 0, max: 100, value: 32, unit: '%' },
    { label: '阻尼', min: 0, max: 100, value: 60, unit: '%' },
  ],
  'Tape Saturation': [
    { label: '驅動', min: 0, max: 100, value: 35, unit: '%' },
    { label: '偏壓', min: -100, max: 100, value: 10, unit: '%' },
    { label: '混音', min: 0, max: 100, value: 100, unit: '%' },
    { label: '輸出', min: -24, max: 6, value: -1.5, unit: 'dB', step: 0.1 },
  ],
  'Stereo Delay': [
    { label: '時間', min: 10, max: 2000, value: 320, unit: 'ms' },
    { label: '回饋', min: 0, max: 95, value: 38, unit: '%' },
    { label: '交錯', min: 0, max: 100, value: 55, unit: '%' },
    { label: '濕度', min: 0, max: 100, value: 25, unit: '%' },
  ],
  'Output Limiter': [
    { label: '門檻', min: -24, max: 0, value: -3, unit: 'dB', step: 0.1 },
    { label: '釋放', min: 20, max: 2000, value: 240, unit: 'ms' },
    { label: '天花板', min: -12, max: 0, value: -0.8, unit: 'dB', step: 0.1 },
  ],
};
const GENERIC_PARAMS: Param[] = [
  { label: '參數一', min: 0, max: 100, value: 50, unit: '%' },
  { label: '參數二', min: -24, max: 24, value: 0, unit: 'dB', step: 0.1 },
  { label: '參數三', min: 0, max: 100, value: 35, unit: '%' },
  { label: '參數四', min: 0, max: 100, value: 70, unit: '%' },
  { label: '輸出', min: -12, max: 12, value: 0, unit: 'dB', step: 0.1 },
];

const VARIANTS = [
  { key: 'A', name: '裝置面板', description: '扁平密實・直式滑桿列・單欄資訊階層' },
  { key: 'B', name: '機架模組', description: '硬體外觀・旋鈕陣列・LCD 預設列' },
  { key: 'C', name: '玻璃浮動', description: '半透明圓角・曲線主視覺・分頁切換' },
  { key: 'D', name: '抽屜分組', description: '摺疊抽屜・依單位分段・下拉預設' },
  { key: 'E', name: '數值瓷磚', description: '大字數值・無滑桿・瓷磚格＋電平柱' },
  { key: 'F', name: '停靠列', description: '超寬橫欄・微型插槽・右端電平' },
];

const state = {
  variant: 'A',
  plugin: 'Studio EQ',
  vendor: 'Rouda Audio',
  bypass: false,
  open: false,
  params: PARAM_SETS['Studio EQ'].map((p) => ({ ...p })),
};

function fmt(p: Param): string {
  const v = p.value;
  const text = p.unit === 'Hz' && v >= 1000 ? `${(v / 1000).toFixed(1)}kHz`
    : p.step ? v.toFixed(1) : String(Math.round(v));
  return p.unit === 'Hz' && v >= 1000 ? text : `${text}${p.unit}`;
}

function el<K extends keyof HTMLElementTagNameMap>(tag: K, className?: string, text?: string) {
  const node = document.createElement(tag);
  if (className) node.className = className;
  if (text !== undefined) node.textContent = text;
  return node;
}

function paramsFor(name: string) {
  return (PARAM_SETS[name] ?? GENERIC_PARAMS).map((p) => ({ ...p }));
}

function rackPlugins(): string[] {
  const names = inspectPreview().snapshot.tracks.flatMap((t) => t.plugins.map((p) => p.name));
  return [...new Set(names.length ? names : ['Studio EQ'])];
}

// ---- 共用：拖曳視窗 -------------------------------------------------------

function makeDraggable(handle: HTMLElement, win: HTMLElement) {
  let startX = 0, startY = 0, baseX = 0, baseY = 0;
  handle.addEventListener('pointerdown', (e) => {
    if ((e.target as HTMLElement).closest('button, input, select')) return;
    handle.setPointerCapture?.(e.pointerId);
    startX = e.clientX; startY = e.clientY;
    baseX = win.offsetLeft; baseY = win.offsetTop;
    const move = (ev: PointerEvent) => {
      win.style.left = `${baseX + ev.clientX - startX}px`;
      win.style.top = `${Math.max(0, baseY + ev.clientY - startY)}px`;
    };
    const up = () => {
      handle.removeEventListener('pointermove', move);
      handle.removeEventListener('pointerup', up);
    };
    handle.addEventListener('pointermove', move);
    handle.addEventListener('pointerup', up);
  });
}

// ---- 共用：垂直拖曳調整參數（旋鈕／瓷磚／插槽用） ------------------------------

function verticalDrag(node: HTMLElement, p: Param, draw: () => void) {
  node.addEventListener('pointerdown', (e) => {
    node.setPointerCapture?.(e.pointerId);
    const startY = e.clientY, start = p.value;
    const move = (ev: PointerEvent) => {
      const range = p.max - p.min;
      p.value = Math.min(p.max, Math.max(p.min, start + (startY - ev.clientY) * range / 160));
      draw();
      logChange(p);
    };
    const up = () => {
      node.removeEventListener('pointermove', move);
      node.removeEventListener('pointerup', up);
    };
    node.addEventListener('pointermove', move);
    node.addEventListener('pointerup', up);
  });
}

// ---- 共用：旋鈕（B 用） ---------------------------------------------------

function makeKnob(p: Param) {
  const wrap = el('div', 'vst-knob');
  wrap.innerHTML = `<svg viewBox="0 0 64 64" aria-hidden="true">
    <circle class="vst-knob-body" cx="32" cy="32" r="24"/>
    <path class="vst-knob-arc" fill="none"/>
    <line class="vst-knob-pointer" x1="32" y1="32" x2="32" y2="12"/>
  </svg>`;
  const arc = wrap.querySelector<SVGPathElement>('.vst-knob-arc')!;
  const pointer = wrap.querySelector<SVGLineElement>('.vst-knob-pointer')!;
  const label = el('span', 'vst-knob-label', p.label);
  const readout = el('output', 'vst-knob-value', fmt(p));
  wrap.append(label, readout);

  function draw() {
    const ratio = (p.value - p.min) / (p.max - p.min);
    const angle = -135 + ratio * 270;
    const rad = (angle - 90) * Math.PI / 180;
    const end = { x: 32 + 27 * Math.cos(rad), y: 32 + 27 * Math.sin(rad) };
    const large = ratio > 0.5 ? 1 : 0;
    const a0 = (-225 - 90) * Math.PI / 180;
    arc.setAttribute('d', ratio <= 0.005 ? '' :
      `M ${32 + 27 * Math.cos(a0)} ${32 + 27 * Math.sin(a0)} A 27 27 0 ${large} 1 ${end.x} ${end.y}`);
    pointer.setAttribute('transform', `rotate(${angle} 32 32)`);
    readout.value = fmt(p);
  }
  draw();

  verticalDrag(wrap, p, draw);
  return wrap;
}

// ---- Variant A：扁平裝置面板 ---------------------------------------------

function buildVariantA(win: HTMLElement) {
  const body = el('div', 'vstA-body');
  const grid = el('div', 'vstA-grid');
  for (const p of state.params) {
    const row = el('div', 'vstA-row');
    const name = el('label', 'vstA-label', p.label);
    const slider = el('input', 'vstA-slider') as HTMLInputElement;
    slider.type = 'range'; slider.min = String(p.min); slider.max = String(p.max);
    slider.step = String(p.step ?? 1); slider.value = String(p.value);
    const readout = el('output', 'vstA-value', fmt(p));
    name.append(slider);
    slider.setAttribute('aria-label', p.label);
    slider.addEventListener('input', () => {
      p.value = Number(slider.value);
      readout.value = fmt(p);
      logChange(p);
    });
    row.append(name, slider, readout);
    grid.append(row);
  }
  body.append(grid);
  return body;
}

// ---- Variant B：硬體機架模組 ---------------------------------------------

function buildVariantB(win: HTMLElement) {
  const body = el('div', 'vstB-body');
  const leds = el('div', 'vstB-leds');
  const meter = el('div', 'vstB-led-meter');
  for (let i = 0; i < 12; i++) meter.append(el('i'));
  leds.append(el('span', 'vstB-led-name', 'IN'), meter, el('span', 'vstB-led-name', 'OUT'));
  body.append(leds);

  const knobs = el('div', 'vstB-knobs');
  for (const p of state.params) knobs.append(makeKnob(p));
  body.append(knobs);

  const jacks = el('div', 'vstB-jacks');
  for (const name of ['IN L', 'IN R', 'OUT L', 'OUT R']) {
    const jack = el('div', 'vstB-jack');
    jack.append(el('i'), el('span', undefined, name));
    jacks.append(jack);
  }
  body.append(jacks);
  return body;
}

// ---- Variant C：玻璃浮動視窗 ---------------------------------------------

function buildVariantC(win: HTMLElement) {
  const body = el('div', 'vstC-body');

  const hero = el('div', 'vstC-hero');
  hero.innerHTML = `<svg viewBox="0 0 560 150" preserveAspectRatio="none" aria-hidden="true">
    <defs><linearGradient id="vstC-fill" x1="0" y1="0" x2="0" y2="1">
      <stop offset="0" stop-color="#4da3ff" stop-opacity=".45"/>
      <stop offset="1" stop-color="#4da3ff" stop-opacity="0"/>
    </linearGradient></defs>
    <path class="vstC-curve" fill="url(#vstC-fill)" stroke="#7cc0ff" stroke-width="2"
      d="M0,140 C60,138 90,132 130,120 C180,104 200,40 250,38 C300,36 322,96 380,116
         C440,134 500,138 560,140 L560,150 L0,150 Z"/>
  </svg>`;
  const spectrum = el('div', 'vstC-spectrum');
  for (let i = 0; i < 28; i++) {
    const bar = el('i');
    bar.style.animationDelay = `${(i % 7) * 0.13}s`;
    bar.style.animationDuration = `${0.9 + (i % 5) * 0.17}s`;
    spectrum.append(bar);
  }
  hero.append(spectrum);
  body.append(hero);

  const tabs = el('div', 'vstC-tabs');
  const panes = el('div', 'vstC-panes');
  const paramPane = el('div', 'vstC-pane');
  const cards = el('div', 'vstC-cards');
  for (const p of state.params) {
    const card = el('div', 'vstC-card');
    const head = el('div', 'vstC-card-head');
    const name = el('span', undefined, p.label);
    const readout = el('output', 'vstC-pill', fmt(p));
    head.append(name, readout);
    const slider = el('input') as HTMLInputElement;
    slider.type = 'range'; slider.min = String(p.min); slider.max = String(p.max);
    slider.step = String(p.step ?? 1); slider.value = String(p.value);
    slider.setAttribute('aria-label', p.label);
    slider.addEventListener('input', () => {
      p.value = Number(slider.value);
      readout.value = fmt(p);
      logChange(p);
    });
    card.append(head, slider);
    cards.append(card);
  }
  paramPane.append(cards);

  const infoPane = el('div', 'vstC-pane');
  const rows: [string, string][] = [
    ['廠牌', state.vendor], ['格式', 'VST3 · 64-bit'],
    ['延遲', '64 samples（已納入 PDC）'], ['負載', '0.8% CPU'],
    ['路徑', 'C:/Prototype/VST3/StudioEQ.vst3'],
  ];
  const info = el('dl', 'vstC-info');
  for (const [k, v] of rows) {
    info.append(el('dt', undefined, k), el('dd', undefined, v));
  }
  infoPane.append(info);

  const buttons = [el('button', undefined, '參數'), el('button', undefined, '資訊')] as [HTMLButtonElement, HTMLButtonElement];
  buttons[0].type = buttons[1].type = 'button';
  function showPane(index: number) {
    paramPane.hidden = index !== 0;
    infoPane.hidden = index !== 1;
    for (const [i, b] of buttons.entries()) b.setAttribute('aria-pressed', String(i === index));
  }
  buttons[0].onclick = () => showPane(0);
  buttons[1].onclick = () => showPane(1);
  showPane(0);
  tabs.append(...buttons);
  panes.append(paramPane, infoPane);
  body.append(tabs, panes);
  return body;
}

// ---- Variant D：抽屜分組（依參數單位分段，C2 語彙） -----------------------------

function groupByUnit(params: Param[]): [string, Param[]][] {
  const groups = new Map<string, Param[]>();
  for (const p of params) {
    const key = p.unit === 'Hz' ? '頻率' : p.unit === 'ms' ? '時間'
      : p.unit === 'dB' ? '電平' : p.unit === '%' ? '混合' : '其他';
    if (!groups.has(key)) groups.set(key, []);
    groups.get(key)!.push(p);
  }
  return [...groups.entries()];
}

function buildVariantD() {
  const body = el('div', 'vstD-body');

  const presetRow = el('div', 'vstD-preset');
  const select = el('select') as HTMLSelectElement;
  select.setAttribute('aria-label', '預設');
  for (const name of ['Init', 'Vocal', 'Bus Glue', 'Bright', 'Warm']) {
    const option = el('option', undefined, name) as HTMLOptionElement;
    option.value = name;
    select.append(option);
  }
  select.onchange = () => console.info('[RoudaMix prototype/editor]', { plugin: state.plugin, preset: select.value });
  const compare = el('button', undefined, 'A／B 對比');
  compare.type = 'button';
  compare.dataset.tooltip = 'A／B 對比僅供示意，不影響參數值。';
  compare.onclick = () => compare.classList.toggle('is-on');
  presetRow.append(select, compare);
  body.append(presetRow);

  for (const [i, [name, list]] of groupByUnit(state.params).entries()) {
    const drawer = el('details', 'vstD-drawer');
    if (i === 0) drawer.open = true;
    drawer.append(el('summary', undefined, `${name}　${list.length} 項`));
    const rows = el('div', 'vstD-rows');
    for (const p of list) {
      const row = el('div', 'vstD-row');
      const slider = el('input', 'vstA-slider') as HTMLInputElement;
      slider.type = 'range'; slider.min = String(p.min); slider.max = String(p.max);
      slider.step = String(p.step ?? 1); slider.value = String(p.value);
      slider.setAttribute('aria-label', p.label);
      const readout = el('output', 'vstA-value', fmt(p));
      slider.addEventListener('input', () => {
        p.value = Number(slider.value);
        readout.value = fmt(p);
        logChange(p);
      });
      row.append(el('span', 'vstD-label', p.label), slider, readout);
      rows.append(row);
    }
    drawer.append(rows);
    body.append(drawer);
  }
  return body;
}

// ---- Variant E：數值瓷磚（無滑桿，拖曳數字或 ± 微調） ---------------------------

function buildVariantE() {
  const body = el('div', 'vstE-body');
  const grid = el('div', 'vstE-grid');
  for (const p of state.params) {
    const tile = el('div', 'vstE-tile');
    const value = el('output', 'vstE-value', fmt(p));
    verticalDrag(tile, p, () => { value.value = fmt(p); });
    const stepSize = p.step ?? (p.max - p.min) / 100;
    const stepper = el('div', 'vstE-steps');
    for (const [symbol, delta] of [['−', -1], ['+', 1]] as const) {
      const button = el('button', undefined, symbol);
      button.type = 'button';
      button.setAttribute('aria-label', `${p.label}${delta < 0 ? '減少' : '增加'}`);
      button.dataset.tooltip = `${p.label}${delta < 0 ? '減少' : '增加'}一階。`;
      button.onclick = () => {
        p.value = Math.min(p.max, Math.max(p.min, p.value + delta * stepSize));
        value.value = fmt(p);
        logChange(p);
      };
      stepper.append(button);
    }
    tile.append(el('span', 'vstE-label', p.label), value, stepper);
    grid.append(tile);
  }
  const meters = el('div', 'vstE-meters');
  for (const [name, kind] of [['IN', 'in'], ['GR', 'gr'], ['OUT', 'in']] as const) {
    const meter = el('div', 'vstE-meter');
    meter.innerHTML = `<div class="vstE-track ${kind}"><i></i></div><span>${name}</span>`;
    meters.append(meter);
  }
  body.append(grid, meters);
  return body;
}

// ---- Variant F：停靠列（超寬橫欄，微型插槽） ----------------------------------

function buildVariantF() {
  const body = el('div', 'vstF-body');
  const slots = el('div', 'vstF-slots');
  for (const p of state.params) {
    const slot = el('div', 'vstF-slot');
    const head = el('div', 'vstF-slot-head');
    const readout = el('output', 'vstF-value', fmt(p));
    head.append(el('span', undefined, p.label), readout);
    const fill = el('i');
    const bar = el('div', 'vstF-bar');
    bar.append(fill);
    verticalDrag(slot, p, () => {
      readout.value = fmt(p);
      fill.style.width = `${((p.value - p.min) / (p.max - p.min)) * 100}%`;
    });
    fill.style.width = `${((p.value - p.min) / (p.max - p.min)) * 100}%`;
    slot.append(head, bar);
    slots.append(slot);
  }
  const meters = el('div', 'vstF-meters');
  for (const name of ['IN L', 'IN R', 'OUT L', 'OUT R']) {
    const meter = el('div', 'vstF-meter');
    meter.innerHTML = `<span>${name}</span><div><i></i></div>`;
    meters.append(meter);
  }
  body.append(slots, meters);
  return body;
}

// ---- 視窗骨架（標題列依 variant 換膚） ------------------------------------

function logChange(p: Param) {
  console.info('[RoudaMix prototype/editor]', { plugin: state.plugin, param: p.label, value: p.value });
}

function buildWindow(): HTMLElement {
  const win = el('section', `vst-window vst-${state.variant.toLowerCase()}`);
  win.setAttribute('aria-label', `VST 編輯器預覽：${state.plugin}`);

  const bar = el('header', 'vst-titlebar');

  const power = el('button', 'vst-power');
  power.type = 'button';
  power.setAttribute('aria-label', '啟用／旁路');
  power.dataset.tooltip = '切換旁路；預覽僅改變視覺狀態。';
  power.setAttribute('aria-pressed', String(!state.bypass));
  power.textContent = '⏻';
  power.onclick = () => {
    state.bypass = !state.bypass;
    win.classList.toggle('is-bypassed', state.bypass);
    power.setAttribute('aria-pressed', String(!state.bypass));
    console.info('[RoudaMix prototype/editor]', { plugin: state.plugin, bypass: state.bypass });
  };

  const titles = el('div', 'vst-titles');
  const name = el('strong', 'vst-name', state.plugin);
  const sub = el('span', 'vst-sub', `${state.vendor} · VST3 · 延遲 64 samples`);
  titles.append(name, sub);

  const preset = el('div', 'vst-preset');
  preset.innerHTML = `
    <button type="button" data-dir="-1" aria-label="上一個預設" data-tooltip="上一個預設（僅示意）。">◀</button>
    <span class="vst-preset-name">Init</span>
    <button type="button" data-dir="1" aria-label="下一個預設" data-tooltip="下一個預設（僅示意）。">▶</button>`;
  const presetName = preset.querySelector('.vst-preset-name')!;
  const presets = ['Init', 'Vocal', 'Bus Glue', 'Bright', 'Warm'];
  let presetIndex = 0;
  for (const b of preset.querySelectorAll<HTMLButtonElement>('[data-dir]')) {
    b.onclick = () => {
      presetIndex = (presetIndex + Number(b.dataset.dir) + presets.length) % presets.length;
      presetName.textContent = presets[presetIndex];
      console.info('[RoudaMix prototype/editor]', { plugin: state.plugin, preset: presets[presetIndex] });
    };
  }

  const close = el('button', 'vst-close');
  close.type = 'button';
  close.setAttribute('aria-label', '關閉編輯器');
  close.dataset.tooltip = '關閉編輯器視窗；可由右下工具列重新開啟。';
  close.textContent = '✕';

  bar.append(power, titles);
  if (state.variant === 'C') bar.append(preset);
  bar.append(close);
  if (state.variant !== 'C') bar.append(preset);
  // C 把預設列放主體上方，B/A 放標題列；此處順序已符合各 variant 樣式。

  const holder = el('div', 'vst-content');
  const body = state.variant === 'A' ? buildVariantA(win)
    : state.variant === 'B' ? buildVariantB(win)
    : state.variant === 'D' ? buildVariantD()
    : state.variant === 'E' ? buildVariantE()
    : state.variant === 'F' ? buildVariantF()
    : buildVariantC(win);
  if (state.variant === 'C') {
    const cWrap = el('div', 'vstC-wrap');
    cWrap.append(preset, body);
    holder.append(cWrap);
  } else holder.append(body);

  win.append(bar, holder);
  makeDraggable(bar, win);
  close.onclick = () => {
    state.open = false;
    win.hidden = true;
    syncToolbar();
  };
  return win;
}

// ---- 工具列 + 掛載 --------------------------------------------------------

let toolbar: HTMLElement | null = null;
let windowEl: HTMLElement | null = null;
let description: HTMLElement | null = null;
let pluginSelect: HTMLSelectElement | null = null;
let openButton: HTMLButtonElement | null = null;

function showVariant(key: string, updateUrl = true) {
  const variant = VARIANTS.find((v) => v.key === key) ?? VARIANTS[0];
  state.variant = variant.key;
  state.params = paramsFor(state.plugin);
  state.bypass = false;
  if (windowEl) windowEl.remove();
  windowEl = buildWindow();
  document.body.append(windowEl);
  windowEl.hidden = !state.open;
  if (updateUrl) {
    const url = new URL(location.href);
    url.searchParams.set('editor', variant.key);
    history.replaceState(null, '', url);
  }
  if (description) {
    description.textContent = `${variant.key} ${variant.name} — ${variant.description}`;
  }
  for (const button of toolbar?.querySelectorAll('.prototype-variants button') ?? []) {
    button.setAttribute('aria-pressed', String((button as HTMLElement).dataset.editor === variant.key));
  }
  console.info('[RoudaMix prototype]', { editor: variant.key, name: variant.name, plugin: state.plugin });
}

function syncToolbar() {
  if (!openButton) return;
  openButton.textContent = state.open ? '隱藏視窗' : '開啟視窗';
  openButton.dataset.tooltip = state.open ? '隱藏編輯器視窗。' : '在畫面中央開啟目前的編輯器樣式。';
}

function refreshPlugins() {
  if (!pluginSelect) return;
  const names = rackPlugins();
  pluginSelect.replaceChildren(...names.map((name) => {
    const option = el('option', undefined, name) as HTMLOptionElement;
    option.value = name;
    return option;
  }));
  if (!names.includes(state.plugin)) state.plugin = names[0];
  pluginSelect.value = state.plugin;
}

function step(delta: number) {
  const index = VARIANTS.findIndex((v) => v.key === state.variant);
  showVariant(VARIANTS[(index + delta + VARIANTS.length) % VARIANTS.length].key);
}

export function installVstEditorPrototype() {
  toolbar = el('aside', 'prototype-toolbar vst-editor-toolbar');
  toolbar.setAttribute('data-ui-tool', '');
  toolbar.setAttribute('popover', 'manual');
  toolbar.setAttribute('aria-label', 'VST 編輯器樣式預覽');
  toolbar.innerHTML = `
    <div class="prototype-row">
      <span class="prototype-mark">編輯器預覽</span>
      <button type="button" data-step="-1" aria-label="上一個編輯器樣式" data-tooltip="上一個樣式；工具列取得焦點時也可使用左方向鍵。">←</button>
      <div class="prototype-variants" role="group" aria-label="編輯器樣式"></div>
      <button type="button" data-step="1" aria-label="下一個編輯器樣式" data-tooltip="下一個樣式；工具列取得焦點時也可使用右方向鍵。">→</button>
      <select class="vst-editor-plugin" aria-label="選擇插件"></select>
      <button type="button" class="vst-editor-open" data-tooltip="開啟編輯器視窗。">開啟視窗</button>
    </div>
    <p class="prototype-description"></p>
    <p class="prototype-notice" role="status">固定範例 · 參數僅存於記憶體 · 重新整理可重設</p>`;
  description = toolbar.querySelector<HTMLElement>('.prototype-description');
  const group = toolbar.querySelector<HTMLElement>('.prototype-variants')!;
  for (const variant of VARIANTS) {
    const button = document.createElement('button');
    button.type = 'button';
    button.dataset.editor = variant.key;
    button.dataset.tooltip = `${variant.name}：${variant.description}`;
    button.textContent = variant.key;
    button.setAttribute('aria-label', `${variant.key} ${variant.name}`);
    button.onclick = () => showVariant(variant.key);
    group.append(button);
  }
  pluginSelect = toolbar.querySelector<HTMLSelectElement>('.vst-editor-plugin');
  openButton = toolbar.querySelector<HTMLButtonElement>('.vst-editor-open');
  for (const b of toolbar.querySelectorAll<HTMLButtonElement>('[data-step]')) {
    b.onclick = () => step(Number(b.dataset.step));
  }
  pluginSelect!.onchange = () => {
    state.plugin = pluginSelect!.value;
    state.vendor = state.plugin.includes('EQ') ? 'Rouda Audio'
      : state.plugin.includes('Reverb') || state.plugin.includes('Tape') ? 'Northern Sound' : 'Signal Works';
    refreshPlugins();
    showVariant(state.variant, false);
    state.open = true;
    windowEl!.hidden = false;
    syncToolbar();
  };
  openButton!.onclick = () => {
    state.open = !state.open;
    if (windowEl) windowEl.hidden = !state.open;
    syncToolbar();
  };
  toolbar.onkeydown = (event) => {
    if (event.altKey || event.ctrlKey || event.metaKey || event.shiftKey ||
      (event.target as HTMLElement).closest('input, textarea, select, [contenteditable]')) return;
    if (event.key === 'ArrowLeft' || event.key === 'ArrowRight') {
      event.preventDefault();
      event.stopPropagation();
      step(event.key === 'ArrowLeft' ? -1 : 1);
    }
  };
  document.body.append(toolbar);
  // popover 屬性未 showPopover() 前是 display:none；與風格列一樣推進頂層。
  toolbar.showPopover?.();

  const onPopState = () => showVariant(new URL(location.href).searchParams.get('editor') ?? 'A', false);
  window.addEventListener('popstate', onPopState);
  const onOpenEditor = (event: Event) => {
    const detail = (event as CustomEvent<{ name?: string }>).detail;
    if (detail?.name) {
      state.plugin = detail.name;
      refreshPlugins();
    }
    showVariant(state.variant, false);
    state.open = true;
    windowEl!.hidden = false;
    syncToolbar();
  };
  window.addEventListener('prototype-open-editor', onOpenEditor);
  refreshPlugins();
  showVariant(new URL(location.href).searchParams.get('editor') ?? 'A', false);
  state.open = true;
  windowEl!.hidden = false;
  syncToolbar();

  return () => {
    window.removeEventListener('popstate', onPopState);
    window.removeEventListener('prototype-open-editor', onOpenEditor);
    toolbar?.remove();
    windowEl?.remove();
    toolbar = null;
    windowEl = null;
  };
}
