import assert from "node:assert";
import { test } from "node:test";
import { pluginMenuItems } from "./pluginMenu.ts";

const base = {
  slot: { name: "Synth", availability: "ok" as const },
  index: 1,
  chainLength: 3,
  latencyEnabled: true,
  monitorBypassShown: false,
  run: { editor: () => {}, move: () => {}, monitorBypass: () => {} },
};

const labels = (items: ReturnType<typeof pluginMenuItems>) => items.map((i) => i.label);

test("選單含編輯、移動與 Monitor Bypass,移動禁用依鏈內位置", () => {
  const items = pluginMenuItems(base);
  assert.deepEqual(labels(items), [
    "編輯",
    "上移",
    "下移",
    "移到最前",
    "移到最後",
    "Monitor Bypass",
  ]);
  const byLabel = Object.fromEntries(items.map((i) => [i.label, i]));
  assert.equal(byLabel["上移"].disabled, false);
  assert.equal(byLabel["下移"].disabled, false);
  assert.equal(byLabel["編輯"].disabled, false);
});

test("鏈端項目的移動被禁用,placeholder 的編輯被禁用", () => {
  const first = pluginMenuItems({ ...base, index: 0 });
  assert.equal(first.find((i) => i.label === "上移")?.disabled, true);
  assert.equal(first.find((i) => i.label === "移到最前")?.disabled, true);

  const last = pluginMenuItems({ ...base, index: base.chainLength - 1 });
  assert.equal(last.find((i) => i.label === "下移")?.disabled, true);
  assert.equal(last.find((i) => i.label === "移到最後")?.disabled, true);

  const ph = pluginMenuItems({ ...base, slot: { name: "Synth", availability: "loadFailed" } });
  assert.equal(ph.find((i) => i.label === "編輯")?.disabled, true);
});

test("Monitor Bypass 項目只在 capability 開啟時出現,標籤隨顯示狀態翻轉", () => {
  assert.ok(!labels(pluginMenuItems({ ...base, latencyEnabled: false })).includes("Monitor Bypass"));
  assert.ok(labels(pluginMenuItems(base)).includes("Monitor Bypass"));
  const shown = pluginMenuItems({ ...base, monitorBypassShown: true });
  assert.equal(shown.find((i) => i.label === "取消 Monitor Bypass")?.run, base.run.monitorBypass);
});
