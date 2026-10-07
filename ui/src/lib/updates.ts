import { check as checkPluginUpdate } from "@tauri-apps/plugin-updater";

export interface ReleaseUpdate {
  version: string;
  available: boolean;
}

/**
 * 走 updater 外掛(Rust 端 HTTP)讀 releases/latest/download/latest.json 檢查版本。
 * 不走 api.github.com:未認證 API 每 IP 每小時僅 60 次,共用出口 IP(如 Cloudflare WARP)
 * 會長期處於 403 限流;release 資產端點無此限制,也不受 webview CORS 影響。
 * 外掛已內建版本比較(回傳 null 代表已是最新)。
 */
export async function checkRelease(current: string): Promise<ReleaseUpdate> {
  const latest = await checkPluginUpdate();
  return latest ? { version: latest.version, available: true } : { version: current, available: false };
}
