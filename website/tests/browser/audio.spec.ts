import { test, expect, type Page } from '@playwright/test';

// Generated test-only PCM; never copied to public/ or advertised as a demo.
function sample(seconds = 3) {
  const rate = 8000;
  const count = seconds * rate;
  const buffer = Buffer.alloc(44 + count * 2);
  buffer.write('RIFF', 0); buffer.writeUInt32LE(36 + count * 2, 4); buffer.write('WAVEfmt ', 8);
  buffer.writeUInt32LE(16, 16); buffer.writeUInt16LE(1, 20); buffer.writeUInt16LE(1, 22);
  buffer.writeUInt32LE(rate, 24); buffer.writeUInt32LE(rate * 2, 28); buffer.writeUInt16LE(2, 32); buffer.writeUInt16LE(16, 34);
  buffer.write('data', 36); buffer.writeUInt32LE(count * 2, 40);
  for (let i = 0; i < count; i++) buffer.writeInt16LE(Math.round(Math.sin(i / rate * 440 * Math.PI * 2) * 200), 44 + i * 2);
  return buffer;
}

async function prepare(page: Page, failure = false) {
  await page.route('**/test-audio/*.wav', route => route.fulfill({ status: failure ? 404 : 200, contentType: 'audio/wav', body: failure ? '' : sample() }));
  await page.goto('en/');
  await page.evaluate(async () => {
    const path = '/RoudaMix/src/config.ts';
    const { publication } = await import(/* @vite-ignore */ path);
    publication.audio = { monitor: '/RoudaMix/test-audio/monitor.wav', stream: '/RoudaMix/test-audio/stream.wav' };
    const originalStart = AudioBufferSourceNode.prototype.start;
    const originalStop = AudioBufferSourceNode.prototype.stop;
    const events = { starts: [] as number[][], stops: 0 };
    Object.assign(window, { audioEvents: events });
    AudioBufferSourceNode.prototype.start = function(when = 0, offset = 0) { events.starts.push([when, offset]); originalStart.call(this, when, offset); };
    AudioBufferSourceNode.prototype.stop = function() { events.stops++; originalStop.call(this); };
  });
  await page.locator('.chapter-nav a').nth(4).click();
  await expect(page.getByRole('button', { name: 'Play sample', exact: true })).toBeVisible();
}

test('real Web Audio clock stays aligned through switch, pause, resume and chapter exit', async ({ page }) => {
  await prepare(page);
  const events = () => page.evaluate(() => (window as unknown as { audioEvents: { starts: number[][]; stops: number } }).audioEvents);
  expect((await events()).starts).toEqual([]);
  await page.getByRole('button', { name: 'Play sample', exact: true }).click();
  await expect(page.getByRole('button', { name: 'Pause', exact: true })).toBeVisible();
  await expect.poll(async () => Number(await page.locator('.audio-demo progress').getAttribute('value'))).toBeGreaterThan(0.15);
  const starts = (await events()).starts;
  expect(starts).toHaveLength(2);
  expect(starts[0]).toEqual(starts[1]);
  await page.getByRole('button', { name: 'Stream', exact: true }).click();
  await expect(page.getByRole('button', { name: 'Stream', exact: true })).toHaveAttribute('aria-pressed', 'true');
  expect((await events()).starts).toHaveLength(2);
  await page.getByRole('button', { name: 'Pause', exact: true }).click();
  expect((await events()).stops).toBe(2);
  await page.getByRole('button', { name: 'Play sample', exact: true }).click();
  await expect(page.getByRole('button', { name: 'Pause', exact: true })).toBeVisible();
  const resumed = (await events()).starts;
  expect(resumed[2]).toEqual(resumed[3]);
  expect(resumed[2][1]).toBeGreaterThan(0.15);
  await page.locator('.chapter-nav a').nth(5).click();
  await expect.poll(async () => (await events()).stops).toBe(4);
});

test('failed audio leaves a readable scene and retry control', async ({ page }) => {
  await prepare(page, true);
  await page.getByRole('button', { name: 'Play sample', exact: true }).click();
  await expect(page.getByRole('status')).toContainText('could not load');
  await expect(page.locator('.scene-heading')).toBeVisible();
  await expect(page.getByRole('button', { name: 'Play sample', exact: true })).toBeVisible();
});

test('leaving during download cancels playback; language navigation closes audio', async ({ page }) => {
  await prepare(page);
  await page.route('**/test-audio/*.wav', async route => {
    await new Promise(resolve => setTimeout(resolve, 350));
    await route.fulfill({ contentType: 'audio/wav', body: sample() }).catch(() => {});
  });
  await page.getByRole('button', { name: 'Play sample', exact: true }).click();
  await page.locator('.chapter-nav a').nth(5).click();
  await page.waitForTimeout(450);
  expect(await page.evaluate(() => (window as unknown as { audioEvents: { starts: number[][] } }).audioEvents.starts.length)).toBe(0);
  await page.locator('.chapter-nav a').nth(4).click();
  await page.getByRole('button', { name: 'Play sample', exact: true }).click();
  await expect(page.getByRole('button', { name: 'Pause', exact: true })).toBeVisible();
  await page.locator('.language').click();
  await expect(page.locator('html')).toHaveAttribute('lang', 'zh-Hant');
  await expect(page.locator('.audio-pending')).toBeVisible();
});
