import { test } from 'node:test';
import assert from 'node:assert/strict';
import { content } from '../src/content.ts';

test('both tutorials distinguish installation, Audio settings, Session and actual routing controls', () => {
  for (const t of Object.values(content)) {
    const guide = t.steps.flat().join(' ');
    assert.match(guide, /setup\.exe/);
    assert.match(guide, /portable\.zip/);
    assert.match(guide, /WebView2 Evergreen Runtime/);
    assert.match(guide, /data/);
    assert.match(guide, /Session/);
    assert.match(guide, /場景/);
    assert.match(guide, /MON（監聽）|MON \(監聽\)/);
    assert.match(guide, /STR（串流）|STR \(串流\)/);
    assert.match(guide, /FX.*送至/);
    assert.match(guide, /低延遲/);
    assert.match(guide, /不勾選即 Full PDC|unchecked on stream for Full PDC/);
    assert.match(guide, /Monitor Bypass/);
    assert.doesNotMatch(JSON.stringify(t), /音訊 \/ Session|Audio \/ Session|Monitor Bypass[（ ]\(M\)|Monitor Bypass（M）/);
    assert.match(t.steps[4][1], /CABLE Input/);
    assert.match(t.steps[5][1], /CABLE Output/);
    assert.match(t.faq[4][1], /不是|No\./);
  }
});
