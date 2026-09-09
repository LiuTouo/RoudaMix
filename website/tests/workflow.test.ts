import { test } from 'node:test';
import assert from 'node:assert/strict';
import { workflowMotion } from '../src/workflowMotion.ts';

test('workflow actions have visible intermediate states and ordered consequences', () => {
  const at = (stage: number, local: number) => workflowMotion((stage + local) / 7);
  assert.equal(at(1, 0.08).inputReveal, 0);
  assert.ok(at(1, 0.24).deviceMenu > 0.9);
  assert.ok(at(1, 0.5).inputReveal > 0 && at(1, 0.5).inputReveal < 1);
  assert.ok(at(2, 0.3).pluginMenu > 0.9);
  assert.equal(at(2, 0.3).pluginInsert, 0);
  assert.ok(at(2, 0.5).pluginInsert > 0 && at(2, 0.5).pluginInsert < 1);
  assert.equal(at(3, 0.36).monitorRoute, 1);
  assert.equal(at(3, 0.36).streamRoute, 0);
  assert.ok(at(4, 0.38).bypassMix > 0 && at(4, 0.38).bypassMix < 1);
  assert.ok(at(5, 0.42).cableRoute > 0 && at(5, 0.42).cableRoute < 1);
  assert.equal(at(5, 0.42).obs, false);
  assert.equal(at(5, 0.78).obs, true);
});

test('every causal transform is reversible and continuous across chapter boundaries', () => {
  const fields = ['launch','inputReveal','pluginInsert','monitorRoute','streamRoute','bypassMix','obsReveal','cableRoute','voiceReveal','voiceRoute'] as const;
  for (let chapter = 1; chapter < 7; chapter++) {
    const before = workflowMotion((chapter - 0.00001) / 7);
    const after = workflowMotion((chapter + 0.00001) / 7);
    for (const field of fields) assert.ok(Math.abs(before[field] - after[field]) < 0.001, `${chapter}: ${field}`);
  }
  const progress = [0.05, 0.2, 0.36, 0.55, 0.63, 0.82, 0.99];
  const forward = progress.map(workflowMotion);
  progress.toReversed().forEach((p, index) => assert.deepEqual(workflowMotion(p), forward[forward.length - 1 - index]));
});
