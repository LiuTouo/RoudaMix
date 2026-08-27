import { invoke } from "@tauri-apps/api/core";
import { listen } from "@tauri-apps/api/event";
import type { ConnectionStatus, MetersFrame, Snapshot } from "./types";

export function engineCommand(
  kind: string,
  payload: Record<string, unknown> = {},
): Promise<Record<string, unknown>> {
  return invoke("engine_command", { kind, payload });
}

export const connectStatus = () => invoke<ConnectionStatus>("connect_status");

export function onConnection(cb: (s: ConnectionStatus) => void) {
  return listen<ConnectionStatus>("connection", (e) => cb(e.payload));
}

export function onSnapshot(cb: (s: Snapshot) => void) {
  return listen<Snapshot>("engine-snapshot", (e) => cb(e.payload));
}

export function onEngineEvent(
  cb: (kind: string, payload: unknown) => void,
) {
  return listen<{ kind: string; payload: unknown }>("engine-event", (e) =>
    cb(e.payload.kind, e.payload.payload),
  );
}

export function onMeters(cb: (m: MetersFrame) => void) {
  return listen<MetersFrame>("meters", (e) => cb(e.payload));
}
