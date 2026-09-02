// 交易式 latency controls：per-plugin Monitor Bypass 與 per-track Output Latency Policy。

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

export type OutputLatencyPolicy = "fullPdc" | "lowLatency";

export type LatencyPolicyState =
  | { phase: "observed" }
  | {
      phase: "pending";
      confirmed: OutputLatencyPolicy;
      requested: OutputLatencyPolicy;
    }
  | { phase: "settled"; value: OutputLatencyPolicy };

export function initialLatencyPolicy(): LatencyPolicyState {
  return { phase: "observed" };
}

export function latencyPolicyValue(
  state: LatencyPolicyState,
  observed: OutputLatencyPolicy,
): OutputLatencyPolicy {
  if (state.phase === "observed") return observed;
  return state.phase === "pending" ? state.requested : state.value;
}

export function beginLatencyPolicy(
  state: LatencyPolicyState,
  observed: OutputLatencyPolicy,
  requested: OutputLatencyPolicy,
): { state: LatencyPolicyState; requested: OutputLatencyPolicy | null } {
  if (state.phase === "pending") return { state, requested: null };
  const confirmed = latencyPolicyValue(state, observed);
  if (confirmed === requested) return { state, requested: null };
  return {
    requested,
    state: { phase: "pending", confirmed, requested },
  };
}

export function finishLatencyPolicy(
  state: LatencyPolicyState,
  outcome: "confirmed" | "rejected",
): LatencyPolicyState {
  if (state.phase !== "pending") return state;
  return {
    phase: "settled",
    value: outcome === "confirmed" ? state.requested : state.confirmed,
  };
}

export function reconcileLatencyPolicy(
  state: LatencyPolicyState,
  observed: OutputLatencyPolicy,
): LatencyPolicyState {
  if (state.phase === "settled" && state.value === observed) return initialLatencyPolicy();
  return state;
}

export function isLatencyPolicyPending(state: LatencyPolicyState): boolean {
  return state.phase === "pending";
}
