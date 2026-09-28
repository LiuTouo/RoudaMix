import assert from "node:assert";
import { test } from "vitest";
import { capsuleItems, type CapsuleNotice } from "../capsules.ts";
import type { MissingPlugin } from "../types.ts";

const notice = (id: number, kind: CapsuleNotice["kind"], msg: string, raw?: string): CapsuleNotice => ({
  id,
  kind,
  msg,
  raw,
});

const missingPlugin = (name: string): MissingPlugin => ({
  trackId: 1,
  trackName: "Stream",
  index: 0,
  name,
  pluginPath: "C:/VST/x.vst3",
  classId: "x",
  code: "load_failed",
  message: "boom",
});

const empty = {
  notices: [] as CapsuleNotice[],
  audioStale: false,
  audioStaleDismissed: false,
  audioStaleDetail: "",
  restoreError: "",
  missing: [] as MissingPlugin[],
};

test("無警示無通知 → 空膠囊清單", () => {
  assert.deepEqual(capsuleItems(empty), []);
});

test("警示在前、通知在後;各 action/關閉語意正確", () => {
  const items = capsuleItems({
    ...empty,
    notices: [notice(1, "error", "切換失敗", "raw-detail"), notice(2, "info", "已複製")],
    audioStale: true,
    audioStaleDetail: "device busy",
    restoreError: "file not found",
    missing: [missingPlugin("Tone 10")],
  });
  assert.deepEqual(
    items.map((c) => c.key),
    ["audioStale", "restoreError", "missing", "notice-1", "notice-2"],
  );
  const [stale, restore, miss, err, info] = items;
  assert.equal(stale.action, "openSettings");
  assert.equal(stale.closable, true);
  assert.match(stale.tooltip, /device busy/);
  assert.equal(restore.action, "openSettings");
  assert.equal(restore.closable, true);
  assert.equal(miss.action, "collapse");
  assert.equal(miss.closable, false);
  assert.match(miss.msg, /1 個 plugin/);
  assert.match(miss.tooltip, /Tone 10.*boom/);
  assert.equal(err.action, "dismiss");
  assert.equal(err.noticeId, 1);
  assert.equal(err.raw, "raw-detail");
  assert.equal(err.closable, false);
  assert.equal(info.noticeId, 2);
  assert.equal(info.raw, undefined);
  assert.equal(info.kind, "info");
});

test("audioStale 關閉後隱藏;錯誤語意色對應 kind", () => {
  const items = capsuleItems({ ...empty, audioStale: true, audioStaleDismissed: true });
  assert.deepEqual(items, []);
  const kinds = capsuleItems({
    ...empty,
    restoreError: "x",
    notices: [notice(3, "error", "e"), notice(4, "info", "i")],
  }).map((c) => c.kind);
  assert.deepEqual(kinds, ["error", "error", "info"]);
});
