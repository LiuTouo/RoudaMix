/** tutorialSteps 資料測試(node --test):兩變體流程與共同步驟、無原生 title 屬性。 */
import assert from "node:assert/strict";
import { test } from "node:test";
import { tutorialSteps } from "./tutorialSteps.ts";

test("兩變體皆以裝置步驟開頭、共同步驟結尾", () => {
  const asio = tutorialSteps("asio");
  const wasapi = tutorialSteps("wasapi");
  assert.equal(asio[0].id, "asio-device");
  assert.equal(wasapi[0].id, "wasapi-device");
  assert.deepEqual(
    asio.slice(-5).map((s) => s.id),
    ["add-track", "set-output", "add-plugin", "mix", "session"],
  );
  assert.deepEqual(
    wasapi.slice(-5).map((s) => s.id),
    ["add-track", "set-output", "add-plugin", "mix", "session"],
  );
});

test("ASIO 變體含 buffer 與控制面板;WASAPI 變體不含", () => {
  const asio = tutorialSteps("asio").map((s) => s.id);
  const wasapi = tutorialSteps("wasapi").map((s) => s.id);
  assert.ok(asio.includes("asio-buffer"));
  assert.ok(asio.includes("asio-panel"));
  assert.ok(!wasapi.includes("asio-buffer"));
  assert.ok(!wasapi.includes("asio-panel"));
});

test("步驟 id 與 demo id 均唯一非空,文案為繁體中文", () => {
  for (const variant of ["asio", "wasapi"] as const) {
    const steps = tutorialSteps(variant);
    assert.equal(new Set(steps.map((s) => s.id)).size, steps.length);
    assert.equal(new Set(steps.map((s) => s.demo)).size, steps.length);
    for (const s of steps) {
      assert.ok(s.id && s.demo && s.title && s.body);
      // 簡體專用字元不應出現(粗檢;音/置等繁簡同形字不列入)
      assert.ok(!/[设软记频录]/.test(`${s.title}${s.body}`));
    }
  }
});
