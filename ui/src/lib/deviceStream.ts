export interface DeviceStreamConfig {
  deviceKey: string;
  bufferSize: number | null;
}

export interface DeviceStreamDevice {
  deviceKey: string;
  name: string;
  preferredBufferSize: number;
  bufferSizes: number[];
}

export interface DeviceStreamFailure {
  code: string;
  message: string;
}

export type DeviceStreamPhase = "idle" | "starting" | "running" | "restarting" | "failed";
export type DeviceStreamRequestKind = "start" | "auto-start" | "restart" | "rollback" | "stop";

export interface DeviceStreamRequest {
  id: number;
  kind: DeviceStreamRequestKind;
  target: DeviceStreamConfig | null;
}

export interface DeviceStreamAutoFailure {
  target: DeviceStreamConfig;
  failure: DeviceStreamFailure;
}

export interface DeviceStreamState {
  phase: DeviceStreamPhase;
  devices: DeviceStreamDevice[];
  request: DeviceStreamRequest | null;
  requestSerial: number;
  lastGood: DeviceStreamConfig | null;
  rollbackTarget: DeviceStreamConfig | null;
  stale: boolean;
  lastStartError: DeviceStreamFailure | null;
  autoStartAttempted: boolean;
  autoStartCandidates: DeviceStreamConfig[];
  autoStartFailures: DeviceStreamAutoFailure[];
}

export type DeviceStreamEvent =
  | { type: "reset" }
  | { type: "devicesChanged"; devices: DeviceStreamDevice[] }
  | {
      type: "statusObserved";
      running: boolean;
      actual: DeviceStreamConfig | null;
    }
  | {
      type: "autoStart";
      preferredDeviceKey: string | null;
      preferredBufferSize: number | null;
    }
  | { type: "startRequested"; target: DeviceStreamConfig }
  | { type: "restartRequested"; target: DeviceStreamConfig }
  | { type: "stopRequested" }
  | { type: "stopSucceeded"; requestId: number }
  | { type: "stopFailed"; requestId: number }
  | {
      type: "startSucceeded";
      requestId: number;
      actual: DeviceStreamConfig;
    }
  | {
      type: "startFailed";
      requestId: number;
      failure: DeviceStreamFailure;
    };

export interface DeviceStreamTransition {
  state: DeviceStreamState;
  accepted: boolean;
}

export function initialDeviceStream(): DeviceStreamState {
  return {
    phase: "idle",
    devices: [],
    request: null,
    requestSerial: 0,
    lastGood: null,
    rollbackTarget: null,
    stale: false,
    lastStartError: null,
    autoStartAttempted: false,
    autoStartCandidates: [],
    autoStartFailures: [],
  };
}

function defaultBuffer(device: DeviceStreamDevice): number | null {
  if (device.bufferSizes.includes(device.preferredBufferSize)) return device.preferredBufferSize;
  return device.bufferSizes[0] ?? null;
}

function sameConfig(
  left: DeviceStreamConfig | null,
  right: DeviceStreamConfig | null,
): boolean {
  if (!left || !right) return left === right;
  return left.deviceKey === right.deviceKey && left.bufferSize === right.bufferSize;
}

export { sameConfig as sameDeviceStreamConfig };

function autoStartCandidates(
  devices: DeviceStreamDevice[],
  preferredDeviceKey: string | null,
  preferredBufferSize: number | null,
): DeviceStreamConfig[] {
  const preferred = devices.find((device) => device.deviceKey === preferredDeviceKey);
  const ordered = preferred
    ? [preferred, ...devices.filter((device) => device.deviceKey !== preferred.deviceKey)]
    : devices;
  return ordered.map((device) => ({
    deviceKey: device.deviceKey,
    bufferSize:
      device === preferred &&
      preferredBufferSize !== null &&
      device.bufferSizes.includes(preferredBufferSize)
        ? preferredBufferSize
        : defaultBuffer(device),
  }));
}

