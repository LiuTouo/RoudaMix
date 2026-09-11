// @vitest-environment jsdom
import { afterEach, beforeEach, expect, it, vi } from "vitest";
import { flushSync, mount, unmount } from "svelte";
import UpdatePanel from "../UpdatePanel.svelte";

const mocks = vi.hoisted(() => ({ check: vi.fn(), listen: vi.fn(), invoke: vi.fn(), cleanup: vi.fn() }));
vi.mock("../updates", () => ({ checkRelease: mocks.check }));
vi.mock("@tauri-apps/api/app", () => ({ getVersion: async () => "0.1.1" }));
vi.mock("@tauri-apps/api/core", () => ({ invoke: mocks.invoke }));
vi.mock("@tauri-apps/api/event", () => ({ listen: mocks.listen }));
let component: ReturnType<typeof mount>;
const settle = async () => { for (let i = 0; i < 8; i++) { await Promise.resolve(); flushSync(); } };
const button = (text: string) => [...document.querySelectorAll("button")].find((b) => b.textContent?.includes(text))!;
beforeEach(() => {
  vi.clearAllMocks();
  mocks.listen.mockResolvedValue(mocks.cleanup);
  mocks.check.mockResolvedValue({ version: "0.1.2", available: true });
  mocks.invoke.mockResolvedValue(undefined);
});
afterEach(async () => { await unmount(component); document.body.innerHTML = ""; });
function show(autoCheck: boolean, onPreference = vi.fn().mockResolvedValue(true)) {
  const props = { visible: true, autoCheck, onShow: vi.fn(), onPreference, onAvailable: vi.fn() };
  component = mount(UpdatePanel, { target: document.body, props });
  flushSync();
  return props;
}
it("啟動檢查一次並提示新版，下載使用受限的後端命令", async () => {
  const props = show(true);
  await settle();
  expect(document.body.textContent).toContain("版本 v0.1.1");
  expect(mocks.check).toHaveBeenCalledTimes(1);
  expect(props.onAvailable).toHaveBeenCalledWith("0.1.2");
  button("前往下載").click();
  await settle();
  expect(mocks.invoke).toHaveBeenCalledWith("open_release_page");
});
it("關閉啟動檢查後，仍可由系統匣開啟關於或手動檢查", async () => {
  const props = show(false);
  await settle();
  expect(mocks.check).not.toHaveBeenCalled();
  const handler = mocks.listen.mock.calls[0][1];
  handler({ payload: false });
  expect(props.onShow).toHaveBeenCalledTimes(1);
  expect(mocks.check).not.toHaveBeenCalled();
  handler({ payload: true });
  await settle();
  expect(mocks.check).toHaveBeenCalledOnce();
});
it("檢查期間停用按鈕、失敗後允許重試", async () => {
  show(false);
  await settle();
  let reject!: (reason: Error) => void;
  mocks.check.mockImplementationOnce(() => new Promise((_resolve, rej) => { reject = rej; }));
  button("檢查更新").click();
  await settle();
  expect(button("檢查中").disabled).toBe(true);
  reject(new Error("離線"));
  await settle();
  expect(document.body.textContent).toContain("檢查更新失敗");
  expect(button("檢查更新").disabled).toBe(false);
  button("檢查更新").click();
  await settle();
  expect(document.body.textContent).toContain("v0.1.2 可供下載");
});
it("設定寫入失敗時還原勾選狀態", async () => {
  const preference = vi.fn().mockResolvedValue(false);
  show(false, preference);
  await settle();
  const input = document.querySelector("input")!;
  input.checked = true;
  input.dispatchEvent(new Event("change", { bubbles: true }));
  await settle();
  expect(preference).toHaveBeenCalledWith(true);
  expect(input.checked).toBe(false);
});
