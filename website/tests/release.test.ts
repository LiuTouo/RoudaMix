import { test } from 'node:test';
import assert from 'node:assert/strict';
import { latestFromApi, fetchLatestRelease } from '../src/release.ts';

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

test('malformed release payloads do not throw or advertise unsafe links', () => {
  for (const data of [undefined, [], 'release', { tag_name: '' }, { tag_name: 'v' }, { tag_name: 'v1', assets: {} }, { tag_name: 'v1', assets: [null, 3, {}] }]) {
    assert.equal(latestFromApi(data), null);
  }
  assert.equal(latestFromApi({ tag_name: 'v1', assets: [
    { name: 'a-windows-x64-setup.exe', state: 'uploaded', browser_download_url: 'javascript:alert(1)' },
    { name: 'a-windows-x64-portable.zip', state: 'uploaded', browser_download_url: 'https://example.com/portable.zip' },
  ] }), null);
});

test('release request passes cancellation through and recovers from HTTP/network/JSON errors', async (t) => {
  const controller = new AbortController();
  const fetchMock = t.mock.method(globalThis, 'fetch', async (_url: string, options: RequestInit) => {
    assert.equal(options.signal, controller.signal);
    return new Response('', { status: 403 });
  });
  assert.equal(await fetchLatestRelease(controller.signal), null);
  fetchMock.mock.mockImplementation(async () => { throw new TypeError('offline'); });
  assert.equal(await fetchLatestRelease(), null);
  fetchMock.mock.mockImplementation(async () => new Response('not JSON'));
  assert.equal(await fetchLatestRelease(), null);
  controller.abort();
  fetchMock.mock.mockImplementation(async () => { throw controller.signal.reason; });
  assert.equal(await fetchLatestRelease(controller.signal), null);
});
