import { test, expect } from '@playwright/test';

test('opening workspace visibly operates without any scrolling; pause and replay work', async ({ page }, info) => {
  await page.goto('en/');
  const demo = page.locator('.scene-presentation');
  await expect(demo).toHaveAttribute('data-playback', 'auto');
  const original = await page.locator('.scene').getAttribute('style');
  await expect.poll(async () => Number(await demo.getAttribute('data-phase'))).toBeGreaterThan(.28);
  expect(await page.locator('.scene').getAttribute('style')).not.toBe(original);
  expect(await page.evaluate(() => scrollY)).toBe(0);
  await expect(page.locator('.waveform')).toHaveClass(/receiving/, { timeout: 6000 });
  await page.getByRole('button', { name: 'Pause animation' }).click();
  const paused = await demo.getAttribute('data-phase');
  await page.waitForTimeout(250);
  expect(await demo.getAttribute('data-phase')).toBe(paused);
  expect(await page.locator('.waveform i').first().evaluate(node => getComputedStyle(node).animationPlayState)).toBe('paused');
  await page.getByRole('button', { name: 'Replay', exact: true }).click();
  expect(Number(await demo.getAttribute('data-phase'))).toBeLessThan(.15);
  await expect(page.locator('.output.stream')).toContainText('CABLE Input', { timeout: 8500 });
  await page.screenshot({ path: info.outputPath('autoplay-result.png'), scale: 'css' });
});

test('chapter links scroll smoothly, animate the scene, and start that chapter demo', async ({ page }) => {
  await page.goto('en/');
  await page.evaluate(() => {
    document.querySelectorAll('.chapter-nav a')[2].addEventListener('click', () => {
      setTimeout(() => Object.assign(window, { sampledNavigationPosition: scrollY }), 120);
    }, { once: true });
  });
  await page.locator('.chapter-nav a').nth(2).click();
  await expect.poll(() => page.evaluate(() => (window as unknown as { sampledNavigationPosition?: number }).sampledNavigationPosition)).toBeDefined();
  const intermediate = await page.evaluate(() => (window as unknown as { sampledNavigationPosition: number }).sampledNavigationPosition);
  const target = await page.evaluate(() => (document.querySelector('#story')!.clientHeight - innerHeight) * 2.78 / 7);
  expect(intermediate).toBeGreaterThan(0);
  expect(intermediate).toBeLessThan(target - 20);
  await expect(page.locator('.scene')).toHaveAttribute('data-stage', '2');
  await expect.poll(async () => page.evaluate(() => Math.abs(scrollY - (document.querySelector('#story')!.clientHeight - innerHeight) * 2.78 / 7))).toBeLessThan(3);
  await expect(page.locator('.plugin-picker-illustration')).toBeVisible();
  await expect(page.locator('.plugin-slot')).toHaveClass(/loaded/, { timeout: 6500 });
  await page.locator('.chapter-nav a').nth(1).click();
  await expect(page.locator('.scene')).toHaveAttribute('data-stage', '1');
  await expect(page.locator('.device-menu')).toBeVisible();
});
