// @vitest-environment jsdom
// 引導式教學 overlay:落地頁 → click/read 步驟導航、代行新增音軌、optional 跳過、
// Esc 結束、VST 面板展開、無原生 title(提示一律 data-tooltip)。
import { afterEach, beforeEach, expect, it, vi } from "vitest";
import { flushSync, mount, tick, unmount } from "svelte";
import GuidedTour from "../GuidedTour.svelte";

// jsdom 無版面:scrollIntoView 未實作,patch 成 no-op。
beforeEach(() => {
  Element.prototype.scrollIntoView = () => {};
});

let component: ReturnType<typeof mount> | undefined;
const calls: string[] = [];
let fixture: HTMLDivElement;
let firstId: number | null = null;
let hasTracks = false;

async function settle(ms = 60) {
  for (let i = 0; i < 5; i++) {
    await tick();
    flushSync();
  }
  await new Promise((r) => setTimeout(r, ms));
}

const STRIP_ANCHORS =
  '<div data-track-id="7"><div data-tour="source">src</div><div data-tour="dest">dest</div>' +
  '<button data-tour="plugin-add">＋</button><input data-tour="fader" /><div data-tour="meter">m</div></div>';

/** 主視窗控制項 fixture:齒輪/設定列/＋Audio(+可選一條 id=7 的音軌錨點)。 */
function buildFixture(withTrack: boolean) {
  fixture = document.createElement("div");
  const add = (html: string) => fixture.insertAdjacentHTML("beforeend", html);
  add('<button data-tour="settings-gear">⚙</button>');
  add('<div data-tour="set-device">裝置</div><div data-tour="set-buffer">buf</div><div data-tour="set-asiopanel">panel</div>');
  add('<div data-tour="add-audio">＋ Audio</div><div data-tour="add-app">＋ App</div><div data-tour="add-fx">＋ FX</div>');
  add('<div data-tour="set-sessions">sessions</div>');
  if (withTrack) add(STRIP_ANCHORS);
  document.body.appendChild(fixture);
}

function show(props: Record<string, unknown> = {}) {
  component = mount(GuidedTour, {
    target: document.body,
    props: {
      resolveTimeoutMs: 60,
      tourTrackId: () => firstId,
      hasInputTracks: () => hasTracks,
      onAddTrack: () => calls.push("addTrack"),
      onEnsureTrack: (id: number) => calls.push(`ensure:${id}`),
      onOpenVst: (id: number) => calls.push(`vst:${id}`),
      onOpenSettings: (tab: string) => calls.push(`settings:${tab}`),
      onCloseSettings: () => calls.push("closeSettings"),
      onEnd: () => calls.push("end"),
      ...props,
    },
  });
  return component;
}

const text = () => document.body.textContent ?? "";
const button = (label: string) =>
  [...document.querySelectorAll<HTMLButtonElement>("button")].find((b) => b.textContent?.includes(label))!;

beforeEach(() => {
  calls.length = 0;
  firstId = null;
  hasTracks = false;
});
afterEach(async () => {
  if (component) await unmount(component);
  component = undefined;
  await settle(20);
  document.body.replaceChildren();
});

it("落地頁選 ASIO → click 步驟實點齒輪前進,裝置步驟自動開設定切音訊分頁", async () => {
  buildFixture(true);
  show();
  await settle();
  expect(text()).toContain("先選擇你的音訊介面");
  button("ASIO").click();
  await settle();
  expect(text()).toContain("開啟設定");
  expect(text()).toContain("1 / 13");
  // click 步驟:實際點擊高亮目標 → 前進
  fixture.querySelector<HTMLButtonElement>('[data-tour="settings-gear"]')!.click();
  await settle();
  expect(text()).toContain("選擇 ASIO 裝置");
  expect(calls).toContain("settings:audio");
});

