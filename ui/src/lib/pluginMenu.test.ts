import assert from "node:assert";
import { test } from "node:test";
import { pluginMenuItems } from "./pluginMenu.ts";

const base = {
  slot: { name: "Synth", availability: "ok" as const },
  index: 1,
  chainLength: 3,
  latencyEnabled: true,
  monitorBypassShown: false,
  monitorBypassPending: false,
  run: { editor: () => {}, move: () => {}, monitorBypass: () => {}, remove: () => {} },
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
    "監聽略過（Monitor Bypass）",
    "移除插件…",
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

test("Monitor Bypass 以勾選表示狀態，capability 關閉時不出現，pending 時不可重複操作", () => {
  assert.ok(!pluginMenuItems({ ...base, latencyEnabled: false }).some((item) => item.checked !== undefined));
  for (const checked of [false, true]) {
    const item = pluginMenuItems({ ...base, monitorBypassShown: checked }).find((item) => item.checked !== undefined)!;
    assert.equal(item.label, "監聽略過（Monitor Bypass）");
    assert.equal(item.checked, checked);
    assert.equal(item.disabled, false);
    assert.equal(item.run, base.run.monitorBypass);
    const pending = pluginMenuItems({ ...base, monitorBypassShown: checked, monitorBypassPending: true })
      .find((item) => item.checked !== undefined)!;
    assert.equal(pending.checked, checked);
    assert.equal(pending.disabled, true);
    assert.equal(pending.label, item.label);
  }
});

test("正常與缺失插件都保留移除確認入口，不依賴 latency capability", () => {
  for (const availability of ["ok", "missing", "loadFailed"] as const) {
    const items = pluginMenuItems({ ...base, slot: { name: "Synth", availability }, latencyEnabled: false });
    assert.equal(items.at(-1)?.label, "移除插件…");
    assert.equal(items.at(-1)?.run, base.run.remove);
    assert.equal(items.at(-1)?.disabled, undefined);
  }
});

test("重新命名項目隨 rename 提供,placeholder 也可用", () => {
  let called = 0;
  const withRename = pluginMenuItems({ ...base, rename: { run: () => called++ } });
  assert.equal(labels(withRename)[1], "重新命名"); // 緊跟在編輯之後
  withRename.find((i) => i.label === "重新命名")?.run();
  assert.equal(called, 1);
  assert.equal(labels(pluginMenuItems(base)).includes("重新命名"), false);

  const ph = pluginMenuItems({
    ...base,
    slot: { name: "Synth", availability: "missing" },
    rename: { run: () => called++ },
  });
  assert.equal(ph.find((i) => i.label === "重新命名")?.disabled, undefined); // metadata-only,不隨 placeholder 禁用
});
