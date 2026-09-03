import {
  initialDeviceStream,
  transitionDeviceStream,
  type DeviceStreamConfig,
  type DeviceStreamState,
} from "./deviceStream.ts";
import {
  initialRevisionDirty,
  transitionRevisionDirty,
  type RevisionDirtyState,
} from "./revisionDirty.ts";
import type { EngineStatus, ScanModule, Track } from "./types.ts";

export interface AppliedStatusState {
  status: EngineStatus | null;
  pluginLatencyPdcSupported: boolean;
  scanModules: ScanModule[];
  revisionDirty: RevisionDirtyState;
  deviceStream: DeviceStreamState;
  selection: DeviceStreamConfig | null;
}

export interface AuthoritativeStatusPayload {
  status: EngineStatus;
  lastScan?: ScanModule[] | null;
  capabilities?: string[];
}

export interface ApplyStatusContext {
  scanRunning: boolean;
  devicesLoaded: boolean;
}

export interface ApplyStatusEffects {
  refreshDevices: boolean;
  ensureDefaults: boolean;
  latencyTracks: Track[];
}

export interface ApplyStatusResult {
  state: AppliedStatusState;
  effects: ApplyStatusEffects;
  deviceAccepted: boolean;
}

export function initialAppliedStatus(): AppliedStatusState {
  return {
    status: null,
    pluginLatencyPdcSupported: false,
    scanModules: [],
    revisionDirty: initialRevisionDirty(),
    deviceStream: initialDeviceStream(),
    selection: null,
  };
}

export function applyStatus(
  state: AppliedStatusState,
  payload: AuthoritativeStatusPayload,
  context: ApplyStatusContext,
): ApplyStatusResult {
  const actual =
    payload.status.running && payload.status.deviceKey
      ? {
          deviceKey: payload.status.deviceKey,
          bufferSize: payload.status.bufferSize,
        }
      : null;
  const stream = transitionDeviceStream(state.deviceStream, {
    type: "statusObserved",
    running: payload.status.running,
    actual,
  });

  let revisionDirty = state.revisionDirty;
  if (typeof payload.status.revision === "number") {
    revisionDirty = transitionRevisionDirty(revisionDirty, {
      type: "engineRevisionObserved",
      revision: payload.status.revision,
    });
  }
  const scanModules =
    !context.scanRunning && Array.isArray(payload.lastScan)
      ? payload.lastScan
      : state.scanModules;
  const pluginLatencyPdcSupported =
    payload.capabilities === undefined
      ? state.pluginLatencyPdcSupported
      : payload.capabilities.includes("pluginLatencyPdcV1");
  const status =
    stream.accepted || !state.status
      ? payload.status
      : {
          ...payload.status,
          running: state.status.running,
          deviceKey: state.status.deviceKey,
          sampleRate: state.status.sampleRate,
          bufferSize: state.status.bufferSize,
          inputLatency: state.status.inputLatency,
          outputLatency: state.status.outputLatency,
          xruns: state.status.xruns,
          error: state.status.error,
        };

  return {
    deviceAccepted: stream.accepted,
    state: {
      status,
      pluginLatencyPdcSupported,
      scanModules,
      revisionDirty,
      deviceStream: stream.state,
      selection: stream.accepted && actual ? actual : state.selection,
    },
    effects: {
      refreshDevices: !context.devicesLoaded,
      ensureDefaults: true,
      latencyTracks: payload.status.tracks,
    },
  };
}
