import assert from "node:assert";
import { test } from "node:test";
import { applyStatus, initialAppliedStatus } from "./applyStatus.ts";
import { transitionDeviceStream } from "./deviceStream.ts";
import { isRevisionDirty, transitionRevisionDirty } from "./revisionDirty.ts";
import type { EngineStatus, ScanModule } from "./types.ts";

function engineStatus(overrides: Partial<EngineStatus> = {}): EngineStatus {
  return {
    running: false,
    deviceKey: null,
    sampleRate: 0,
    bufferSize: null,
    inputLatency: null,
    outputLatency: null,
    xruns: 0,
    trackCount: 0,
    pluginFails: 0,
    revision: 5,
    telemetryStrips: [],
    tracks: [],
    error: null,
    ...overrides,
  };
}

const scan: ScanModule[] = [{ path: "synth.vst3", classes: [] }];

test("權威 payload 一次縮減 status、capability、掃描清單、dirty 與裝置選擇", () => {
  const initial = initialAppliedStatus();
  initial.revisionDirty = transitionRevisionDirty(initial.revisionDirty, {
    type: "baselineConfirmed",
    revision: 4,
  });
  const status = engineStatus({
    running: true,
    deviceKey: "asio:one",
    bufferSize: 256,
  });

  const applied = applyStatus(
    initial,
    { status, capabilities: ["pluginLatencyPdcV1"], lastScan: scan },
    { scanRunning: false, devicesLoaded: false },
  );

  assert.equal(applied.accepted, true);
  assert.equal(applied.state.status, status);
  assert.equal(applied.state.latencyEnabled, true);
  assert.equal(applied.state.scanModules, scan);
  assert.equal(isRevisionDirty(applied.state.revisionDirty), true);
  assert.equal(applied.state.deviceStream.phase, "running");
  assert.deepEqual(applied.state.deviceStream.lastGood, {
    deviceKey: "asio:one",
    bufferSize: 256,
  });
  assert.deepEqual(applied.state.selection, {
    deviceKey: "asio:one",
    bufferSize: 256,
  });
  assert.deepEqual(applied.effects, {
    refreshDevices: true,
    ensureDefaults: true,
    latencyTracks: status.tracks,
  });
});

test("status-only 事件保留 capability，掃描進行中不覆寫既有 registry", () => {
  const previousScan: ScanModule[] = [{ path: "existing.vst3", classes: [] }];
  const initial = {
    ...initialAppliedStatus(),
    latencyEnabled: true,
    scanModules: previousScan,
  };
  const applied = applyStatus(
    initial,
    { status: engineStatus({ revision: 6 }), lastScan: scan },
    { scanRunning: true, devicesLoaded: true },
  );

  assert.equal(applied.state.latencyEnabled, true);
  assert.equal(applied.state.scanModules, previousScan);
  assert.equal(applied.effects.refreshDevices, false);
  assert.equal(applied.effects.ensureDefaults, true);
});

test("裝置狀態機拒絕遲到 payload 時，權威縮減不覆寫較新的 UI 狀態", () => {
  const current = engineStatus({
    running: true,
    deviceKey: "asio:first",
    bufferSize: 128,
  });
  const running = applyStatus(
    initialAppliedStatus(),
    { status: current },
    { scanRunning: false, devicesLoaded: true },
  ).state;
  const pending = {
    ...running,
    deviceStream: transitionDeviceStream(running.deviceStream, {
      type: "restartRequested",
      target: { deviceKey: "asio:preferred", bufferSize: 512 },
    }).state,
  };
  const late = applyStatus(
    pending,
    { status: current },
    { scanRunning: false, devicesLoaded: true },
  );

  assert.equal(late.accepted, false);
  assert.equal(late.state, pending);
});
