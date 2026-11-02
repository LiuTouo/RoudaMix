/** 介面字型設定：--font-ui 的套用與安全檢查（設定 → 通用）。
 *  字族名會被內插進 inline style 的 font-family，必須擋掉 CSS 字串逃脫
 *  （引號/反斜線/括號語境字元）；settings.rs normalize 也有同規則把關。 */

export const FONT_VAR = "--font-ui";

/** 預設字族階梯（app.css :root 同步維護）。 */
const DEFAULT_STACK = '"IBM Plex Sans TC", "Segoe UI", system-ui, sans-serif';

/** 字族名安全檢查：含引號/反斜線/分號/大括號/換行或空字串 → 回 null（用預設）。 */
export function sanitizeFontFamily(raw: string | null | undefined): string | null {
  if (!raw) return null;
  const name = raw.trim();
  if (!name || name.length > 64) return null;
  if (/["\\;{}]|[\r\n\u0000]/.test(name)) return null;
  return name;
}

/** 套用字族到文件根；null = 移除 inline 覆蓋，回到樣式表預設。 */
export function applyUiFont(family: string | null | undefined): string | null {
  const name = sanitizeFontFamily(family);
  const root = document.documentElement;
  if (!name) {
    root.style.removeProperty(FONT_VAR);
    return null;
  }
  root.style.setProperty(FONT_VAR, `"${name}", ${DEFAULT_STACK}`);
  return name;
}
