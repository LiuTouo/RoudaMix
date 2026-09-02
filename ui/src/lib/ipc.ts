import { invoke } from "@tauri-apps/api/core";
import { listen } from "@tauri-apps/api/event";
import type { ConnectionStatus, MetersFrame, Snapshot } from "./types";

export { engineCommand } from "./protocol-commands.generated";

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

// ---------- 應用層設定(bridge settings.json;P1-E typed schema) ----------

export type AppSettings = {
  schemaVersion?: number;
  startupMode: "blank" | "last" | "folder";
  sessionDir: string | null;
  /** folder 模式:sessionDir 內選定的檔名(空 = 空白 session) */
  startupFile: string | null;
  /** last 模式:上次存/載的路徑 */
  lastSessionPath: string | null;
  /** P1-D:最後「成功啟動」的裝置/Buffer(只有成功才寫入) */
  lastWorkingDevice?: string | null;
  lastWorkingBuffer?: number | null;
  /** 主視窗關閉按鈕的行為；null/undefined 代表首次關閉時詢問。 */
  closeBehavior?: "tray" | "exit" | null;
  /** 僅由 Windows 登入自動啟動時，讓主視窗保持在系統匣。 */
  startMinimizedOnAutostart: boolean;
};

/** get/set 的回覆:typed settings + 載入時的 normalize 警告(可呈現) */
export interface SettingsReply {
  settings: AppSettings;
  warnings: string[];
}

export const getSettings = () => invoke<SettingsReply>("get_settings");

export const setSettings = (patch: Partial<AppSettings>) =>
  invoke<SettingsReply>("set_settings", { patch });

export const listSessions = (dir: string) => invoke<string[]>("list_sessions", { dir });

export const onTrayExitRequested = (cb: () => void) =>
  listen("tray-exit-requested", cb);

/** 第二個主程序啟動要求已被攔截，既有主視窗會被喚醒。 */
export const onSingleInstance = (cb: () => void) =>
  listen("single-instance-requested", cb);

export const quitApp = () => invoke<void>("quit_app");

/** P1-L:連線失敗/spawn 失敗時手動重試(冪等) */
export const respawnEngine = () => invoke<void>("respawn_engine");
