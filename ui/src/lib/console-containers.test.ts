import test from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { STRIP_W, STRIP_GAP } from './laneView.ts';

const app = readFileSync(new URL('../App.svelte', import.meta.url), 'utf8');
const css = readFileSync(new URL('../console.css', import.meta.url), 'utf8');
const strip = readFileSync(new URL('./TrackStrip.svelte', import.meta.url), 'utf8');

test('App gives the real input/output lanes named containers and authoritative track counts', () => {
  for (const [group, label, tracks] of [['inputs', 'Inputs', 'inputTracks'], ['outputs', 'Outputs', 'outputTracks']]) {
    assert.ok(app.includes(`<section class="group" aria-labelledby="${group}-heading">`));
    assert.ok(app.includes(`<h2 id="${group}-heading">${label}</h2>`));
    assert.ok(app.includes(`<span class="track-count">{${tracks}.length}</span>`));
  }
  assert.equal(app.match(/<TrackStrip\s/g)?.length, 2);
  assert.match(app, /inputTracks\.slice\(inputWin\.start, inputWin\.end\)/);
  assert.match(app, /outputTracks\.slice\(outputWin\.start, outputWin\.end\)/);
  assert.match(app, /尚未加入輸入來源/);
  assert.match(app, /新增輸出軌\(監聽 \/ 串流\)/);
  assert.doesNotMatch(app, /ContainerPrototype|PrototypeSwitcher|consolePrototypeFixtures/);
});

test('console container gutters stay outside scrollable lanes and preserve virtualization pitch', () => {
  const board = css.match(/& \.board \{([^}]+)\}/)![1];
  const group = css.match(/& \.group \{([^}]+)\}/)![1];
  const lanes = css.match(/& \.lanes \{([^}]+)\}/)![1];
  assert.match(board, /grid-template-columns: minmax\(256px, 2fr\) minmax\(256px, 1fr\)/);
  assert.match(board, /gap: 20px/);
  assert.match(group, /padding: 0 12px 12px/);
  assert.match(group, /background: #090B0D/);
  assert.match(group, /box-shadow: inset/);
  assert.match(lanes, /overflow: auto/);
  assert.doesNotMatch(lanes, /padding|border|gap:/);
  assert.doesNotMatch(css, /& \.group \+ \.group/);
  assert.ok(app.includes(`gap: ${STRIP_GAP}px;`));
  assert.ok(strip.includes(`flex: 0 0 ${STRIP_W}px;`));
});

test('M/B switches flow-align on the routing row; dropdown rows are never overlaid', () => {
  // 舊版 .console-switches 絶對定位 top:68px 壓在「送至」下拉列上；改為第三列流排版。
  const switches = css.match(/& \.console-switches \{([^}]+)\}/)![1];
  assert.match(switches, /margin-left: auto/);
  assert.doesNotMatch(switches, /position: absolute|top: \d+px|right: \d+px/);
  const routing = css.match(/& \.routing-row \{([^}]+)\}/)![1];
  assert.doesNotMatch(routing, /padding-right/);
  const row = strip.match(/<div class="row routing-row"[\s\S]*?<div class="console-switches">[\s\S]*?<\/div>\r?\n  <\/div>/)![0];
  assert.ok(row.includes('<div class="console-switches">'));
  assert.doesNotMatch(strip, /class="row destination-row routing-row"/);
  assert.ok(strip.indexOf('<div class="row routing-row"') < strip.indexOf('<div class="lower">'));
});
