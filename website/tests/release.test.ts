import { test } from 'node:test';
import assert from 'node:assert/strict';
import { latestFromApi } from '../src/release.ts';

test('latest release resolves assets by suffix and rejects incomplete payloads', () => {
  const release = latestFromApi({
    tag_name: 'v0.1.0',
    assets: [
      { name: 'SHA256SUMS.txt', state: 'uploaded', browser_download_url: 'https://example.com/SHA256SUMS.txt' },
      { name: 'RoudaMix-0.1.0-windows-x64-portable.zip', state: 'uploaded', browser_download_url: 'https://example.com/portable.zip' },
      { name: 'RoudaMix-0.1.0-windows-x64-setup.exe', state: 'uploaded', browser_download_url: 'https://example.com/setup.exe' },
    ],
  });
  assert.deepEqual(release, { version: '0.1.0', setupUrl: 'https://example.com/setup.exe', portableUrl: 'https://example.com/portable.zip' });
  assert.equal(latestFromApi({ tag_name: 'v0.1.0', assets: [{ name: 'RoudaMix-0.1.0-windows-x64-setup.exe', state: 'uploaded', browser_download_url: 'https://example.com/setup.exe' }] }), null);
  assert.equal(latestFromApi({ tag_name: 'v0.1.0', assets: [] }), null);
  assert.equal(latestFromApi(null), null);
});
