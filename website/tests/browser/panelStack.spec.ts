import { test, expect } from '@playwright/test';

test('opaque full-width panels rise over the previous panel and leave long content readable', async ({ page }, info) => {
  await page.goto('en/');
  for (const id of ['guide', 'download', 'faq']) {
    const top = await page.locator(`#${id}-start`).evaluate(el => el.getBoundingClientRect().top + scrollY);
    await page.evaluate(top => scrollTo(0, top - innerHeight / 2), top);
    await page.waitForTimeout(120);
    const panel = page.locator(`#${id}`);
    const box = (await panel.boundingBox())!;
    expect(box.x).toBe(0);
    expect(box.width).toBe(page.viewportSize()!.width);
    expect(Math.abs(box.y - page.viewportSize()!.height / 2)).toBeLessThan(3);
    expect(await panel.evaluate(el => getComputedStyle(el).opacity)).toBe('1');
    expect(await panel.evaluate(el => getComputedStyle(el).backgroundColor)).not.toBe('rgba(0, 0, 0, 0)');
    for (const x of [2, page.viewportSize()!.width / 2, page.viewportSize()!.width - 2]) {
      expect(await page.evaluate(({ id, x }) => document.elementFromPoint(x, innerHeight * .8)?.closest('.cover-panel')?.id === id, { id, x })).toBe(true);
    }
    await page.screenshot({ path: info.outputPath(`cover-${id}.png`), scale: 'css' });
    const height = await panel.evaluate(el => el.clientHeight);
    await page.evaluate(({ top, height }) => scrollTo(0, top + Math.max(0, height - innerHeight)), { top, height });
    await page.waitForTimeout(80);
    const bottom = (await panel.boundingBox())!.y + (await panel.boundingBox())!.height;
    expect(Math.abs(bottom - page.viewportSize()!.height)).toBeLessThan(4);
  }
  await page.locator('.skip-link').focus();
  await page.locator('.skip-link').press('Enter');
  await expect(page.locator('#guide')).toBeFocused();
  expect((await page.locator('#guide').boundingBox())!.y).toBeGreaterThanOrEqual(0);
});

test('a wheel chapter change has visible vertical travel and finishes after the wheel stops', async ({ page }) => {
  await page.goto('en/');
  await page.evaluate(() => scrollTo(0, (document.querySelector('#story')!.clientHeight - innerHeight) * .98 / 7));
  await page.waitForTimeout(1050);
  await page.mouse.wheel(0, 180);
  await expect(page.locator('.scene')).toHaveAttribute('data-stage', '1');
  const early = await page.locator('.scene-presentation').evaluate(el => new DOMMatrix(getComputedStyle(el).transform).m42);
  expect(early).toBeGreaterThan(5);
  await page.waitForTimeout(1100);
  const settled = await page.locator('.scene-presentation').evaluate(el => new DOMMatrix(getComputedStyle(el).transform).m42);
  expect(Math.abs(settled)).toBeLessThan(.5);
});
