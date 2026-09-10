import { test, expect } from '@playwright/test';

test('both language routes work on direct open and refresh; no fake controls', async ({ page }) => {
  const errors: string[] = [];
  page.on('pageerror', error => errors.push(error.message));
  await page.route('**/api.github.com/**', route => route.abort());
  for (const route of ['', 'en/']) {
    await page.goto(route || './');
    await expect(page.locator('h1')).toBeVisible();
    await page.reload();
    await expect(page.locator('html')).toHaveAttribute('lang', route ? 'en' : 'zh-Hant');
    await expect(page.locator('.release-pending')).toBeAttached();
    await expect(page.locator('a[download], [title], audio[autoplay]')).toHaveCount(0);
    await expect(page.locator('.download-actions a')).toHaveCount(1);
    expect(await page.evaluate(() => document.documentElement.scrollWidth <= innerWidth)).toBe(true);
  }
  expect(errors).toEqual([]);
});

test('download section resolves the latest GitHub release with a portable link', async ({ page }) => {
  await page.route('**/api.github.com/repos/LiuTouo/RoudaMix/releases/latest', async (route) => {
    await route.fulfill({ status: 200, contentType: 'application/json', body: JSON.stringify({
      tag_name: 'v0.9.9',
      assets: [
        { name: 'RoudaMix-0.9.9-windows-x64-portable.zip', state: 'uploaded', browser_download_url: 'https://example.com/portable.zip' },
        { name: 'RoudaMix-0.9.9-windows-x64-setup.exe', state: 'uploaded', browser_download_url: 'https://example.com/setup.exe' },
      ],
    }) });
  });
  await page.goto('./');
  await expect(page.locator('.download-actions .button')).toHaveAttribute('href', 'https://example.com/setup.exe');
  await expect(page.locator('.download-actions .button')).toHaveText(/v0\.9\.9/);
  await expect(page.locator('.download-actions .text-link').first()).toHaveAttribute('href', 'https://example.com/portable.zip');
});

test('seven chapters support reverse, fast jumps and language preservation', async ({ page }, info) => {
  await page.goto('./');
  for (const stage of [0, 1, 2, 3, 4, 5, 6, 2, 0, 5]) {
    await page.locator('.chapter-nav a').nth(stage).click();
    await expect(page.locator('.scene')).toHaveAttribute('data-stage', String(stage));
    await expect(page.locator('.chapter-nav a').nth(stage)).toHaveAttribute('aria-current', 'step');
    if (stage < 7) await page.screenshot({ path: info.outputPath(`stage-${stage}.png`) });
  }
  await page.locator('.language').click();
  await expect(page).toHaveURL(/\/en\/#obs$/);
  await expect(page.locator('.scene')).toHaveAttribute('data-stage', '5');
  await page.reload();
  await expect(page.locator('.scene')).toHaveAttribute('data-stage', '5');
  await page.locator('.chapter-nav a').nth(4).click();
  await expect(page.locator('.audio-pending')).toBeVisible();
  await expect(page.locator('.audio-demo button')).toHaveCount(0);
});

test('scroll position is deterministic forward and backward', async ({ page }) => {
  await page.goto('./');
  for (const ratio of [0.95, 0.65, 0.2, 0.01, 0.8]) {
    await page.evaluate(ratio => window.scrollTo(0, (document.querySelector('#story')!.clientHeight - innerHeight) * ratio), ratio);
    await expect(page.locator('.scene')).toHaveAttribute('data-stage', String(Math.min(6, Math.floor(ratio * 7))));
  }
});

test('keyboard skip, guide, FAQ and language survive narrow and landscape layouts', async ({ page }, info) => {
  await page.goto('en/');
  await page.keyboard.press('Tab');
  await expect(page.locator('.skip-link')).toBeFocused();
  await page.keyboard.press('Enter');
  await expect(page.locator('#guide')).toBeFocused();
  await page.locator('summary').first().click();
  await expect(page.locator('details').first()).toHaveAttribute('open', '');
  await page.screenshot({ path: info.outputPath('guide.png'), fullPage: false });
  for (const viewport of [{ width: 320, height: 740 }, { width: 844, height: 390 }]) {
    await page.setViewportSize(viewport);
    await page.goto('en/#listen');
    await expect(page.locator('.scene')).toHaveAttribute('data-stage', '4');
    expect(await page.evaluate(() => document.documentElement.scrollWidth <= innerWidth)).toBe(true);
    await page.screenshot({ path: info.outputPath(`viewport-${viewport.width}.png`) });
  }
});
