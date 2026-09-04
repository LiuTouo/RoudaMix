import assert from "node:assert";
import { test } from "vitest";
import {
  initialDeviceStream,
  isDeviceStreamBusy,
  transitionDeviceStream,
  type DeviceStreamDevice,
} from "../deviceStream.ts";

const devices: DeviceStreamDevice[] = [
  {
    deviceKey: "asio:first",
    name: "First",
    preferredBufferSize: 256,
    bufferSizes: [128, 256],
  },
  {
    deviceKey: "asio:preferred",
    name: "Preferred",
    preferredBufferSize: 512,
    bufferSizes: [256, 512],
  },
];

test("自動啟動依上次成功設定優先，成功後成為 running 與 lastGood", () => {
  const listed = transitionDeviceStream(initialDeviceStream(), {
    type: "devicesChanged",
    devices,
  });
  const requested = transitionDeviceStream(listed.state, {
    type: "autoStart",
    preferredDeviceKey: "asio:preferred",
    preferredBufferSize: 256,
  });

  assert.equal(listed.accepted, true);
  assert.equal(requested.accepted, true);
  assert.equal(requested.state.phase, "starting");
  assert.deepEqual(requested.state.request?.target, {
    deviceKey: "asio:preferred",
    bufferSize: 256,
  });

  const succeeded = transitionDeviceStream(requested.state, {
    type: "startSucceeded",
    requestId: requested.state.request!.id,
    actual: requested.state.request!.target!,
  });

  assert.equal(succeeded.accepted, true);
  assert.equal(succeeded.state.phase, "running");
  assert.deepEqual(succeeded.state.lastGood, {
    deviceKey: "asio:preferred",
    bufferSize: 256,
  });
  assert.equal(succeeded.state.request, null);
  assert.equal(succeeded.state.stale, false);
});

test("自動啟動逐一嘗試候選且整輪只接受一次", () => {
  const listed = transitionDeviceStream(initialDeviceStream(), {
    type: "devicesChanged",
    devices,
  }).state;
  const requested = transitionDeviceStream(listed, {
    type: "autoStart",
    preferredDeviceKey: null,
    preferredBufferSize: null,
  }).state;
  const firstFailed = transitionDeviceStream(requested, {
    type: "startFailed",
    requestId: requested.request!.id,
    failure: { code: "device_busy", message: "first is busy" },
  });

  assert.equal(firstFailed.accepted, true);
  assert.equal(firstFailed.state.phase, "starting");
  assert.equal(firstFailed.state.request?.kind, "auto-start");
  assert.deepEqual(firstFailed.state.request?.target, {
    deviceKey: "asio:preferred",
    bufferSize: 512,
  });
  assert.equal(firstFailed.state.autoStartFailures.length, 1);

  const exhausted = transitionDeviceStream(firstFailed.state, {
    type: "startFailed",
    requestId: firstFailed.state.request!.id,
    failure: { code: "device_open_failed", message: "second failed" },
  });
  // M6:兩台 ASIO 全敗後,最後候選 = 系統音訊(WASAPI master fallback)
  assert.equal(exhausted.state.phase, "starting");
  assert.deepEqual(exhausted.state.request?.target, {
    deviceKey: "wasapi",
    bufferSize: null,
  });
  assert.equal(exhausted.state.autoStartFailures.length, 2);

  const wasapiFailed = transitionDeviceStream(exhausted.state, {
    type: "startFailed",
    requestId: exhausted.state.request!.id,
    failure: { code: "device_open_failed", message: "no render endpoint" },
  });
  const duplicate = transitionDeviceStream(wasapiFailed.state, {
    type: "autoStart",
    preferredDeviceKey: null,
    preferredBufferSize: null,
  });

  assert.equal(wasapiFailed.state.phase, "failed");
  assert.equal(wasapiFailed.state.request, null);
  assert.equal(wasapiFailed.state.stale, true);
  assert.equal(wasapiFailed.state.autoStartFailures.length, 3);
  assert.equal(duplicate.accepted, false);
  assert.equal(duplicate.state, wasapiFailed.state);
});

test("無 ASIO 裝置時自動啟動直接以系統音訊(WASAPI master)為候選", () => {
  const listed = transitionDeviceStream(initialDeviceStream(), {
    type: "devicesChanged",
    devices: [],
  });
  const requested = transitionDeviceStream(listed.state, {
    type: "autoStart",
    preferredDeviceKey: null,
    preferredBufferSize: null,
  });

  assert.equal(requested.accepted, true);
  assert.equal(requested.state.phase, "starting");
  assert.deepEqual(requested.state.request?.target, {
    deviceKey: "wasapi",
    bufferSize: null,
  });

  const succeeded = transitionDeviceStream(requested.state, {
    type: "startSucceeded",
    requestId: requested.state.request!.id,
    actual: { deviceKey: "wasapi", bufferSize: null },
  });
  assert.equal(succeeded.accepted, true);
  assert.equal(succeeded.state.phase, "running");
  assert.deepEqual(succeeded.state.lastGood, {
    deviceKey: "wasapi",
    bufferSize: null,
  });
});

