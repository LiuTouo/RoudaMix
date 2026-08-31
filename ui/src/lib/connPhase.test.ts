// P1-A/P1-L 測試:連線狀態分類、epoch 變更、初始化同步策略
import { test } from "node:test";
import assert from "node:assert";
import { classifyConnError, connView, needsActiveSnapshot, epochChanged } from "./connPhase.ts";

test("connView:五個 phase 標籤 + tone", () => {
  const base = { connected: false, epoch: 0, engineVersion: "" };
  assert.equal(connView({ ...base, phase: "connected", connected: true }).label, "已連線");
  assert.equal(connView({ ...base, phase: "connected", connected: true }).tone, "ok");
  assert.equal(connView({ ...base, phase: "spawning" }).label, "引擎啟動中…");
  assert.equal(connView({ ...base, phase: "spawn_failed", detail: "exe not found" }).tone, "err");
  assert.match(connView({ ...base, phase: "spawn_failed", detail: "exe not found" }).detail, /exe/);
  assert.equal(connView({ ...base, phase: "disconnected" }).tone, "warn");
});

test("connView:舊 bridge 無 phase → 以 connected 判斷 fallback", () => {
  const v = connView({ connected: true, epoch: 1, engineVersion: "0.1.0" });
  assert.equal(v.phase, "connected");
  assert.equal(connView({ connected: false, epoch: 0, engineVersion: "" }).phase, "connecting");
});

test("connView:未知 phase 防禦 → connecting", () => {
  const base = { connected: false, epoch: 0, engineVersion: "" };
  // @ts-expect-error 故意塞未知值
  assert.equal(connView({ ...base, phase: "warp" }).phase, "connecting");
});

test("classifyConnError:unsupported_version 分類", () => {
  assert.equal(classifyConnError("unsupported_version: server speaks protocol v2"), "version_mismatch");
  assert.equal(classifyConnError("timeout"), null);
  assert.equal(classifyConnError(""), null);
});

test("connView:命令錯誤帶 unsupported_version → version_mismatch 顯示", () => {
  const v = connView({ connected: true, epoch: 1, engineVersion: "" }, "unsupported_version: server speaks protocol v2");
  assert.equal(v.phase, "version_mismatch");
  assert.equal(v.tone, "err");
});

test("needsActiveSnapshot:listener 掛好後永遠主動拉(snapshot 事件可能早已錯過)", () => {
  assert.equal(needsActiveSnapshot(true), true);
  assert.equal(needsActiveSnapshot(false), true);
});

test("epochChanged:0(未知)不算變更;非 0 相異才算(reconnect 重置本地一次性旗標)", () => {
  assert.equal(epochChanged(0, 5), false);
  assert.equal(epochChanged(3, 0), false);
  assert.equal(epochChanged(3, 3), false);
  assert.equal(epochChanged(3, 7), true);
});