it("read 步驟按下一步前進;closesSettings 步驟離開時關設定", async () => {
  buildFixture(true);
  show();
  await settle();
  button("ASIO").click();
  await settle();
  button("下一步").click(); // open-settings 下一步 → 下一步 opensSettings 接手
  await settle();
  expect(text()).toContain("選擇 ASIO 裝置");
  button("下一步").click();
  await settle();
  expect(text()).toContain("調整 Buffer Size");
  button("下一步").click();
  await settle();
  expect(text()).toContain("ASIO 控制面板");
  button("下一步").click(); // 離開 asio-panel(closesSettings)
  await settle();
  expect(calls).toContain("closeSettings");
  expect(text()).toContain("新增音軌");
});

it("add-track:無音軌按下一步代行新增;進入插件步驟展開 VST 面板", async () => {
  buildFixture(false);
  show();
  await settle();
  button("WASAPI").click();
  await settle();
  button("下一步").click();
  await settle();
  expect(text()).toContain("選擇系統音訊");
  button("下一步").click();
  await settle();
  expect(text()).toContain("取樣率跟隨系統");
  button("下一步").click();
  await settle();
  expect(text()).toContain("新增音軌");
  // 下一步代行:onAddTrack 被叫;引擎往返後軌出現(id 經函式 prop 恆新)
  const p = Promise.resolve().then(() => {
    fixture.insertAdjacentHTML("beforeend", STRIP_ANCHORS);
    firstId = 7;
  });
  button("下一步").click();
  await p;
  await settle(300);
  expect(calls).toContain("addTrack");
  expect(calls).toContainEqual("ensure:7");
  for (const t of ["選擇輸入來源", "指定輸出目的地"]) {
    expect(text()).toContain(t);
    button("下一步").click();
    await settle();
  }
  expect(text()).toContain("加入 VST 插件");
  expect(calls).toContainEqual("vst:7");
});

it("已有音軌 → 沿用現有軌跳過新增;一路下一步到完成頁;完成頁可改看另一介面", async () => {
  buildFixture(true);
  firstId = 7;
  hasTracks = true;
  show();
  await settle();
  button("ASIO").click();
  await settle();
  // asio 共 13 步;已有軌 add-track 跳過 → 12 次「下一步/完成」到完成頁
  for (let k = 0; k < 12; k++) {
    const next = [...document.querySelectorAll<HTMLButtonElement>("button")].find((b) =>
      b.textContent?.includes("下一步") || b.textContent?.includes("完成"));
    next!.click();
    await settle();
    if (k === 3) expect(text()).toContain("選擇輸入來源"); // 新增被跳過 → 直接沿用現有軌
  }
  expect(text()).toContain("教學完成");
  expect(calls).toContain("settings:scenes");
  expect(calls).toContain("closeSettings");
  button("改看WASAPI教學").click();
  await settle();
  expect(text()).toContain("開啟設定");
  expect(text()).toContain("1 / 12");
});

it("無音軌且代行失敗 → 停在原步驟置中卡（不自動跳過），手動下一步仍可前進", async () => {
  buildFixture(false);
  show();
  await settle();
  button("WASAPI").click();
  await settle();
  button("下一步").click();
  await settle();
  button("下一步").click();
  await settle();
  button("下一步").click();
  await settle();
  expect(text()).toContain("新增音軌");
  button("下一步").click(); // addTrack 代行但軌永遠不會出現
  await settle(400); // 超時後仍停在這步（置中卡、無 spotlight）
  expect(text()).toContain("選擇輸入來源");
  expect(text()).toContain("5 / 12");
  expect(document.querySelector(".tour-ring")).toBeNull(); // 無目標 = 無 spotlight 框
  // 手動走完剩餘步驟（5→12 共 8 次）
  for (let k = 0; k < 8; k++) {
    const next = [...document.querySelectorAll<HTMLButtonElement>("button")].find((b) =>
      b.textContent?.includes("下一步") || b.textContent?.includes("完成"));
    next!.click();
    await settle();
  }
  expect(text()).toContain("教學完成");
});

it("上一步在第一步停用;Esc 結束教學", async () => {
  buildFixture(true);
  show();
  await settle();
  button("ASIO").click();
  await settle();
  expect(button("上一步").disabled).toBe(true);
  button("下一步").click();
  await settle();
  button("上一步").click();
  await settle();
  expect(text()).toContain("1 / 13");
  window.dispatchEvent(new KeyboardEvent("keydown", { key: "Escape", cancelable: true }));
  await settle();
  expect(calls).toContain("end");
});

