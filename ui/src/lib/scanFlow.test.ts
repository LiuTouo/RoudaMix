import assert from "node:assert";
import { test } from "node:test";
import { matchScanJob } from "./scanFlow.ts";

test("start_scan reply 前先到的 event 會綁定目前 job", () => {
  assert.deepEqual(matchScanJob(null, true, 7), { matches: true, jobId: 7 });
});

test("已知 job 只接受相同 id，非 pending 不收陌生 event", () => {
  assert.deepEqual(matchScanJob(7, false, 7), { matches: true, jobId: 7 });
  assert.deepEqual(matchScanJob(7, false, 8), { matches: false, jobId: 7 });
  assert.deepEqual(matchScanJob(null, false, 8), { matches: false, jobId: null });
});
