import { test, expect, type Page } from '@playwright/test';
import { publication } from '../../src/config';

const fallback = '.download-actions a[href$="/releases/latest"]';

for (const locale of ['zh', 'en'] as const) {
  const path = locale === 'en' ? 'en/' : './';
  test(`${locale}: download stays actionable during loading and after timeout`, async ({ page }) => {
    await page.clock.install();
    await page.route('**/api.github.com/**', () => {});
    await page.goto(path);
    await expect(page.locator(fallback)).toHaveAttribute('href', `${publication.repository}/releases/latest`);
    await expect(page.locator('.release-status')).toContainText(locale === 'en' ? 'Checking' : '正在查詢');
    await expect(page.locator('.download-actions .button')).toHaveCount(0);
    await page.clock.fastForward(8100);
    await expect(page.locator('.release-status')).toContainText(locale === 'en' ? 'unavailable' : '無法取得');
    await expect(page.locator(fallback)).toHaveCount(1);
  });

  test(`${locale}: failed, malformed and incomplete API responses retain Releases`, async ({ page }) => {
    for (const body of [null, '{broken', JSON.stringify({ tag_name: 'v1', assets: {} }), JSON.stringify({ tag_name: 'v1', assets: [] })]) {
      await page.route('**/api.github.com/**', route => route.fulfill({ status: body === null ? 403 : 200, contentType: 'application/json', body: body ?? '{}' }));
      await page.goto(path);
      await expect(page.locator('.release-status')).toContainText(locale === 'en' ? 'unavailable' : '無法取得');
      await expect(page.locator(fallback)).toHaveAttribute('href', `${publication.repository}/releases/latest`);
      await expect(page.locator('.download-actions .button')).toHaveCount(0);
      await page.unroute('**/api.github.com/**');
    }
  });

  test(`${locale}: mobile hero quick start and playback are visible touch targets`, async ({ page }, info) => {
    test.skip(info.project.name !== 'mobile');
    for (const viewport of [{ width: 320, height: 740 }, { width: 390, height: 844 }, { width: 844, height: 390 }]) {
      await page.setViewportSize(viewport);
      await page.goto(path);
      await page.locator('.story-copy').evaluate(async element => { await Promise.allSettled(element.getAnimations({ subtree: true }).map(animation => animation.finished)); });
      const guide = page.locator('.hero-guide');
      await expect(guide).toHaveText(locale === 'en' ? 'Quick start' : '跳至教學');
      for (const control of [guide, page.locator('.playback-toggle')]) {
        await expect(control).toBeVisible();
        const box = (await control.boundingBox())!;
        expect(box.height).toBeGreaterThanOrEqual(44);
        expect(box.width).toBeGreaterThanOrEqual(44);
        expect(box.x).toBeGreaterThanOrEqual(0);
        expect(box.x + box.width).toBeLessThanOrEqual(viewport.width);
        expect(box.y).toBeGreaterThanOrEqual(54);
        expect(box.y + box.height).toBeLessThanOrEqual(viewport.height - 34);
      }
      expect(await page.evaluate(() => document.documentElement.scrollWidth <= innerWidth)).toBe(true);
      await page.screenshot({ path: info.outputPath(`hero-${locale}-${viewport.width}.png`), scale: 'css' });
      const playback = page.locator('.playback-toggle');
      const box = (await playback.boundingBox())!;
      await page.touchscreen.tap(box.x + box.width / 2, box.y + box.height / 2);
      await expect(playback).toHaveText(locale === 'en' ? 'Continue' : '繼續');
      await expect(playback).toHaveAttribute('aria-pressed', 'true');
      await page.touchscreen.tap(box.x + box.width / 2, box.y + box.height / 2);
      await expect(playback).toHaveText(locale === 'en' ? 'Pause' : '暫停');
      await expect(playback).toHaveAttribute('aria-pressed', 'false');
      await guide.click();
      await expect(page.locator('#guide')).toBeFocused();
    }
  });
}

