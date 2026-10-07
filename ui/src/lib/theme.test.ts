import test from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";

const css = readFileSync(new URL("../flat-theme.css", import.meta.url), "utf8");

test("D uses neutral surfaces and separates toggled, dangerous and warning states", () => {
  for (const [i, hex] of ["121212", "1b1b1b", "232323", "2b2b2b", "353535"].entries()) {
    assert.ok(css.includes(`--flat-${i}: #${hex};`));
  }
  const toggled = css.match(/#app button\.on:not\(\.tabs button\) \{([^}]+)\}/)![1];
  assert.match(toggled, /color: var\(--active\)/);
  assert.doesNotMatch(toggled, /--warn|--err/);
  const danger = css.match(/#app button\.danger:not\(\.plugicon\) \{([^}]+)\}/)![1];
  assert.match(danger, /color: var\(--err\)/);
  assert.match(css, /\.plug\.placeholder \{ border-left-color: var\(--warn\); \}/);
});
