import assert from "node:assert";
import { test } from "node:test";
import {
  beginMonitorBypass,
  finishMonitorBypass,
  initialMonitorBypass,
  isMonitorBypassPending,
  monitorBypassValue,
  reconcileMonitorBypass,
} from "./monitorBypass.ts";

test("Monitor Bypass request 保留確認值並只送出反向意圖", () => {
  const result = beginMonitorBypass(initialMonitorBypass(), 17, false);

  assert.deepEqual(result.command, { instanceId: 17, bypassed: true });
  assert.equal(isMonitorBypassPending(result.state, 17), true);
  assert.equal(monitorBypassValue(result.state, 17, false), false);
});

test("同一 plugin pending 期間拒絕後續值，其他 plugin 仍可獨立 pending", () => {
  const first = beginMonitorBypass(initialMonitorBypass(), 17, false);
  const duplicate = beginMonitorBypass(first.state, 17, true);
  const other = beginMonitorBypass(first.state, 23, true);

  assert.equal(duplicate.command, null);
  assert.equal(duplicate.state, first.state);
  assert.deepEqual(other.command, { instanceId: 23, bypassed: false });
  assert.equal(isMonitorBypassPending(other.state, 17), true);
  assert.equal(isMonitorBypassPending(other.state, 23), true);
});

test("成功採用 requested 值；失敗回滾 confirmed 值並清除 pending", () => {
  const pending = beginMonitorBypass(initialMonitorBypass(), 17, false).state;
  const confirmed = finishMonitorBypass(pending, 17, "confirmed");
  const rejected = finishMonitorBypass(pending, 17, "rejected");

  assert.equal(monitorBypassValue(confirmed, 17, false), true);
  assert.equal(monitorBypassValue(rejected, 17, false), false);
  assert.equal(isMonitorBypassPending(confirmed, 17), false);
  assert.equal(isMonitorBypassPending(rejected, 17), false);
});

test("engine observation 對齊後移除 local value，之後採用外部權威值", () => {
  const pending = beginMonitorBypass(initialMonitorBypass(), 17, false).state;
  const confirmed = finishMonitorBypass(pending, 17, "confirmed");
  const reconciled = reconcileMonitorBypass(confirmed, [[17, true]]);

  assert.notEqual(reconciled, confirmed);
  assert.equal(monitorBypassValue(reconciled, 17, true), true);
  assert.equal(monitorBypassValue(reconciled, 17, false), false);
});

test("重複完成或未知 instance 是冪等操作", () => {
  const state = initialMonitorBypass();

  assert.equal(finishMonitorBypass(state, 999, "confirmed"), state);
  assert.equal(reconcileMonitorBypass(state, []), state);
});
