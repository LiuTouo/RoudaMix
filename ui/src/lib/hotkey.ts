// 全域快捷鍵擷取:KeyboardEvent → Tauri Shortcut 字串(如 "Ctrl+Shift+M")。
// 規則:必須含 Ctrl/Alt/Super 其中之一,或為 F1–F24(純字母/Shift 組合會在
// 其他應用程式打字時誤觸,拒絕)。鍵名以 e.code 為準(不受鍵盤配置影響),
// 輸出 global-hotkey crate 解析器接受的 friendly 名稱。

const MODIFIER_KEYS = new Set([
  "Control",
  "Shift",
  "Alt",
  "Meta",
  "CapsLock",
  "NumLock",
  "ScrollLock",
]);

/** e.code → global-hotkey 解析器接受的鍵名;不支援 = null */
function codeToKey(code: string): string | null {
  const letter = /^Key([A-Z])$/.exec(code);
  if (letter) return letter[1];
  const digit = /^Digit(\d)$/.exec(code);
  if (digit) return digit[1];
  if (/^F(\d{1,2})$/.test(code)) return code;
  const named: Record<string, string> = {
    Space: "Space",
    Minus: "-",
    Equal: "=",
    Comma: ",",
    Period: ".",
    Slash: "/",
    Semicolon: ";",
    Quote: "'",
    BracketLeft: "[",
    BracketRight: "]",
    Backslash: "\\",
    Backquote: "`",
    ArrowUp: "Up",
    ArrowDown: "Down",
    ArrowLeft: "Left",
    ArrowRight: "Right",
  };
  return named[code] ?? null;
}

/** 擷取到的鍵盤事件 → 正規 shortcut 字串(修飾鍵序 Ctrl, Alt, Shift, Super);
 *  不符合安全規則或不支援的鍵 = null(呼叫端維持擷取模式)。 */
export function eventToShortcut(e: KeyboardEvent): string | null {
  if (MODIFIER_KEYS.has(e.key)) return null; // 純修飾鍵按下,不採計
  const key = codeToKey(e.code);
  if (!key) return null;
  const isFKey = /^F(\d{1,2})$/.test(key);
  if (!isFKey && !e.ctrlKey && !e.altKey && !e.metaKey) return null;
  const mods: string[] = [];
  if (e.ctrlKey) mods.push("Ctrl");
  if (e.altKey) mods.push("Alt");
  if (e.shiftKey) mods.push("Shift");
  if (e.metaKey) mods.push("Super");
  return [...mods, key].join("+");
}