it("不使用原生 title 屬性,提示一律 data-tooltip", async () => {
  buildFixture(true);
  show();
  await settle();
  button("ASIO").click();
  await settle();
  expect(document.querySelectorAll("[title]")).toHaveLength(0);
  expect(document.querySelectorAll("[data-tooltip]").length).toBeGreaterThan(0);
});

it("回歸：目標在 dialog 內 → 等 dialog open 才接受；遮罩照常、卡片不 portal（App 以非 modal 開啟）", async () => {
  // 設定 dialog 內容先在 DOM、open 屬性較晚補上（模擬 Svelte flush 後才開）
  const dlg = document.createElement("dialog");
  dlg.innerHTML = '<div data-tour="set-device">dev</div><div data-tour="set-buffer">b</div><div data-tour="set-asiopanel">p</div>';
  document.body.appendChild(dlg);
  const other = document.createElement("div");
  other.innerHTML = '<button data-tour="settings-gear">g</button><div data-tour="add-audio">a</div><div data-tour="add-app">ap</div><div data-tour="add-fx">fx</div>';
  document.body.appendChild(other);
  setTimeout(() => dlg.setAttribute("open", ""), 100);
  show();
  await settle();
  button("ASIO").click();
  await settle();
  button("下一步").click(); // open-settings → asio-device(opensSettings)
  await settle(300);
  expect(text()).toContain("選擇 ASIO 裝置");
  const pop = document.querySelector(".tour-pop");
  expect(pop).toBeTruthy();
  expect(pop!.parentElement).not.toBe(dlg); // 不 portal:非 modal 無 top-layer,z-index 即可覆蓋
  expect(document.querySelector(".tour-mask")).toBeTruthy(); // 遮罩照常 spotlight
  button("下一步").click(); // 同 dialog 下一列
  await settle();
  expect(text()).toContain("調整 Buffer Size");
  expect(document.querySelector(".tour-card")).toBeTruthy();
});

it("僅有非 audio 輸入軌（如 FX）→ 同樣沿用：跳過新增、strip 步驟指向該軌", async () => {
  const fixture = document.createElement("div");
  fixture.insertAdjacentHTML(
    "beforeend",
    '<button data-tour="settings-gear">g</button><div data-tour="set-device">d</div>' +
      '<div data-tour="add-audio">a</div><div data-tour="add-app">ap</div><div data-tour="add-fx">fx</div>' +
      '<div data-tour="set-sessions">s</div>' +
      '<div data-track-id="9"><div data-tour="source">src</div></div>',
  );
  document.body.appendChild(fixture);
  firstId = 9;
  hasTracks = true;
  show();
  await settle();
  button("ASIO").click();
  await settle();
  for (let k = 0; k < 4; k++) {
    button("下一步").click();
    await settle();
  }
  // add-track 被跳過，直接落到 fx 軌的來源步驟
  expect(text()).toContain("選擇輸入來源");
  expect(text()).toContain("6 / 13");
  expect(calls).toContainEqual("ensure:9");
});

it("回歸：dialog 永遠沒開 → 停在步驟置中卡（不自動跳過），手動下一步可離開", async () => {
  const dlg = document.createElement("dialog");
  dlg.innerHTML = '<div data-tour="set-device">dev</div><div data-tour="set-buffer">b</div><div data-tour="set-asiopanel">p</div>';
  document.body.appendChild(dlg);
  const other = document.createElement("div");
  other.innerHTML = '<button data-tour="settings-gear">g</button><div data-tour="add-audio">a</div><div data-tour="add-app">ap</div><div data-tour="add-fx">fx</div>';
  document.body.appendChild(other);
  show();
  await settle();
  button("ASIO").click();
  await settle();
  button("下一步").click(); // 進 asio-device → dialog 永不開
  await settle(300);
  expect(text()).toContain("選擇 ASIO 裝置"); // 不自動跳過，停在原步驟
  expect(document.querySelector(".tour-ring")).toBeNull();
  for (let k = 0; k < 3; k++) {
    button("下一步").click();
    await settle();
  }
  expect(text()).toContain("新增音軌");
});
