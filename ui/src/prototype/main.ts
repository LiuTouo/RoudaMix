// THROWAWAY：同一首頁上的原版與三種深色風格，以 ?variant= 切換。
import { mount, unmount } from 'svelte';
import App from '../App.svelte';
import '../app.css';
import '../c-theme.css';
import './styles.css';
import { installTooltip } from '../lib/tooltip';
import { startPreviewMeters } from './tauri';
import { installStyleSwitcher } from './switcher';

// TrackStrip 的高度偏好也只留在記憶體，重新整理即回復固定基準。
const storageDescriptor = Object.getOwnPropertyDescriptor(window, 'localStorage');
const values = new Map<string, string>();
const memoryStorage: Storage = {
  get length() { return values.size; },
  getItem: (key) => values.get(key) ?? null,
  setItem: (key, value) => { values.set(key, String(value)); },
  removeItem: (key) => { values.delete(key); },
  clear: () => values.clear(),
  key: (index) => [...values.keys()][index] ?? null,
};
Object.defineProperty(window, 'localStorage', { configurable: true, value: memoryStorage });
document.title = 'RoudaMix — 風格預覽';
const app = mount(App, { target: document.getElementById('app')! });
const removeSwitcher = installStyleSwitcher();
const removeTooltip = installTooltip();
const stopMeters = startPreviewMeters();

if (import.meta.hot) import.meta.hot.dispose(() => {
  removeSwitcher();
  removeTooltip();
  stopMeters();
  void unmount(app);
  if (storageDescriptor) Object.defineProperty(window, 'localStorage', storageDescriptor);
});
