import { test } from 'node:test';
import assert from 'node:assert/strict';
import { resolveReducedMotion } from '../src/motionPreference.ts';

test('system preference is respected until the visitor explicitly overrides it', () => {
  assert.equal(resolveReducedMotion(true, null), true);
  assert.equal(resolveReducedMotion(false, null), false);
  assert.equal(resolveReducedMotion(true, 'full'), false);
  assert.equal(resolveReducedMotion(false, 'reduced'), true);
  assert.equal(resolveReducedMotion(true, 'invalid'), true);
});
