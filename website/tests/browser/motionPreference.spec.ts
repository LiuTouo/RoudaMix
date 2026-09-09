import { test, expect } from '@playwright/test';

test('the actual reduced-motion environment has a working explicit opt-in, including CSS animation', async ({ page }) => {
  await page.emulateMedia({ reducedMotion: 'reduce' });
  await page.goto('en/');
  await expect(page.locator('.static-chapter')).toHaveCount(7);
  await expect(page.locator('.motion-notice')).toBeVisible();
  await page.locator('.site-header .motion-toggle').click();
  await expect(page).toHaveURL(/motion=full/);
  await expect(page.locator('html')).toHaveAttribute('data-motion', 'full');
  await expect(page.locator('.static-chapter')).toHaveCount(0);
  await expect(page.locator('.playback-toolbar')).toBeVisible();
  await expect.poll(async () => Number(await page.locator('.scene-presentation').getAttribute('data-phase'))).toBeGreaterThan(.25);
  expect(await page.evaluate(() => matchMedia('(prefers-reduced-motion: reduce)').matches)).toBe(true);
  expect(await page.locator('.source-packet').first().evaluate(node => getComputedStyle(node).animationName)).toBe('signal-travel');
  const first = await page.locator('.source-packet').first().evaluate(node => { const box = node.getBoundingClientRect(); return { x: box.x, y: box.y }; });
  await page.waitForTimeout(180);
  expect(await page.locator('.source-packet').first().evaluate(node => { const box = node.getBoundingClientRect(); return { x: box.x, y: box.y }; })).not.toEqual(first);
  await page.locator('.language').click();
  await expect(page.locator('html')).toHaveAttribute('lang', 'zh-Hant');
  await expect(page.locator('html')).toHaveAttribute('data-motion', 'full');
  await page.reload();
  await expect(page.locator('.playback-toolbar')).toBeVisible();
  await page.locator('footer .motion-toggle').click();
  await expect(page.locator('html')).toHaveAttribute('data-motion', 'reduced');
  await expect(page.locator('.static-chapter')).toHaveCount(7);
});

test('shared full-motion link enables animation even when OS animations are disabled', async ({ page }) => {
  await page.emulateMedia({ reducedMotion: 'reduce' });
  await page.goto('./?motion=full');
  await expect(page.locator('.scene-presentation')).toHaveAttribute('data-playback', 'auto');
  await expect(page.locator('.playback-toolbar')).toBeVisible();
  await expect.poll(async () => Number(await page.locator('.scene-presentation').getAttribute('data-phase'))).toBeGreaterThan(.15);
});
