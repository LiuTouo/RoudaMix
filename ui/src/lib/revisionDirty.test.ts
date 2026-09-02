import assert from "node:assert";
import { test } from "node:test";
import {
  initialRevisionDirty,
  isRevisionDirty,
  resolveDirtyChoice,
  transitionRevisionDirty,
} from "./revisionDirty.ts";

test("未知 revision 或尚無 clean baseline 時不標記 dirty", () => {
  const initial = initialRevisionDirty();
  const observed = transitionRevisionDirty(initial, {
    type: "engineRevisionObserved",
    revision: 5,
  });

  assert.equal(isRevisionDirty(initial), false);
  assert.equal(isRevisionDirty(observed), false);
});

test("只有 engine 權威 revision 推進會離開 clean baseline", () => {
  const clean = transitionRevisionDirty(initialRevisionDirty(), {
    type: "baselineConfirmed",
    revision: 5,
  });
  const unchanged = transitionRevisionDirty(clean, {
    type: "engineRevisionObserved",
    revision: 5,
  });
  const advanced = transitionRevisionDirty(clean, {
    type: "engineRevisionObserved",
    revision: 6,
  });

  assert.equal(unchanged, clean);
  assert.equal(isRevisionDirty(unchanged), false);
  assert.equal(isRevisionDirty(advanced), true);
});

test("runtime observation 與重複事件不推進 dirty revision", () => {
  const clean = transitionRevisionDirty(initialRevisionDirty(), {
    type: "baselineConfirmed",
    revision: 5,
  });
  const latencyObserved = transitionRevisionDirty(clean, { type: "runtimeObserved" });
  const duplicateStatus = transitionRevisionDirty(latencyObserved, {
    type: "engineRevisionObserved",
    revision: 5,
  });

  assert.equal(latencyObserved, clean);
  assert.equal(duplicateStatus, clean);
  assert.equal(isRevisionDirty(duplicateStatus), false);
});

test("save/load 成功建立新 baseline；失敗保留原 dirty 狀態", () => {
  const dirty = transitionRevisionDirty(
    transitionRevisionDirty(initialRevisionDirty(), {
      type: "baselineConfirmed",
      revision: 5,
    }),
    { type: "engineRevisionObserved", revision: 6 },
  );
  const rejected = transitionRevisionDirty(dirty, { type: "baselineRejected" });
  const saved = transitionRevisionDirty(dirty, {
    type: "baselineConfirmed",
    revision: 6,
  });
  const loaded = transitionRevisionDirty(saved, {
    type: "baselineConfirmed",
    revision: 2,
  });

  assert.equal(rejected, dirty);
  assert.equal(isRevisionDirty(rejected), true);
  assert.equal(isRevisionDirty(saved), false);
  assert.deepEqual(loaded, { revision: 2, cleanRevision: 2 });
});

test("dirty choice：cancel 擋、discard 過、save 先存；clean 時直接過", () => {
  assert.deepEqual(resolveDirtyChoice(true, "cancel"), {
    proceed: false,
    shouldSave: false,
  });
  assert.deepEqual(resolveDirtyChoice(true, "discard"), {
    proceed: true,
    shouldSave: false,
  });
  assert.deepEqual(resolveDirtyChoice(true, "save"), {
    proceed: false,
    shouldSave: true,
  });
  assert.deepEqual(resolveDirtyChoice(false, "save"), {
    proceed: true,
    shouldSave: false,
  });
});
