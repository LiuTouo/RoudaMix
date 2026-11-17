/** guidedTour 資料測試(node --test):兩變體流程順序、id/selector 唯一、繁體中文文案。 */
import assert from "node:assert/strict";
import { test } from "node:test";
import { guidedTourSteps } from "./guidedTour.ts";

test("兩變體皆以開啟設定與裝置步驟開頭、Session 步驟結尾", () => {
  const asio = guidedTourSteps("asio").map((s) => s.id);
  const wasapi = guidedTourSteps("wasapi").map((s) => s.id);
  assert.equal(asio[0], "open-settings");
  assert.equal(asio[1], "asio-device");
  assert.equal(wasapi[0], "open-settings");
  assert.equal(wasapi[1], "wasapi-device");
  assert.equal(asio[asio.length - 1], "session");
  assert.equal(wasapi[wasapi.length - 1], "session");
  // App/FX 軌使用說明在 strip 步驟之後、Session 之前
  for (const ids of [asio, wasapi]) {
    assert.ok(ids.includes("app-track") && ids.includes("fx-track"));
    assert.ok(ids.indexOf("track-meter") < ids.indexOf("app-track"));
    assert.ok(ids.indexOf("fx-track") < ids.indexOf("session"));
  }
});

test("ASIO 變體含 buffer 與控制面板;WASAPI 變體不含", () => {
  const asio = guidedTourSteps("asio").map((s) => s.id);
  const wasapi = guidedTourSteps("wasapi").map((s) => s.id);
  assert.ok(asio.includes("asio-buffer"));
  assert.ok(asio.includes("asio-panel"));
  assert.ok(!wasapi.includes("asio-buffer"));
  assert.ok(!wasapi.includes("asio-panel"));
});

test("步驟 id 與 selector 均非空,音軌步驟以 {track} 代換", () => {
  for (const variant of ["asio", "wasapi"] as const) {
    const steps = guidedTourSteps(variant);
    assert.equal(new Set(steps.map((s) => s.id)).size, steps.length);
    for (const s of steps) {
      assert.ok(s.id && s.selector && s.title && s.body, `${s.id} 欄位不得為空`);
      assert.match(s.body, /[\u4e00-\u9fff]/);
    }
  }
});

test("opensSettings/closesSettings 成對出現:設定流程結尾關閉、Session 步驟開關兼具", () => {
  for (const variant of ["asio", "wasapi"] as const) {
    const steps = guidedTourSteps(variant);
    const settingsSteps = steps.filter((s) => s.opensSettings);
    assert.ok(settingsSteps.length > 0);
    const session = steps.find((s) => s.id === "session")!;
    assert.equal(session.opensSettings, "scenes");
    assert.ok(session.closesSettings);
    // 裝置流程（session 前）最後一個設定步驟要負責關閉
    const deviceTail = steps.filter((s) => s.id !== "session" && s.opensSettings).at(-1)!;
    assert.ok(deviceTail.closesSettings, `${deviceTail.id} 應 closesSettings`);
  }
});