async function clickPlayback(page: Page) {
  const box = (await page.locator('.playback-toggle').boundingBox())!;
  expect(await page.evaluate(({ x, y }) => !!document.elementFromPoint(x, y)?.closest('.playback-toggle'), { x: box.x + box.width / 2, y: box.y + box.height / 2 }), JSON.stringify(box)).toBe(true);
  await page.mouse.click(box.x + box.width / 2, box.y + box.height / 2);
}

test('Pause freezes operation motion and auto advance; Continue resumes without a focus race', async ({ page }) => {
  await page.goto('en/');
  const demo = page.locator('.scene-presentation');
  await expect.poll(async () => Number(await demo.getAttribute('data-phase'))).toBeGreaterThan(.15);
  await clickPlayback(page);
  await expect(page.locator('.playback-toggle')).toHaveAttribute('aria-pressed', 'true');
  await expect(page.locator('.playback-toggle')).toHaveText('Continue');
  const phase = await demo.getAttribute('data-phase');
  const style = await page.locator('.scene').getAttribute('style');
  await expect(page.locator('.scene')).not.toHaveClass(/is-active/);
  expect(await page.locator('.signal-packet').first().evaluate(element => getComputedStyle(element).animationPlayState)).toBe('paused');
  await page.clock.install();
  await page.clock.runFor(10000);
  expect(await demo.getAttribute('data-phase')).toBe(phase);
  expect(await page.locator('.scene').getAttribute('style')).toBe(style);
  expect(await page.evaluate(() => scrollY)).toBe(0);
  await clickPlayback(page);
  await expect(page.locator('.playback-toggle')).toHaveAttribute('aria-pressed', 'false');
  await page.clock.runFor(500);
  await expect.poll(async () => Number(await demo.getAttribute('data-phase'))).toBeGreaterThan(Number(phase));
  await expect(demo).toHaveAttribute('data-auto-advance', 'true');
  await page.keyboard.press('Space');
  await expect(demo).toHaveAttribute('data-paused', 'true');
  await page.keyboard.press('Space');
  await expect(demo).toHaveAttribute('data-paused', 'false');
  await page.keyboard.press('ArrowDown');
  await expect(demo).toHaveAttribute('data-paused', 'true');
});

test('keyboard scrolling and real audio-control focus take over until explicit Continue', async ({ page }) => {
  await page.goto('en/');
  const demo = page.locator('.scene-presentation');
  await page.keyboard.press('Tab');
  await expect(demo).toHaveAttribute('data-auto-advance', 'false');
  await page.keyboard.press('PageDown');
  await expect(demo).toHaveAttribute('data-paused', 'true');
  await page.evaluate(async () => {
    const path = '/RoudaMix/src/config.ts';
    const { publication } = await import(/* @vite-ignore */ path);
    publication.audio = { monitor: '/test-monitor.wav', stream: '/test-stream.wav' };
  });
  await page.locator('.chapter-nav a').nth(4).click();
  await expect(page.locator('#story-heading')).toBeFocused();
  await clickPlayback(page);
  await expect(demo).toHaveAttribute('data-paused', 'false');
  await page.getByRole('button', { name: 'Play sample', exact: true }).focus();
  await expect(demo).toHaveAttribute('data-paused', 'true');
  await expect(demo).toHaveAttribute('data-auto-advance', 'false');
  const phase = await demo.getAttribute('data-phase');
  const scroll = await page.evaluate(() => scrollY);
  await page.clock.install();
  await page.clock.runFor(10000);
  expect(await demo.getAttribute('data-phase')).toBe(phase);
  expect(await page.evaluate(() => scrollY)).toBe(scroll);
  await expect(page.getByRole('button', { name: 'Play sample', exact: true })).toBeFocused();
  await clickPlayback(page);
  await expect(demo).toHaveAttribute('data-auto-advance', 'true');
});
