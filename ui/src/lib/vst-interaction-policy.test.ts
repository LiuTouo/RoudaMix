import assert from "node:assert";
import { readFileSync } from "node:fs";
import { join } from "node:path";
import { test } from "node:test";

test("VST 掃描完成通知三秒自動消失且不顯示關閉按鈕", () => {
  const app = readFileSync(join(process.cwd(), "src", "App.svelte"), "utf8");

  assert.match(app, /autoDismissMs/);
  assert.match(app, /VST 清單已更新：[\s\S]*?3000/);
  assert.match(app, /\{#if n\.dismissible\}[\s\S]*?dismissNotice\(n\.id\)/);
});

test("VST 機架以單擊名稱與右鍵編輯開啟 GUI", () => {
  const strip = readFileSync(
    join(process.cwd(), "src", "lib", "TrackStrip.svelte"),
    "utf8",
  );

  assert.match(strip, />VST 機架 \(\{track\.plugins\.length\}\)<\/span>/);
  assert.match(
    strip,
    /onclick=\{\(\) => openEditor\(s\)\}[\s\S]*?\{s\.name\}<\/span\s*>/,
  );
  assert.match(strip, /label: "編輯"[\s\S]*?openEditor\(slot\)/);
  assert.doesNotMatch(strip, /ondblclick=\{\(\) => openEditor\(s\)\}/);
  assert.doesNotMatch(strip, />GUI<\/button/);
});
