import { publication } from './config.ts';

export interface LatestRelease { version: string; setupUrl: string; portableUrl: string }

// Release workflow uploads RoudaMix-<version>-windows-x64-{setup.exe,portable.zip}; keep suffixes in sync with .github/workflows/release.yml.
export function latestFromApi(data: unknown): LatestRelease | null {
  const release = data as { tag_name?: unknown; assets?: Array<{ name?: unknown; state?: unknown; browser_download_url?: unknown }> } | null;
  if (!release || typeof release.tag_name !== 'string') return null;
  const pick = (suffix: string) => {
    const asset = (release.assets ?? []).find(asset => asset.state === 'uploaded' && typeof asset.name === 'string'
      && typeof asset.browser_download_url === 'string' && asset.name.endsWith(suffix));
    return asset ? asset.browser_download_url as string : null;
  };
  const setupUrl = pick('-windows-x64-setup.exe');
  const portableUrl = pick('-windows-x64-portable.zip');
  if (!setupUrl || !portableUrl) return null;
  return { version: release.tag_name.replace(/^v/, ''), setupUrl, portableUrl };
}

export async function fetchLatestRelease(signal?: AbortSignal): Promise<LatestRelease | null> {
  const repo = publication.repository.replace(/^https:\/\/github\.com\//, '');
  try {
    // GitHub "latest" already excludes drafts and prereleases (rc builds).
    const response = await fetch(`https://api.github.com/repos/${repo}/releases/latest`, { headers: { Accept: 'application/vnd.github+json' }, signal });
    if (!response.ok) return null;
    return latestFromApi(await response.json());
  } catch { return null; }
}
