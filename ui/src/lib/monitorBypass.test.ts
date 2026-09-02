import assert from "node:assert";
import { test } from "node:test";
import {
  beginMonitorBypass,
  finishMonitorBypass,
  isMonitorBypassPending,
  type MonitorBypassTransactions,
} from "./monitorBypass.ts";

const empty = (): MonitorBypassTransactions => new Map();

test("Monitor Bypass request 保留確認值並只送出反向意圖", () => {
  const result = beginMonitorBypass(empty(), 17, false);

  assert.deepEqual(result.command, { instanceId: 17, bypassed: true });
  assert.equal(isMonitorBypassPending(result.transactions, 17), true);
});

test("同一 plugin pending 期間拒絕後續值，其他 plugin 仍可獨立 pending", () => {
  const first = beginMonitorBypass(empty(), 17, false);
  const duplicate = beginMonitorBypass(first.transactions, 17, true);
  const other = beginMonitorBypass(first.transactions, 23, true);

  assert.equal(duplicate.command, null);
  assert.equal(duplicate.transactions, first.transactions);
  assert.deepEqual(other.command, { instanceId: 23, bypassed: false });
  assert.equal(isMonitorBypassPending(other.transactions, 17), true);
  assert.equal(isMonitorBypassPending(other.transactions, 23), true);
});

test("成功採用 requested 值；失敗回滾 confirmed 值並清除 pending", () => {
  const pending = beginMonitorBypass(empty(), 17, false).transactions;
  const confirmed = finishMonitorBypass(pending, 17, "confirmed");
  const rejected = finishMonitorBypass(pending, 17, "rejected");

  assert.equal(confirmed.value, true);
  assert.equal(rejected.value, false);
  assert.equal(isMonitorBypassPending(confirmed.transactions, 17), false);
  assert.equal(isMonitorBypassPending(rejected.transactions, 17), false);
});

test("重複完成或未知 instance 是冪等操作", () => {
  const transactions = empty();
  const result = finishMonitorBypass(transactions, 999, "confirmed");

  assert.equal(result.transactions, transactions);
  assert.equal(result.value, null);
});