export function transitionDeviceStream(
  state: DeviceStreamState,
  event: DeviceStreamEvent,
): DeviceStreamTransition {
  if (event.type === "reset")
    return {
      state: { ...initialDeviceStream(), requestSerial: state.requestSerial },
      accepted: true,
    };

  if (event.type === "devicesChanged") {
    return { state: { ...state, devices: [...event.devices] }, accepted: true };
  }

  if (event.type === "statusObserved") {
    if (state.request) {
      if (
        !event.running &&
        (state.request.kind === "restart" || state.request.kind === "rollback")
      )
        return { state, accepted: true };
      if (!event.running && state.request.kind === "stop")
        return transitionDeviceStream(state, {
          type: "stopSucceeded",
          requestId: state.request.id,
        });
      if (
        event.running &&
        state.request.kind !== "stop" &&
        sameConfig(state.request.target, event.actual)
      )
        return transitionDeviceStream(state, {
          type: "startSucceeded",
          requestId: state.request.id,
          actual: event.actual!,
        });
      return { state, accepted: false };
    }
    if (event.running && event.actual) {
      return {
        accepted: true,
        state: {
          ...state,
          phase: "running",
          lastGood: event.actual,
          rollbackTarget: null,
          stale: false,
          lastStartError: null,
        },
      };
    }
    if (state.phase === "failed") return { state, accepted: true };
    return {
      accepted: true,
      state: {
        ...state,
        phase: "idle",
        lastGood: null,
        rollbackTarget: null,
        stale: false,
        lastStartError: null,
      },
    };
  }

  if (event.type === "autoStart") {
    if (state.autoStartAttempted || state.request || state.phase === "running")
      return { state, accepted: false };
    const candidates = autoStartCandidates(
      state.devices,
      event.preferredDeviceKey,
      event.preferredBufferSize,
    );
    if (candidates.length === 0) return { state, accepted: false };
    const requestSerial = state.requestSerial + 1;
    return {
      accepted: true,
      state: {
        ...state,
        phase: "starting",
        request: { id: requestSerial, kind: "auto-start", target: candidates[0] },
        requestSerial,
        stale: false,
        lastStartError: null,
        autoStartAttempted: true,
        autoStartCandidates: candidates,
        autoStartFailures: [],
      },
    };
  }

  if (event.type === "startRequested") {
    if (!event.target.deviceKey || state.phase === "running") return { state, accepted: false };
    const requestSerial = state.requestSerial + 1;
    return {
      accepted: true,
      state: {
        ...state,
        phase: "starting",
        request: { id: requestSerial, kind: "start", target: event.target },
        requestSerial,
        rollbackTarget: null,
        stale: false,
        lastStartError: null,
        autoStartCandidates: [],
        autoStartFailures: [],
      },
    };
  }

  if (event.type === "restartRequested") {
    if (!event.target.deviceKey) return { state, accepted: false };
    const requestSerial = state.requestSerial + 1;
    const rollbackTarget =
      state.lastGood && !sameConfig(state.lastGood, event.target) ? state.lastGood : null;
    return {
      accepted: true,
      state: {
        ...state,
        phase: state.lastGood ? "restarting" : "starting",
        request: { id: requestSerial, kind: "restart", target: event.target },
        requestSerial,
        rollbackTarget,
        stale: false,
        lastStartError: null,
        autoStartCandidates: [],
        autoStartFailures: [],
      },
    };
  }

  if (event.type === "stopRequested") {
    if (state.phase === "idle" && !state.request) return { state, accepted: false };
    const requestSerial = state.requestSerial + 1;
    return {
      accepted: true,
      state: {
        ...state,
        request: { id: requestSerial, kind: "stop", target: null },
        requestSerial,
      },
    };
  }

  if (event.type === "stopSucceeded" || event.type === "stopFailed") {
    if (!state.request || state.request.kind !== "stop" || state.request.id !== event.requestId)
      return { state, accepted: false };
    if (event.type === "stopFailed")
      return {
        state: {
          ...state,
          phase: state.lastGood ? "running" : "failed",
          request: null,
          stale: state.lastGood ? state.stale : true,
        },
        accepted: true,
      };
    return {
      accepted: true,
      state: {
        ...state,
        phase: "idle",
        request: null,
        lastGood: null,
        rollbackTarget: null,
        stale: false,
        lastStartError: null,
        autoStartCandidates: [],
        autoStartFailures: [],
      },
    };
  }

  if (
    !state.request ||
    state.request.id !== event.requestId ||
    state.request.kind === "stop"
  )
    return { state, accepted: false };

  if (event.type === "startFailed") {
    const failures =
      state.request.kind === "auto-start" && state.request.target
        ? [
            ...state.autoStartFailures,
            { target: state.request.target, failure: event.failure },
          ]
        : state.autoStartFailures;
    if (state.request.kind === "auto-start") {
      const nextTarget = state.autoStartCandidates[failures.length];
      if (nextTarget) {
        const requestSerial = state.requestSerial + 1;
        return {
          accepted: true,
          state: {
            ...state,
            request: { id: requestSerial, kind: "auto-start", target: nextTarget },
            requestSerial,
            lastStartError: event.failure,
            autoStartFailures: failures,
          },
        };
      }
    }
    if (state.request.kind === "restart" && state.rollbackTarget) {
      const requestSerial = state.requestSerial + 1;
      return {
        accepted: true,
        state: {
          ...state,
          phase: "restarting",
          request: {
            id: requestSerial,
            kind: "rollback",
            target: state.rollbackTarget,
          },
          requestSerial,
          stale: true,
          lastStartError: event.failure,
        },
      };
    }
    return {
      accepted: true,
      state: {
        ...state,
        phase: "failed",
        request: null,
        stale: true,
        lastStartError: event.failure,
        autoStartCandidates: [],
        autoStartFailures: failures,
      },
    };
  }

  return {
    accepted: true,
    state: {
      ...state,
      phase: "running",
      request: null,
      lastGood: event.actual,
      rollbackTarget: null,
      stale: false,
      lastStartError: state.request.kind === "rollback" ? state.lastStartError : null,
      autoStartCandidates: [],
      autoStartFailures:
        state.request.kind === "auto-start" ? state.autoStartFailures : [],
    },
  };
}

export function isDeviceStreamBusy(state: DeviceStreamState): boolean {
  return state.request !== null;
}
