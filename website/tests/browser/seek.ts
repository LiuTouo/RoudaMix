import { expect, type Page } from '@playwright/test';

// Zero-delta wheels engage/refresh the scroll takeover without moving the
// page (Lenis treats deltaY=0 as a tap and ignores it), so the programmatic
// scrollTo stays exact. The takeover lags ~110 ms behind the scroll; each
// wheel refreshes the 450 ms manual window that freezes the demo, and the
// phase is read well inside the final window after the lag has settled.
export async function seek(page: Page, stage: number, local: number) {
  await page.mouse.wheel(0, 0);
  await page.evaluate(({ stage, local }) => window.scrollTo(0, (document.querySelector('#story')!.clientHeight - innerHeight) * (stage + local) / 7), { stage, local });
  await expect(page.locator('.scene')).toHaveAttribute('data-stage', String(stage));
  await page.waitForTimeout(250);
  await page.mouse.wheel(0, 0);
  await page.waitForTimeout(250);
  await page.mouse.wheel(0, 0);
  const phase = Number(await page.locator('.scene-presentation').getAttribute('data-phase'));
  expect(Math.abs(phase - local)).toBeLessThan(0.01);
}
