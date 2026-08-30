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

// ---------- 應用層設定(bridge settings.json) ----------

export type AppSettings = {
  startupMode: "blank" | "last" | "folder";
  sessionDir: string | null;
  /** folder 模式:sessionDir 內選定的檔名(空 = 空白 session) */
  startupFile: string | null;
  /** last 模式:上次存/載的路徑 */
  lastSessionPath: string | null;
};

export const getSettings = () => invoke<AppSettings>("get_settings");

export const setSettings = (patch: Partial<AppSettings>) =>
  invoke<AppSettings>("set_settings", { patch });

export const listSessions = (dir: string) => invoke<string[]>("list_sessions", { dir });
