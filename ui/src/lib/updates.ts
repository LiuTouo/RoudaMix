export const RELEASE_API = "https://api.github.com/repos/LiuTouo/RoudaMix/releases/latest";

export interface ReleaseUpdate {
  version: string;
  available: boolean;
}

function parseVersion(value: string): { numbers: number[]; prerelease: boolean } {
  const match = /^(0|[1-9]\d*)\.(0|[1-9]\d*)\.(0|[1-9]\d*)(?:-(?:alpha|beta|rc)\.(?:0|[1-9]\d*))?$/.exec(value);
  if (!match) throw new Error("版本格式無法辨識");
  const numbers = match.slice(1, 4).map(Number);
  if (numbers.some((part) => !Number.isSafeInteger(part))) throw new Error("版本格式無法辨識");
  return { numbers, prerelease: value.includes("-") };
}

export function releaseUpdate(raw: unknown, current: string): ReleaseUpdate {
  if (!raw || typeof raw !== "object") throw new Error("更新資訊格式不正確");
  const release = raw as Record<string, unknown>;
  if (release.draft !== false || release.prerelease !== false || typeof release.tag_name !== "string")
    throw new Error("尚無可用的正式版本");
  const version = release.tag_name.replace(/^v/, "");
  const latest = parseVersion(version);
  const installed = parseVersion(current);
  if (latest.prerelease) throw new Error("尚無可用的正式版本");
  let comparison = 0;
  for (let i = 0; i < 3; i++) {
    comparison = Math.sign(latest.numbers[i] - installed.numbers[i]);
    if (comparison !== 0) break;
  }
  return { version, available: comparison > 0 || (comparison === 0 && installed.prerelease) };
}

export async function checkRelease(current: string): Promise<ReleaseUpdate> {
  const response = await fetch(RELEASE_API, {
    headers: { Accept: "application/vnd.github+json" },
    signal: AbortSignal.timeout(15_000),
    cache: "no-store",
  });
  if (response.status === 403 || response.status === 429)
    throw new Error("更新服務暫時限制請求，請稍後再試");
  if (!response.ok) throw new Error(`更新服務暫時無法使用（${response.status}）`);
  return releaseUpdate(await response.json(), current);
}
