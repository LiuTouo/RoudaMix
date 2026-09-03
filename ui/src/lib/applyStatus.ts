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
  latencyEnabled: boolean;
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
  accepted: boolean;
}

export function initialAppliedStatus(): AppliedStatusState {
  return {
    status: null,
    latencyEnabled: false,
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
  if (!stream.accepted) {
    return {
      state,
      accepted: false,
      effects: {
        refreshDevices: false,
        ensureDefaults: false,
        latencyTracks: [],
      },
    };
  }

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
  const latencyEnabled =
    payload.capabilities === undefined
      ? state.latencyEnabled
      : payload.capabilities.includes("pluginLatencyPdcV1");

  return {
    accepted: true,
    state: {
      status: payload.status,
      latencyEnabled,
      scanModules,
      revisionDirty,
      deviceStream: stream.state,
      selection: actual ?? state.selection,
    },
    effects: {
      refreshDevices: !context.devicesLoaded,
      ensureDefaults: true,
      latencyTracks: payload.status.tracks,
    },
  };
}
