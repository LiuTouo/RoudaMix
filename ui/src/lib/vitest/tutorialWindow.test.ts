// @vitest-environment jsdom
// 使用教學獨立視窗:渲染與導覽(選介面 → 步驟 → 完成 → 換介面重看)、Esc 關窗。
import { afterEach, beforeEach, expect, it, vi } from "vitest";
import { flushSync, mount, tick, unmount } from "svelte";
import TutorialWindow from "../TutorialWindow.svelte";

const { windowCloseCount } = vi.hoisted(() => ({ windowCloseCount: { n: 0 } }));
vi.mock("@tauri-apps/api/window", () => ({
  getCurrentWindow: () => ({
    close: async () => { windowCloseCount.n++; },
    startDragging: async () => {},
  }),
}));

let component: ReturnType<typeof mount> | undefined;
async function settle() { for (let i = 0; i < 30; i++) { await tick(); flushSync(); } }
function show() { component = mount(TutorialWindow, { target: document.body }); return component; }
const text = () => document.body.textContent ?? "";
const button = (label: string) =>
  [...document.querySelectorAll<HTMLButtonElement>("button")].find((b) => b.textContent?.includes(label))!;

beforeEach(() => { windowCloseCount.n = 0; });
afterEach(async () => {
  if (component) await unmount(component);
  component = undefined;
  await settle();
  document.body.replaceChildren();
});

it("落地頁選 ASIO 進入對應步驟,一路下一步到完成頁", async () => {
  show(); await settle();
  expect(text()).toContain("先選擇你的音訊介面");
  // 落地頁也有關閉鈕（可拖曳 header）
  expect(document.querySelector('[aria-label="關閉教學視窗"]')).toBeTruthy();
  button("ASIO").click(); await settle();
  expect(text()).toContain("選擇 ASIO 裝置");
  expect(text()).toContain("1 / 8");
  expect(document.querySelector('[aria-label="重播動畫"]')).toBeTruthy();
  for (let k = 1; k < 8; k++) button("下一步").click(), await settle();
  button("完成").click(); await settle();
  expect(text()).toContain("教學完成");
});

it("WASAPI 變體跳過 ASIO 專屬步驟", async () => {
  show(); await settle();
  button("WASAPI").click(); await settle();
  expect(text()).toContain("選擇系統音訊");
  expect(text()).toContain("1 / 7");
  expect(text()).not.toContain("ASIO 控制面板");
});

it("完成頁可改看另一介面教學並回到第一步", async () => {
  show(); await settle();
  button("ASIO").click(); await settle();
  for (let k = 1; k < 8; k++) button("下一步").click(), await settle();
  button("完成").click(); await settle();
  button("改看WASAPI教學").click(); await settle();
  expect(text()).toContain("選擇系統音訊");
  expect(text()).toContain("1 / 7");
});

it("上一步在第一步停用;Esc 關閉視窗", async () => {
  show(); await settle();
  button("ASIO").click(); await settle();
  expect(button("上一步").disabled).toBe(true);
  button("下一步").click(); await settle();
  expect(button("上一步").disabled).toBe(false);
  button("上一步").click(); await settle();
  expect(text()).toContain("1 / 8");
  window.dispatchEvent(new KeyboardEvent("keydown", { key: "Escape", cancelable: true }));
  await settle();
  expect(windowCloseCount.n).toBe(1);
});

it("不使用原生 title 屬性,提示一律 data-tooltip", async () => {
  show(); await settle();
  button("ASIO").click(); await settle();
  expect(document.querySelectorAll("[title]")).toHaveLength(0);
  expect(document.querySelectorAll("[data-tooltip]").length).toBeGreaterThan(0);
});
