interface PendingMonitorBypass {
  phase: "pending";
  confirmed: boolean;
  requested: boolean;
}

interface SettledMonitorBypass {
  phase: "settled";
  value: boolean;
}

type MonitorBypassEntry = PendingMonitorBypass | SettledMonitorBypass;

export type MonitorBypassState = ReadonlyMap<number, MonitorBypassEntry>;

export interface MonitorBypassCommand {
  instanceId: number;
  bypassed: boolean;
}

export function initialMonitorBypass(): MonitorBypassState {
  return new Map();
}

export function monitorBypassValue(
  state: MonitorBypassState,
  instanceId: number,
  observed: boolean,
): boolean {
  const entry = state.get(instanceId);
  if (!entry) return observed;
  return entry.phase === "pending" ? entry.confirmed : entry.value;
}

export function beginMonitorBypass(
  state: MonitorBypassState,
  instanceId: number,
  observed: boolean,
): { state: MonitorBypassState; command: MonitorBypassCommand | null } {
  const current = state.get(instanceId);
  if (current?.phase === "pending") return { state, command: null };

  const confirmed = current?.value ?? observed;
  const requested = !confirmed;
  const next = new Map(state);
  next.set(instanceId, { phase: "pending", confirmed, requested });
  return {
    state: next,
    command: { instanceId, bypassed: requested },
  };
}

export function finishMonitorBypass(
  state: MonitorBypassState,
  instanceId: number,
  outcome: "confirmed" | "rejected",
): MonitorBypassState {
  const transaction = state.get(instanceId);
  if (transaction?.phase !== "pending") return state;

  const next = new Map(state);
  next.set(instanceId, {
    phase: "settled",
    value: outcome === "confirmed" ? transaction.requested : transaction.confirmed,
  });
  return next;
}

/** Engine status 對齊 local settle 後移除 override，後續顯示回到外部權威值。 */
export function reconcileMonitorBypass(
  state: MonitorBypassState,
  observations: Iterable<readonly [number, boolean]>,
): MonitorBypassState {
  const observed = new Map(observations);
  let next: Map<number, MonitorBypassEntry> | null = null;
  for (const [instanceId, entry] of state) {
    if (
      !observed.has(instanceId) ||
      (entry.phase === "settled" && observed.get(instanceId) === entry.value)
    ) {
      next ??= new Map(state);
      next.delete(instanceId);
    }
  }
  return next ?? state;
}

export function isMonitorBypassPending(
  state: MonitorBypassState,
  instanceId: number,
): boolean {
  return state.get(instanceId)?.phase === "pending";
}
