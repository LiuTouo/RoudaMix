import { test } from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync, readdirSync } from 'node:fs';
import { storyState, chapterProgress } from '../src/story.ts';
import { content } from '../src/content.ts';
import { publication } from '../src/config.ts';

test('direct jumps and reverse scrolling restore complete scene state', () => {
  for (const index of [0, 6, 2, 5, 4, 1, 0, 3]) {
    const state = storyState(chapterProgress(index));
    assert.equal(state.stage, index);
    assert.equal(state.bypass, index >= 4);
    assert.equal(state.obs, index >= 5);
    assert.equal(state.routed, index >= 3);
  }
  assert.equal(storyState(-1).stage, 0);
  assert.equal(storyState(Infinity).stage, 0);
  assert.equal(storyState(1).stage, 6);
});
test('both languages have every scene, guide step and FAQ', () => {
  for (const language of Object.values(content)) {
    assert.equal(language.scenes.length, 7);
    assert.equal(language.steps.length, 6);
    assert.equal(language.faq.length, 5);
    for (const scene of language.scenes) assert.ok(scene.every(Boolean));
  }
});
test('unready publication never offers a fake download or sample', () => {
  assert.equal(publication.release, null);
  assert.equal(publication.audio, null);
});
test('tooltip policy forbids native title attributes throughout the website', () => {
  for (const file of readdirSync(new URL('../src/', import.meta.url))) {
    if (!/\.(svelte|ts)$/.test(file)) continue;
    assert.doesNotMatch(readFileSync(new URL(`../src/${file}`, import.meta.url), 'utf8'), /\btitle\s*=/, file);
  }
});
