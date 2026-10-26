// VST 電源 icon(使用者提供 SVG):綠 = 啟用、黑 = bypass。
// 內嵌 data URI — 免資源管線,Tauri dist 烘焙直接可用。
const svg = (stroke: string) =>
  `data:image/svg+xml,${encodeURIComponent(
    `<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 512 512" fill="none">` +
      `<path d="M255 21V258" stroke="${stroke}" stroke-width="40" stroke-linecap="round"/>` +
      `<path d="M421 90C465 134 491 193 491 256C491 386 386 492 256 492C126 492 20 386 20 256C20 193 46 134 89 90" stroke="${stroke}" stroke-width="40" stroke-linecap="round" stroke-linejoin="round"/>` +
      `</svg>`,
  )}`;

export const powerOn = svg("#16A34A");
export const powerOff = svg("#000000");

const trackIcon = (label: string, paths: string) => ({
  label,
  src: `data:image/svg+xml,${encodeURIComponent(`<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 24 24" fill="none" stroke="#E2E5E8" stroke-width="1.7" stroke-linecap="round" stroke-linejoin="round">${paths}</svg>`)}`,
});

export const trackIcons = {
  monitor: trackIcon("監聽輸出", '<path d="M4 14v-3a8 8 0 0 1 16 0v3"/><rect x="3" y="12" width="4" height="8" rx="2"/><rect x="17" y="12" width="4" height="8" rx="2"/>'),
  stream: trackIcon("串流輸出", '<circle cx="12" cy="10" r="2"/><path d="m12 12-3 9h6l-3-9M7 5a7 7 0 0 0 0 10M17 5a7 7 0 0 1 0 10M4 2a11 11 0 0 0 0 16M20 2a11 11 0 0 1 0 16"/>'),
  output: trackIcon("音訊輸出", '<path d="M4 9h4l5-4v14l-5-4H4zM16 8a6 6 0 0 1 0 8M19 5a10 10 0 0 1 0 14"/>'),
  fx: trackIcon("效果音軌", '<path d="M5 3v8m0 4v6M12 3v3m0 4v11M19 3v11m0 4v3M2 11h6v4H2zM9 6h6v4H9zM16 14h6v4h-6z"/>'),
  app: trackIcon("應用程式音軌", '<rect x="3" y="4" width="18" height="16" rx="2"/><path d="M3 9h18M6 6.5h1m3 0h1"/>'),
  mic: trackIcon("麥克風輸入", '<rect x="9" y="2" width="6" height="13" rx="3"/><path d="M5 10v2a7 7 0 0 0 14 0v-2M12 19v3m-4 0h8"/>'),
  sine: trackIcon("正弦波來源", '<path d="M2 12c4-14 6-14 10 0s6 14 10 0"/>'),
  audio: trackIcon("音訊輸入", '<path d="M3 10v4M7 6v12M12 2v20M17 6v12M21 10v4"/>'),
};
