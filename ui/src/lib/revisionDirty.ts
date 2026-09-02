export type DirtyChoice = "save" | "discard" | "cancel";

export interface RevisionDirtyState {
  revision: number | null;
  cleanRevision: number | null;
}

export type RevisionDirtyEvent =
  | { type: "engineRevisionObserved"; revision: number }
  | { type: "baselineConfirmed"; revision: number }
  | { type: "baselineRejected" }
  | { type: "runtimeObserved" }
  | { type: "reset" };

export function initialRevisionDirty(): RevisionDirtyState {
  return { revision: null, cleanRevision: null };
}

/**
 * Engine revision 只因使用者 mutation 推進；latency/load/suspension 等 runtime
 * observation 不帶新的 revision。save/load/default setup 成功則同時建立 baseline。
 */
export function transitionRevisionDirty(
  state: RevisionDirtyState,
  event: RevisionDirtyEvent,
): RevisionDirtyState {
  if (event.type === "reset") return initialRevisionDirty();
  if (event.type === "runtimeObserved" || event.type === "baselineRejected") return state;
  if (event.type === "baselineConfirmed")
    return { revision: event.revision, cleanRevision: event.revision };
  if (state.revision === event.revision) return state;
  return { ...state, revision: event.revision };
}

export function isRevisionDirty(state: RevisionDirtyState): boolean {
  if (state.revision === null || state.cleanRevision === null) return false;
  return state.revision !== state.cleanRevision;
}

/**
 * dirty 時 cancel 阻止、discard 直接通過、save 由呼叫端先存；clean 時直接通過。
 */
export function resolveDirtyChoice(
  dirty: boolean,
  choice: DirtyChoice,
): { proceed: boolean; shouldSave: boolean } {
  if (!dirty || choice === "discard") return { proceed: true, shouldSave: false };
  if (choice === "cancel") return { proceed: false, shouldSave: false };
  return { proceed: false, shouldSave: true };
}
