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