test("裝置切換失敗後以 lastGood 回滾，保留原失敗供 UI 呈現", () => {
  const original = { deviceKey: "asio:first", bufferSize: 128 };
  const observed = transitionDeviceStream(initialDeviceStream(), {
    type: "statusObserved",
    running: true,
    actual: original,
  }).state;
  const restart = transitionDeviceStream(observed, {
    type: "restartRequested",
    target: { deviceKey: "asio:preferred", bufferSize: 512 },
  }).state;
  const failed = transitionDeviceStream(restart, {
    type: "startFailed",
    requestId: restart.request!.id,
    failure: { code: "device_open_failed", message: "cannot open preferred" },
  });

  assert.equal(failed.accepted, true);
  assert.equal(failed.state.phase, "restarting");
  assert.equal(failed.state.request?.kind, "rollback");
  assert.deepEqual(failed.state.request?.target, original);
  assert.deepEqual(failed.state.rollbackTarget, original);
  assert.equal(failed.state.stale, true);

  const rolledBack = transitionDeviceStream(failed.state, {
    type: "startSucceeded",
    requestId: failed.state.request!.id,
    actual: original,
  });

  assert.equal(rolledBack.state.phase, "running");
  assert.deepEqual(rolledBack.state.lastGood, original);
  assert.equal(rolledBack.state.stale, false);
  assert.equal(rolledBack.state.lastStartError?.code, "device_open_failed");
});

test("新請求取代舊請求後，遲到回覆與不相符的引擎狀態都被拒絕", () => {
  const original = { deviceKey: "asio:first", bufferSize: 128 };
  const running = transitionDeviceStream(initialDeviceStream(), {
    type: "statusObserved",
    running: true,
    actual: original,
  }).state;
  const first = transitionDeviceStream(running, {
    type: "restartRequested",
    target: { deviceKey: "asio:preferred", bufferSize: 256 },
  }).state;
  const latest = transitionDeviceStream(first, {
    type: "restartRequested",
    target: { deviceKey: "asio:preferred", bufferSize: 512 },
  }).state;

  const lateReply = transitionDeviceStream(latest, {
    type: "startSucceeded",
    requestId: first.request!.id,
    actual: first.request!.target!,
  });
  const lateStatus = transitionDeviceStream(latest, {
    type: "statusObserved",
    running: true,
    actual: first.request!.target!,
  });

  assert.equal(lateReply.accepted, false);
  assert.equal(lateReply.state, latest);
  assert.equal(lateStatus.accepted, false);
  assert.equal(lateStatus.state, latest);

  const currentStatus = transitionDeviceStream(latest, {
    type: "statusObserved",
    running: true,
    actual: latest.request!.target!,
  });
  const duplicateReply = transitionDeviceStream(currentStatus.state, {
    type: "startSucceeded",
    requestId: latest.request!.id,
    actual: latest.request!.target!,
  });

  assert.equal(currentStatus.accepted, true);
  assert.equal(currentStatus.state.phase, "running");
  assert.equal(duplicateReply.accepted, false);
  assert.equal(duplicateReply.state, currentStatus.state);
});

test("停止請求的狀態事件可先於回覆完成，reset 會清除連線中斷時的 pending 狀態", () => {
  const original = { deviceKey: "asio:first", bufferSize: 128 };
  const running = transitionDeviceStream(initialDeviceStream(), {
    type: "statusObserved",
    running: true,
    actual: original,
  }).state;
  const stopping = transitionDeviceStream(running, { type: "stopRequested" });

  assert.equal(stopping.accepted, true);
  assert.equal(stopping.state.phase, "running");
  assert.equal(stopping.state.request?.kind, "stop");
  assert.equal(isDeviceStreamBusy(stopping.state), true);

  const stoppedByEvent = transitionDeviceStream(stopping.state, {
    type: "statusObserved",
    running: false,
    actual: null,
  });
  const lateReply = transitionDeviceStream(stoppedByEvent.state, {
    type: "stopSucceeded",
    requestId: stopping.state.request!.id,
  });

  assert.equal(stoppedByEvent.state.phase, "idle");
  assert.equal(stoppedByEvent.state.request, null);
  assert.equal(stoppedByEvent.state.lastGood, null);
  assert.equal(isDeviceStreamBusy(stoppedByEvent.state), false);
  assert.equal(lateReply.accepted, false);

  const stopFailed = transitionDeviceStream(stopping.state, {
    type: "stopFailed",
    requestId: stopping.state.request!.id,
  });
  assert.equal(stopFailed.state.phase, "running");
  assert.deepEqual(stopFailed.state.lastGood, original);
  assert.equal(isDeviceStreamBusy(stopFailed.state), false);

  const pending = transitionDeviceStream(running, {
    type: "restartRequested",
    target: { deviceKey: "asio:preferred", bufferSize: 512 },
  }).state;
  const reset = transitionDeviceStream(pending, { type: "reset" });

  assert.equal(reset.accepted, true);
  assert.deepEqual(reset.state, {
    ...initialDeviceStream(),
    requestSerial: pending.requestSerial,
  });

  const afterReconnect = transitionDeviceStream(reset.state, {
    type: "startRequested",
    target: original,
  }).state;
  const previousEpochReply = transitionDeviceStream(afterReconnect, {
    type: "startSucceeded",
    requestId: pending.request!.id,
    actual: pending.request!.target!,
  });
  assert.equal(previousEpochReply.accepted, false);
  assert.equal(previousEpochReply.state, afterReconnect);
});
