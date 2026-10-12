// @vitest-environment jsdom
import { afterEach, beforeEach, expect, it, vi } from "vitest";
import { flushSync, mount, unmount } from "svelte";
import ContextMenu from "../ContextMenu.svelte";
import ConfirmDialog from "../ConfirmDialog.svelte";

let component: ReturnType<typeof mount> | undefined;
const originalShowModal = HTMLDialogElement.prototype.showModal;
const originalClose = HTMLDialogElement.prototype.close;
beforeEach(() => {
  HTMLDialogElement.prototype.showModal = function () { this.open = true; };
  HTMLDialogElement.prototype.close = function () { this.open = false; };
});
afterEach(async () => {
  if (component) await unmount(component);
  component = undefined;
  document.body.replaceChildren();
  vi.restoreAllMocks();
  if (originalShowModal) HTMLDialogElement.prototype.showModal = originalShowModal;
  else Reflect.deleteProperty(HTMLDialogElement.prototype, "showModal");
  if (originalClose) HTMLDialogElement.prototype.close = originalClose;
  else Reflect.deleteProperty(HTMLDialogElement.prototype, "close");
});

function openMenu(disabled = false) {
  const opener = document.createElement("button");
  opener.textContent = "開啟選單";
  document.body.append(opener);
  opener.focus();
  const close = vi.fn();
  const run = vi.fn();
  component = mount(ContextMenu, { target: document.body, props: {
    menu: { x: 10, y: 10, label: "插件操作", items: [
      { label: "無法上移", disabled: true, run },
      { label: "開啟", disabled, run },
      { label: "複製", disabled, run },
      { label: "貼上", disabled: true, run },
      { label: "下移", disabled, run },
    ] }, onClose: close,
  } });
  flushSync();
  return { opener, close, run };
}
function key(value: string) {
  document.activeElement!.dispatchEvent(new KeyboardEvent("keydown", { key: value, bubbles: true, cancelable: true }));
  flushSync();
}

it("選單聚焦首個可用項目，方向鍵循環並略過停用項目，Home/End 到首尾", () => {
  const { run } = openMenu();
  expect(document.querySelector("dialog")?.getAttribute("aria-label")).toBe("插件操作");
  expect(document.activeElement?.textContent).toBe("開啟");
  for (const [pressed, label] of [["ArrowDown", "複製"], ["ArrowDown", "下移"], ["ArrowDown", "開啟"],
    ["ArrowUp", "下移"], ["Home", "開啟"], ["End", "下移"]]) {
    key(pressed);
    expect(document.activeElement?.textContent).toBe(label);
  }
  expect(run).not.toHaveBeenCalled();
  (document.activeElement as HTMLButtonElement).click();
  expect(run).toHaveBeenCalledOnce();
});

it("Esc 取消選單，移除後將焦點還給開啟控制項", async () => {
  const { opener, close, run } = openMenu();
  const event = new Event("cancel", { cancelable: true });
  document.querySelector("dialog")!.dispatchEvent(event);
  expect(event.defaultPrevented).toBe(true);
  expect(close).toHaveBeenCalledOnce();
  await unmount(component!); component = undefined;
  expect(document.activeElement).toBe(opener);
  expect(run).not.toHaveBeenCalled();
});

it("Tab 關閉 modal，不攔截瀏覽器原本的焦點前進", () => {
  const { close, run } = openMenu();
  const event = new KeyboardEvent("keydown", { key: "Tab", bubbles: true, cancelable: true });
  document.activeElement!.dispatchEvent(event);
  expect(document.querySelector<HTMLDialogElement>("dialog")!.open).toBe(false);
  expect(close).toHaveBeenCalledOnce();
  expect(event.defaultPrevented).toBe(false);
  expect(run).not.toHaveBeenCalled();
});

it("移除選單不搶走操作已移到其他控制項的焦點", async () => {
  openMenu();
  const destination = document.createElement("input");
  document.body.append(destination);
  destination.focus();
  await unmount(component!); component = undefined;
  expect(document.activeElement).toBe(destination);
});

it("全部項目停用時仍可取消，不執行任何項目", () => {
  const { close, run } = openMenu(true);
  const menu = document.querySelector<HTMLElement>('[role="menu"]')!;
  menu.focus(); key("ArrowDown"); key("End");
  expect(document.activeElement).toBe(menu);
  document.querySelector("dialog")!.dispatchEvent(new Event("cancel", { cancelable: true }));
  expect(close).toHaveBeenCalledOnce();
  expect(run).not.toHaveBeenCalled();
});

it("刪除確認視窗有可辨識名稱，保留影響清單及取消入口", () => {
  const answer = vi.fn();
  component = mount(ConfirmDialog, { target: document.body, props: {
    confirm: { title: "移除混響？", impact: ["外掛參數無法直接還原"], confirmLabel: "移除" }, onAnswer: answer,
  } });
  flushSync();
  const dialog = document.querySelector("dialog")!;
  expect(dialog.getAttribute("aria-label")).toBe("移除混響？");
  expect(dialog.querySelector("li")?.textContent).toBe("外掛參數無法直接還原");
  dialog.querySelector("button")!.click();
  expect(answer).toHaveBeenCalledWith(false);
});
