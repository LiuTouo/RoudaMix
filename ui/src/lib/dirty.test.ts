// P0 dirty 判定(P1 保留行為鎖定):revision 權威模型回歸
import { test } from "node:test";
import assert from "node:assert";
import { isDirty, resolveDirtyChoice } from "./dirty.ts";

test("isDirty:未知 revision(未連上)或無基準(未存未載)= 不髒", () => {
  assert.equal(isDirty(null, null), false);
  assert.equal(isDirty(5, null), false);
  assert.equal(isDirty(null, 5), false);
});

test("isDirty:revision 離開基準才髒", () => {
  assert.equal(isDirty(5, 5), false);
  assert.equal(isDirty(6, 5), true);
  assert.equal(isDirty(4, 5), true);
});

test("resolveDirtyChoice:三分支 —— cancel 擋、discard 過、save 先存(成功才 proceed)", () => {
  assert.deepEqual(resolveDirtyChoice(true, "cancel"), { proceed: false, shouldSave: false });
  assert.deepEqual(resolveDirtyChoice(true, "discard"), { proceed: true, shouldSave: false });
  assert.deepEqual(resolveDirtyChoice(true, "save"), { proceed: false, shouldSave: true });
  // 不髒 = 一律直接過
  for (const c of ["save", "discard", "cancel"] as const) {
    assert.deepEqual(resolveDirtyChoice(false, c), { proceed: true, shouldSave: false });
  }
});
