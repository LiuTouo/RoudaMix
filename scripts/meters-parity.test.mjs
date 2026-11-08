// meters JSON 事件契約跨側比對:bridge(shm.rs serde camelCase)↔ UI(types.ts 手寫介面)。
// SHM 二進位佈局由 contracts/telemetry_abi.md + 兩側編譯期斷言把關;
// 這裡補的是 Rust→UI 的 JSON 鍵名——任一側改名,本測試與 shm.rs 單元測試都會攔。
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import test from "node:test";
import path from "node:path";
import { fileURLToPath } from "node:url";

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..");
const ts = readFileSync(path.join(root, "ui/src/lib/types.ts"), "utf8");
const rs = readFileSync(path.join(root, "src-tauri/src/shm.rs"), "utf8");

const camel = (name) => name.replace(/_([a-z])/g, (_, c) => c.toUpperCase());

/** TS interface 頂層欄位(恰好 2 空格縮排;更深縮排 = 巢狀型別,另測) */
function tsInterfaceFields(name) {
  const body = ts.match(new RegExp(`export interface ${name} \\{([\\s\\S]*?)\\n\\}`))?.[1] ?? "";
  return [...body.matchAll(/^ {2}(\w+)\??:/gm)].map((m) => m[1]).sort();
}

/** Rust struct 欄位 → camelCase(rename_all = "camelCase" 語意) */
function rsStructFields(name) {
  const body = rs.match(new RegExp(`struct ${name}(?:<[^>]*>)? \\{([\\s\\S]*?)\\n\\}`))?.[1] ?? "";
  return [...body.matchAll(/^ {4}(\w+):/gm)].map((m) => camel(m[1])).sort();
}

test("MetersFrame:shm.rs 序列化欄位 = UI types.ts 介面欄位", () => {
  assert.deepEqual(rsStructFields("MetersFrame"), tsInterfaceFields("MetersFrame"));
});

test("MeterStrip:UI 介面 = shm.rs StripOut", () => {
  assert.deepEqual(rsStructFields("StripOut"), tsInterfaceFields("MeterStrip"));
});

test("pluginLoads 元素:UI 內嵌型別 = shm.rs PluginLoadOut", () => {
  const inline = ts.match(/pluginLoads\?: Array<\{([\s\S]*?)\}>/)?.[1] ?? "";
  const uiFields = [...inline.matchAll(/^ {4}(\w+):/gm)].map((m) => m[1]).sort();
  assert.deepEqual(rsStructFields("PluginLoadOut"), uiFields);
});
