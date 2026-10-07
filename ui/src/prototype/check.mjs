// 暫用瀏覽器驗收。既有 Playwright 可用 PLAYWRIGHT_MODULE 指定，不加入正式依賴。
import assert from 'node:assert/strict';
import { mkdir, writeFile, readdir, readFile } from 'node:fs/promises';
import { fileURLToPath } from 'node:url';
const { chromium } = await import(process.env.PLAYWRIGHT_MODULE || 'playwright');
const out = fileURLToPath(new URL('../../../.scratch/geometric-flat-ui/', import.meta.url));
await mkdir(out, { recursive: true });
const browser = await chromium.launch({ headless: true, ...(process.env.CHROMIUM_PATH ? { executablePath: process.env.CHROMIUM_PATH } : {}) });
const context = await browser.newContext({ viewport: { width: 1600, height: 1000 }, reducedMotion: 'reduce' });
const page = await context.newPage();
page.setDefaultTimeout(8000);
const report = { errors: [], screens: [], interactions: [], contrast: [], geometry: [] };
page.on('pageerror', error => report.errors.push(String(error)));
page.on('request', request => {
  if (!request.url().startsWith('http://127.0.0.1:5188/') && !request.url().startsWith('data:')) report.errors.push(`外部請求：${request.url()}`);
});
async function variant(key) {
  for (let step = 0; step < 4 && await page.locator('html').getAttribute('data-prototype-variant') !== key; step++) {
    await page.locator('[data-step="1"]').click();
  }
  assert.equal(await page.locator('html').getAttribute('data-prototype-variant'), key);
}
async function geometry() {
  return page.evaluate(() => [...document.querySelectorAll('#app .bar, #app main, #app .strip, #app button, #app input, #app select, #app dialog[open], #app .destination-popup:popover-open')]
    .filter(el => !el.closest('[data-ui-tool]') && el.getBoundingClientRect().width > 0 && el.getBoundingClientRect().height > 0)
    .map(el => { const r = el.getBoundingClientRect(); return [el.tagName, ...[r.x, r.y, r.width, r.height].map(n => Math.round(n * 10) / 10)]; }));
}
async function compare(screen, capture = true) {
  await variant('original');
  await page.evaluate(() => Promise.all(document.getAnimations().filter(a => a.effect?.getTiming().iterations !== Infinity).map(a => a.finished.catch(() => {}))));
  const baseline = await geometry();
  for (const key of ['original', 'A', 'B', 'C']) {
    await variant(key);
    await page.mouse.move(1590, 0);
    const current = await geometry();
    assert.deepEqual(current, baseline, `${screen}/${key} 改變配置`);
    report.geometry.push(`${screen}/${key}`);
    if (key !== 'original') {
      const decoration = await page.evaluate(() => [...document.querySelectorAll('#app *')]
        .filter(el => !el.closest('[data-ui-tool]') && el.getBoundingClientRect().width && el.getBoundingClientRect().height)
        .filter(el => { const s = getComputedStyle(el); return s.boxShadow !== 'none' || s.backgroundImage.includes('gradient'); })
        .map(el => el.className));
      assert.deepEqual(decoration, [], `${screen}/${key} 殘留立體裝飾`);
    }
    if (capture) {
      const file = `${key}-${screen}.png`;
      await page.screenshot({ path: `${out}/${file}` });
      report.screens.push({ screen, variant: key, file });
    }
  }
}
try {
  await page.goto('http://127.0.0.1:5188/?variant=A');
  await page.locator('.strip').first().waitFor();
  await page.evaluate(() => document.fonts.ready);
  await page.locator('.capsule').waitFor();
  await page.locator('.capsule').waitFor({ state: 'hidden' });
  assert.equal(await page.locator('.strip').count(), 5);
  await page.getByRole('button', { name: '收合', exact: true }).click();
  await compare('main');
  await page.getByRole('button', { name: '設定', exact: true }).click();
  for (const [label, screen] of [['音訊 / Session', 'audio'], ['通用', 'general'], ['快捷鍵', 'hotkeys'], ['關於 / 更新', 'about']]) {
    await page.getByRole('button', { name: label, exact: true }).click();
    await compare(`settings-${screen}`);
  }
  await page.getByRole('button', { name: '檢查更新', exact: true }).click();
  await page.getByRole('button', { name: '前往下載新版', exact: true }).waitFor();
  await page.getByRole('button', { name: '前往下載新版', exact: true }).click();
  await page.getByRole('button', { name: '關閉設定視窗', exact: true }).click();
  await page.locator('.vstfoot button').first().click();
  for (const summary of await page.locator('.plugin-group summary').all()) await summary.click();
  await compare('plugins');
  const search = page.getByRole('searchbox');
  await search.fill('找不到的插件');
  await compare('plugins-empty');
  await search.fill('studio');
  await variant('A');
  assert.equal(await search.inputValue(), 'studio');
  await search.press('ArrowRight');
  assert.equal(await page.locator('html').getAttribute('data-prototype-variant'), 'A');
  await page.locator('[data-variant="A"]').focus();
  await page.keyboard.press('ArrowRight');
  assert.equal(await page.locator('html').getAttribute('data-prototype-variant'), 'B');
  await page.getByRole('button', { name: '關閉插件選擇器', exact: true }).click();
  await page.getByRole('button', { name: '更換', exact: true }).click();
  await compare('app-picker');
  await page.getByRole('button', { name: '關閉程序選擇器', exact: true }).click();
  await page.locator('.destination-trigger').first().click();
  await compare('destinations');
  assert.equal(await page.locator('.destination-popup:popover-open').count(), 1);
  await page.keyboard.press('Escape');
  await page.getByRole('button', { name: /空間效果 的側鏈來源/ }).click();
  await compare('sidechain');
  await page.locator('.destination-popup:popover-open').getByRole('checkbox', { name: '音樂播放' }).check();
  await page.keyboard.press('Escape');
  await page.getByRole('button', { name: '開啟 Plugin 延遲與 Process Load 明細' }).click();
  await page.locator('.latency-output').first().waitFor();
  await compare('latency');
  assert.match(await page.locator('.latency-output').first().innerText(), /Low Latency/);
  await page.getByRole('button', { name: '關閉延遲明細', exact: true }).click();
  await page.locator('.plug').first().click({ button: 'right', position: { x: 3, y: 3 } });
  await compare('context-menu');
  await page.getByRole('menuitem', { name: '複製', exact: true }).click();
  await page.locator('.plug').first().click({ button: 'right', position: { x: 3, y: 3 } });
  await page.getByRole('menuitem', { name: '在此插件後貼上', exact: true }).click();
  assert.equal(await page.locator('.strip').first().locator('.plug').count(), 3);
  await page.getByRole('button', { name: '移除 plugin Voice Compressor(會先確認)', exact: true }).click();
  await compare('confirm');
  await page.getByRole('button', { name: '取消', exact: true }).click();
  const mute = page.locator('.strip').first().locator('.fctl .mute');
  await mute.click();
  await variant('A');
  assert.equal(await mute.getAttribute('aria-pressed'), 'true');
  await page.getByRole('button', { name: '展開', exact: true }).click();
  await page.getByRole('button', { name: '模擬關閉', exact: true }).click();
  await page.getByRole('button', { name: '收合', exact: true }).click();
  await compare('close-behavior');
  await page.getByRole('button', { name: '關閉程式', exact: true }).click();
  await page.getByRole('button', { name: '捨棄變更', exact: true }).waitFor();
  await compare('unsaved');
  await page.getByRole('button', { name: '取消', exact: true }).click();
  await page.getByRole('button', { name: '展開', exact: true }).click();
  await page.getByRole('button', { name: '顯示警示', exact: true }).click();
  await page.getByRole('button', { name: '收合', exact: true }).click();
  await page.locator('.capsule.iserr').waitFor();
  await compare('warning');
  report.interactions.push('搜尋、選單、側鏈、複製貼上、靜音、更新、關閉保護、通知與樣式切換正常；輸入方向鍵不切換版本');
  await page.reload();
  await page.locator('.strip').first().waitFor();
  await page.locator('.capsule').waitFor();
  await page.locator('.capsule').waitFor({ state: 'hidden' });
  assert.equal(new URL(page.url()).searchParams.get('variant'), 'C');
  assert.equal(await page.locator('.strip').first().locator('.plug').count(), 2);
  assert.equal(await page.locator('.strip').first().locator('.fctl .mute').getAttribute('aria-pressed'), 'false');
  for (const [width, height] of [[1100, 820], [900, 600]]) {
    await page.setViewportSize({ width, height });
    await page.getByRole('button', { name: '收合', exact: true }).click();
    await compare(`main-${width}`, false);
    await page.getByRole('button', { name: '設定', exact: true }).click();
    await compare(`settings-${width}`, false);
    await page.getByRole('button', { name: '關閉設定視窗', exact: true }).click();
    await page.locator('.vstfoot button').first().click();
    await compare(`plugins-${width}`, false);
    await page.getByRole('button', { name: '關閉插件選擇器', exact: true }).click();
    await page.getByRole('button', { name: '展開', exact: true }).click();
  }
  await page.setViewportSize({ width: 1600, height: 1000 });
  for (const key of ['A', 'B', 'C']) {
    await variant(key);
    const ratios = await page.evaluate(() => {
      const s = getComputedStyle(document.documentElement);
      const luminance = hex => hex.trim().slice(1).match(/../g).map(n => parseInt(n, 16) / 255)
        .map(v => v <= .04045 ? v / 12.92 : ((v + .055) / 1.055) ** 2.4)
        .reduce((v, n, i) => v + n * [.2126, .7152, .0722][i], 0);
      return ['--text', '--text-dim', '--accent'].map(token => ({ token, minimum: Math.min(...[0, 1, 2, 3, 4].map(level => {
        const a = luminance(s.getPropertyValue(token)), b = luminance(s.getPropertyValue(`--flat-${level}`));
        return (Math.max(a, b) + .05) / (Math.min(a, b) + .05);
      })) }));
    });
    for (const ratio of ratios) assert.ok(ratio.minimum >= 4.5, `${key}/${ratio.token} 對比 ${ratio.minimum}`);
    report.contrast.push({ key, ratios });
  }
  await page.getByRole('button', { name: '設定', exact: true }).click();
  const close = page.getByRole('button', { name: '關閉設定視窗', exact: true });
  await close.hover();
  await page.locator('.app-tooltip:not([hidden])').waitFor();
  assert.equal(await page.locator('.app-tooltip').evaluate(el => getComputedStyle(el).boxShadow), 'none');
  await close.focus();
  await page.keyboard.press('Tab');
  await page.keyboard.press('Shift+Tab');
  assert.notEqual(await close.evaluate(el => getComputedStyle(el).outlineStyle), 'none');
  assert.equal(await page.locator('#app [title]').count(), 0);
  const storage = await context.storageState();
  assert.equal(storage.origins.flatMap(origin => origin.localStorage).length, 0);
  // 正式產物不應含原型入口、資料或切換工具。
  const dist = new URL('../../../ui/dist/assets/', import.meta.url);
  for (const file of await readdir(dist)) {
    const content = await readFile(new URL(file, dist), 'utf8');
    assert.ok(!/prototype-toolbar|prototype-asio|幾何扁平 UI 原型/.test(content), `正式建置包含原型：${file}`);
  }
  assert.deepEqual(report.errors, []);
  report.interactions.push('重新整理保留版本並重設模擬資料；無持久儲存、外部請求或原生 title；正式產物不含原型');
  const names = { main: '混音主畫面', 'settings-audio': '音訊 / Session', 'settings-general': '通用設定',
    'settings-hotkeys': '快捷鍵', 'settings-about': '關於 / 更新', plugins: '插件列表', 'plugins-empty': '搜尋無結果',
    'app-picker': '應用程式選擇', destinations: '輸出路由', sidechain: '側鏈來源', latency: '延遲明細',
    'context-menu': '右鍵選單', confirm: '移除確認', 'close-behavior': '關閉行為', unsaved: '未儲存變更', warning: '警示通知' };
  const labels = { original: '現行樣式', A: 'A 直角色塊', B: 'B 細框幾何', C: 'C 連續分區' };
  await writeFile(`${out}/gallery.html`, `<!doctype html><html lang="zh-Hant"><meta charset="utf-8"><meta name="viewport" content="width=device-width, initial-scale=1"><title>RoudaMix 幾何扁平 UI 對照</title>
    <style>body{margin:32px;background:#111418;color:#edf2f7;font:15px/1.6 system-ui,sans-serif}h1{font-size:26px}a{color:#9acaff}nav{display:flex;flex-wrap:wrap;gap:8px 20px}section{margin:40px 0}h2{font-size:19px}.grid{display:grid;grid-template-columns:repeat(2,minmax(0,1fr));gap:20px}figure{margin:0}img{width:100%;display:block;border:1px solid #465365}figcaption{padding:8px 0}@media(max-width:800px){body{margin:16px}.grid{grid-template-columns:1fr}}</style>
    <h1>RoudaMix / 幾何扁平 UI</h1><p>保留配置 · 深色底 · 上層逐級變亮。<a href="http://127.0.0.1:5188/?variant=A">開啟可操作原型</a>；點截圖可看原尺寸。</p>
    <nav>${Object.entries(names).map(([key, label]) => `<a href="#${key}">${label}</a>`).join('')}</nav>
    ${Object.entries(names).map(([key, label]) => `<section id="${key}"><h2>${label}</h2><div class="grid">${report.screens.filter(s => s.screen === key).map(s => `<figure><a href="${s.file}"><img src="${s.file}" alt="${labels[s.variant]}：${label}" loading="lazy"></a><figcaption>${labels[s.variant]}</figcaption></figure>`).join('')}</div></section>`).join('')}</html>`);
  console.log(JSON.stringify({ screenshots: report.screens.length, geometryChecks: report.geometry.length, contrast: report.contrast, interactions: report.interactions }, null, 2));
} catch (error) {
  await page.screenshot({ path: `${out}/failure.png` });
  console.error((await page.locator('body').innerText()).slice(0, 3000));
  throw error;
} finally {
  await writeFile(`${out}/report.json`, JSON.stringify(report, null, 2));
  await browser.close();
}
