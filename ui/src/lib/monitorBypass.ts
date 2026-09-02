export interface MonitorBypassTransaction {
  confirmed: boolean;
  requested: boolean;
}

export type MonitorBypassTransactions = ReadonlyMap<number, MonitorBypassTransaction>;

export interface MonitorBypassCommand {
  instanceId: number;
  bypassed: boolean;
}

export function beginMonitorBypass(
  transactions: MonitorBypassTransactions,
  instanceId: number,
  confirmed: boolean,
): { transactions: MonitorBypassTransactions; command: MonitorBypassCommand | null } {
  if (transactions.has(instanceId)) return { transactions, command: null };

  const requested = !confirmed;
  const next = new Map(transactions);
  next.set(instanceId, { confirmed, requested });
  return {
    transactions: next,
    command: { instanceId, bypassed: requested },
  };
}

export function finishMonitorBypass(
  transactions: MonitorBypassTransactions,
  instanceId: number,
  outcome: "confirmed" | "rejected",
): { transactions: MonitorBypassTransactions; value: boolean | null } {
  const transaction = transactions.get(instanceId);
  if (!transaction) return { transactions, value: null };

  const next = new Map(transactions);
  next.delete(instanceId);
  return {
    transactions: next,
    value: outcome === "confirmed" ? transaction.requested : transaction.confirmed,
  };
}

export function isMonitorBypassPending(
  transactions: MonitorBypassTransactions,
  instanceId: number,
): boolean {
  return transactions.has(instanceId);
}
