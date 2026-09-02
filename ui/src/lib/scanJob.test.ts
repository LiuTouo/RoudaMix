import assert from "node:assert";
import { test } from "node:test";
import {
  initialScanJob,
  isScanJobRunning,
  scanCompletionNotice,
  transitionScanJob,
} from "./scanJob.ts";

test("scan job 從 idle 經 request/reply 進入已綁定 job 的 scanning", () => {
  const started = transitionScanJob(initialScanJob(), { type: "startRequested" });
  const confirmed = transitionScanJob(started.state, {
    type: "startConfirmed",
    jobId: 7,
  });

  assert.equal(started.accepted, true);
  assert.equal(started.state.requestPending, true);
  assert.equal(confirmed.state.phase, "scanning");
  assert.equal(confirmed.state.jobId, 7);
  assert.equal(confirmed.state.requestPending, false);
  assert.equal(isScanJobRunning(confirmed.state), true);
});

test("reply 前的 event 會綁定目前 job；後到的 reply 不覆蓋 event jobId", () => {
  const started = transitionScanJob(initialScanJob(), { type: "startRequested" }).state;
  const progressed = transitionScanJob(started, {
    type: "progress",
    jobId: 7,
    done: 2,
    total: 5,
  });
  const confirmed = transitionScanJob(progressed.state, {
    type: "startConfirmed",
    jobId: 8,
  });

  assert.equal(progressed.accepted, true);
  assert.equal(progressed.state.jobId, 7);
  assert.deepEqual(progressed.state.progress, { done: 2, total: 5 });
  assert.equal(confirmed.state.jobId, 7);
  assert.equal(confirmed.state.requestPending, false);
});

test("running job 拒絕重複 start 與其他 job 的 late event", () => {
  const running = transitionScanJob(
    transitionScanJob(initialScanJob(), { type: "startRequested" }).state,
    { type: "startConfirmed", jobId: 7 },
  ).state;
  const duplicateStart = transitionScanJob(running, { type: "startRequested" });
  const lateProgress = transitionScanJob(running, {
    type: "progress",
    jobId: 6,
    done: 5,
    total: 5,
  });

  assert.equal(duplicateStart.accepted, false);
  assert.equal(duplicateStart.state, running);
  assert.equal(lateProgress.accepted, false);
  assert.equal(lateProgress.state, running);
});

test("完成、失敗、取消與 dismiss 都有明確轉移", () => {
  const starting = transitionScanJob(initialScanJob(), { type: "startRequested" }).state;
  const completed = transitionScanJob(starting, {
    type: "completed",
    jobId: 7,
    modules: ["synth"],
    failures: ["broken"],
  });
  const failed = transitionScanJob(starting, {
    type: "failed",
    jobId: 7,
    error: "scan failed",
  });
  const cancelled = transitionScanJob(starting, { type: "cancelled", jobId: 7 });

  assert.deepEqual(completed.state.result, {
    outcome: "success",
    modules: ["synth"],
    failures: ["broken"],
  });
  assert.equal(completed.state.phase, "completed");
  assert.equal(failed.state.phase, "failed");
  assert.equal(failed.state.error, "scan failed");
  assert.deepEqual(cancelled.state.result, {
    outcome: "cancelled",
    modules: [],
    failures: [],
  });
  assert.equal(
    transitionScanJob(failed.state, { type: "dismiss", jobId: 7 }).state.phase,
    "idle",
  );
  assert.equal(isScanJobRunning(completed.state), false);
});

test("完成通知描述結果、三秒自動 dismiss 且不提供手動關閉", () => {
  assert.deepEqual(scanCompletionNotice(4, 0), {
    message: "VST 清單已更新：4 個模組",
    autoDismissMs: 3000,
    dismissible: false,
  });
  assert.deepEqual(scanCompletionNotice(4, 2), {
    message: "VST 清單已更新：4 個模組，2 個無法載入",
    autoDismissMs: 3000,
    dismissible: false,
  });
});

test("start 失敗可回滾為 failed；cancel request 失敗維持 scanning", () => {
  const starting = transitionScanJob(initialScanJob(), { type: "startRequested" }).state;
  const startRejected = transitionScanJob(starting, {
    type: "startRejected",
    error: "not connected",
  });
  const cancelRejected = transitionScanJob(starting, {
    type: "cancelRejected",
    error: "cancel failed",
  });

  assert.equal(startRejected.state.phase, "failed");
  assert.equal(startRejected.state.jobId, null);
  assert.equal(cancelRejected.state.phase, "scanning");
  assert.equal(cancelRejected.state.error, "cancel failed");
});

test("reset 與 terminal state 收到重複 late event 都保持冪等", () => {
  const starting = transitionScanJob(initialScanJob(), { type: "startRequested" }).state;
  const completed = transitionScanJob(starting, {
    type: "completed",
    jobId: 7,
    modules: [],
    failures: [],
  }).state;
  const duplicate = transitionScanJob(completed, {
    type: "completed",
    jobId: 7,
    modules: ["duplicate"],
    failures: [],
  });
  const reset = transitionScanJob(completed, { type: "reset" });
  const staleDismiss = transitionScanJob(completed, { type: "dismiss", jobId: 6 });

  assert.equal(duplicate.accepted, false);
  assert.equal(duplicate.state, completed);
  assert.equal(staleDismiss.accepted, false);
  assert.equal(staleDismiss.state, completed);
  assert.deepEqual(reset.state, initialScanJob());
});
