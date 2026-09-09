import { test } from 'node:test';
import assert from 'node:assert/strict';
import { createServer } from 'node:http';
import { readFile, stat } from 'node:fs/promises';
import { resolve, extname, sep } from 'node:path';
import { chromium } from '@playwright/test';

test('built files serve both language entries and their assets without SPA fallback', async () => {
  const root = resolve('dist');
  const types = { '.html': 'text/html', '.js': 'text/javascript', '.css': 'text/css', '.png': 'image/png' };
  const server = createServer(async (request, response) => {
    try {
      const pathname = new URL(request.url, 'http://localhost').pathname;
      if (!pathname.startsWith('/RoudaMix/')) throw new Error('Base path required');
      let file = resolve(root, '.' + pathname.slice('/RoudaMix'.length));
      if (file !== root && !file.startsWith(root + sep)) throw new Error('Invalid path');
      if ((await stat(file)).isDirectory()) file = resolve(file, 'index.html');
      response.writeHead(200, { 'Content-Type': types[extname(file)] || 'application/octet-stream' });
      response.end(await readFile(file));
    } catch { response.writeHead(404); response.end('Not found'); }
  });
  await new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
  const browser = await chromium.launch();
  try {
    const base = `http://127.0.0.1:${server.address().port}/RoudaMix/`;
    const page = await browser.newPage();
    const failures = [];
    page.on('pageerror', error => failures.push(error.message));
    page.on('response', response => { if (response.url().startsWith(base) && response.status() >= 400) failures.push(response.url()); });
    for (const [route, language, title] of [['', 'zh-Hant', '你的聲音'], ['en/', 'en', 'Your sound']]) {
      const html = await readFile(resolve(root, route, 'index.html'), 'utf8');
      assert.match(html, new RegExp(`<html lang="${language}">`));
      assert.match(html, new RegExp(title));
      assert.match(html, /property="og:description"/);
      await page.goto(`${base}${route}#obs`);
      await page.waitForSelector('.scene[data-stage="5"]');
      await page.reload();
      await page.waitForSelector('.scene[data-stage="5"]');
      assert.equal(await page.locator('[title]').count(), 0);
      assert.equal(await page.locator('.release-pending').count(), 1);
      assert.equal(await page.locator('.brand img').first().evaluate(image => image.naturalWidth > 0), true);
    }
    assert.deepEqual(failures, []);
  } finally { await browser.close(); await new Promise(resolve => server.close(resolve)); }
});
