import { test, expect } from '@playwright/test';

test.beforeEach(async ({ page }) => {
  await page.addInitScript(() => localStorage.setItem('roudamix-demo-mode', 'scrub'));
});

test('English chapters keep their key result visible and support orientation changes', async ({ page }, info) => {
  await page.goto('en/');
  for (let stage = 0; stage < 7; stage++) {
    await page.locator('.chapter-nav a').nth(stage).click();
    await expect(page.locator('.scene')).toHaveAttribute('data-stage', String(stage));
    const key = stage === 1 ? '.settings-card' : stage === 2 ? '.plugin-slot' : stage === 3 || stage === 4 ? '.output-result' : stage > 4 ? '.destination-app' : '.track-label';
    for (const locator of await page.locator(key).all()) {
      const box = await locator.boundingBox();
      const windowBox = await page.locator('.mix-window').boundingBox();
      const viewport = page.viewportSize()!;
      expect(box!.y).toBeGreaterThanOrEqual(54);
      expect(box!.y + box!.height).toBeLessThanOrEqual(viewport.height - 34);
      if (stage < 5) expect(box!.y + box!.height).toBeLessThanOrEqual(windowBox!.y + windowBox!.height);
    }
    await page.screenshot({ path: info.outputPath(`en-${stage}.png`), scale: 'css' });
  }
  await page.locator('.chapter-nav a').nth(4).click();
  await page.setViewportSize({ width: 844, height: 390 });
  await expect(page.locator('.scene')).toHaveAttribute('data-stage', '4');
  await page.waitForTimeout(400); // covers the resize debounce, not animation readiness
  await expect(page.locator('.scene')).toHaveAttribute('data-stage', '4');
});

test('desktop wheel settles without a second scrub delay and cursor reports page progress', async ({ page }, info) => {
  test.skip(info.project.name !== 'desktop');
  await page.goto('./');
  await page.mouse.move(600, 300);
  await expect(page.locator('.cursor')).toBeVisible();
  await page.mouse.wheel(0, 800);
  await page.waitForTimeout(750);
  const settled = await page.evaluate(() => scrollY);
  expect(settled).toBeGreaterThan(770);
  await page.waitForTimeout(150);
  expect(Math.abs(await page.evaluate(() => scrollY) - settled)).toBeLessThan(10);
  const progress = await page.locator('.cursor').getAttribute('style');
  expect(progress).not.toContain('--page-progress:0deg');
});
