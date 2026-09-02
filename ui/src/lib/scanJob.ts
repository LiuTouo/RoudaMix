export type ScanJobPhase = "idle" | "scanning" | "completed" | "failed";

export interface ScanProgress {
  done: number;
  total: number;
}

export interface ScanCompletionNotice {
  message: string;
  autoDismissMs: 3000;
  dismissible: false;
}

export type ScanJobResult<TModule, TFailure> =
  | { outcome: "success"; modules: TModule[]; failures: TFailure[] }
  | { outcome: "cancelled"; modules: []; failures: [] };

export interface ScanJobState<TModule = unknown, TFailure = unknown> {
  phase: ScanJobPhase;
  jobId: number | null;
  requestPending: boolean;
  progress: ScanProgress | null;
  error: string;
  result: ScanJobResult<TModule, TFailure> | null;
}

export type ScanJobEvent<TModule, TFailure> =
  | { type: "startRequested" }
  | { type: "startConfirmed"; jobId: number }
  | { type: "startRejected"; error: string }
  | { type: "progress"; jobId: number; done: number; total: number }
  | { type: "completed"; jobId: number; modules: TModule[]; failures: TFailure[] }
  | { type: "failed"; jobId: number; error: string }
  | { type: "cancelled"; jobId: number }
  | { type: "cancelRejected"; error: string }
  | { type: "dismiss"; jobId: number | null }
  | { type: "reset" };

export interface ScanJobTransition<TModule, TFailure> {
  state: ScanJobState<TModule, TFailure>;
  accepted: boolean;
}

export function initialScanJob<TModule = unknown, TFailure = unknown>(): ScanJobState<
  TModule,
  TFailure
> {
  return {
    phase: "idle",
    jobId: null,
    requestPending: false,
    progress: null,
    error: "",
    result: null,
  };
}

function jobMatches<TModule, TFailure>(
  state: ScanJobState<TModule, TFailure>,
  jobId: number,
): { matches: boolean; jobId: number | null } {
  if (state.phase !== "scanning") return { matches: false, jobId: state.jobId };
  if (state.jobId === jobId) return { matches: true, jobId };
  if (state.requestPending && state.jobId === null) return { matches: true, jobId };
  return { matches: false, jobId: state.jobId };
}

export function transitionScanJob<TModule, TFailure>(
  state: ScanJobState<TModule, TFailure>,
  event: ScanJobEvent<TModule, TFailure>,
): ScanJobTransition<TModule, TFailure> {
  if (event.type === "reset") return { state: initialScanJob(), accepted: true };

  if (event.type === "startRequested") {
    if (state.phase === "scanning") return { state, accepted: false };
    return {
      accepted: true,
      state: {
        phase: "scanning",
        jobId: null,
        requestPending: true,
        progress: null,
        error: "",
        result: null,
      },
    };
  }

  if (event.type === "dismiss") {
    if (state.jobId !== event.jobId) return { state, accepted: false };
    if (state.phase === "completed" || state.phase === "failed")
      return { state: initialScanJob(), accepted: true };
    if (state.phase === "scanning" && state.error)
      return { state: { ...state, error: "" }, accepted: true };
    return { state, accepted: false };
  }

  if (event.type === "startRejected") {
    if (state.phase !== "scanning" || !state.requestPending)
      return { state, accepted: false };
    return {
      accepted: true,
      state: {
        phase: "failed",
        jobId: null,
        requestPending: false,
        progress: null,
        error: event.error,
        result: null,
      },
    };
  }

  if (event.type === "startConfirmed") {
    if (state.phase !== "scanning" || !state.requestPending)
      return { state, accepted: false };
    return {
      accepted: true,
      state: {
        ...state,
        jobId: state.jobId ?? event.jobId,
        requestPending: false,
      },
    };
  }

  if (event.type === "cancelRejected") {
    if (state.phase !== "scanning") return { state, accepted: false };
    return { state: { ...state, error: event.error }, accepted: true };
  }

  const match = jobMatches(state, event.jobId);
  if (!match.matches) return { state, accepted: false };

  if (event.type === "progress") {
    return {
      accepted: true,
      state: {
        ...state,
        jobId: match.jobId,
        progress: { done: event.done, total: event.total },
      },
    };
  }

  if (event.type === "completed") {
    return {
      accepted: true,
      state: {
        phase: "completed",
        jobId: match.jobId,
        requestPending: false,
        progress: null,
        error: "",
        result: {
          outcome: "success",
          modules: event.modules,
          failures: event.failures,
        },
      },
    };
  }

  if (event.type === "failed") {
    return {
      accepted: true,
      state: {
        phase: "failed",
        jobId: match.jobId,
        requestPending: false,
        progress: null,
        error: event.error,
        result: null,
      },
    };
  }

  return {
    accepted: true,
    state: {
      phase: "completed",
      jobId: match.jobId,
      requestPending: false,
      progress: null,
      error: "",
      result: { outcome: "cancelled", modules: [], failures: [] },
    },
  };
}

export function isScanJobRunning(state: ScanJobState): boolean {
  return state.phase === "scanning";
}

export function scanCompletionNotice(
  moduleCount: number,
  failureCount: number,
): ScanCompletionNotice {
  return {
    message: `VST 清單已更新：${moduleCount} 個模組${failureCount ? `，${failureCount} 個無法載入` : ""}`,
    autoDismissMs: 3000,
    dismissible: false,
  };
}
