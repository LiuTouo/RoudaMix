import { test, expect, type Page } from '@playwright/test';

async function seek(page: Page, stage: number, local: number) {
  await page.evaluate(({ stage, local }) => window.scrollTo(0, (document.querySelector('#story')!.clientHeight - innerHeight) * (stage + local) / 7), { stage, local });
  await expect(page.locator('.scene')).toHaveAttribute('data-stage', String(stage));
  await page.waitForTimeout(60);
}

test('each operation demonstrates approach, selection and its signal result', async ({ page }, info) => {
  await page.goto('en/');
  for (const [stage, local, locator] of [[1, .25, '.device-menu'], [2, .3, '.plugin-picker-illustration'], [3, .2, '.monitor-path'], [3, .5, '.routing .stream-path'], [4, .37, '.bypass-path'], [5, .42, '.app-device-menu'], [6, .42, '.destination-app']] as const) {
    await seek(page, stage, local);
    await expect(page.locator(locator)).toBeVisible();
    await expect(page.locator('.demo-pointer')).toHaveCount(1);
    await page.screenshot({ path: info.outputPath(`operation-${stage}-${local}.png`), scale: 'css' });
  }
  await seek(page, 4, .1);
  const before = await page.locator('.bypass-path').evaluate(el => getComputedStyle(el).strokeDashoffset);
  await seek(page, 4, .37);
  const during = await page.locator('.bypass-path').evaluate(el => getComputedStyle(el).strokeDashoffset);
  await seek(page, 4, .78);
  const after = await page.locator('.bypass-path').evaluate(el => getComputedStyle(el).strokeDashoffset);
  expect(parseFloat(before)).toBe(1);
  expect(parseFloat(during)).toBeGreaterThan(0);
  expect(parseFloat(during)).toBeLessThan(1);
  expect(parseFloat(after)).toBe(0);
  await expect(page.locator('.output.monitor')).toContainText('Reverb skipped');
  await expect(page.locator('.output.stream')).toContainText('Reverb included');
  await seek(page, 4, .1);
  expect(await page.locator('.bypass-path').evaluate(el => getComputedStyle(el).strokeDashoffset)).toBe(before);
});

test('signal particles and meters move at rest, while leaving the scene pauses them', async ({ page }) => {
  await page.goto('en/#obs');
  const particle = page.locator('.stream-packet').first();
  await expect(page.locator('.scene')).toHaveClass(/is-active/);
  const first = await particle.evaluate(el => getComputedStyle(el).offsetDistance);
  await page.waitForTimeout(180);
  const second = await particle.evaluate(el => getComputedStyle(el).offsetDistance);
  expect(first).not.toBe(second);
  expect(await page.locator('.output.stream .meter i').evaluate(el => getComputedStyle(el).animationPlayState)).toBe('running');
  await page.locator('.nav-download').click();
  await expect(page.locator('.scene')).not.toHaveClass(/is-active/);
  expect(await particle.evaluate(el => getComputedStyle(el).animationPlayState)).toBe('paused');
});
