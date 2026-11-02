import assert from "node:assert";
import { test } from "node:test";
import { sanitizeFontFamily } from "./uiFont.ts";

test("合法字族名原樣保留", () => {
  assert.equal(sanitizeFontFamily("Segoe UI"), "Segoe UI");
  assert.equal(sanitizeFontFamily("微軟正黑體"), "微軟正黑體");
  assert.equal(sanitizeFontFamily("  IBM Plex Sans TC  "), "IBM Plex Sans TC");
});

test("null/空字串/空白 → null（用預設）", () => {
  assert.equal(sanitizeFontFamily(null), null);
  assert.equal(sanitizeFontFamily(undefined), null);
  assert.equal(sanitizeFontFamily(""), null);
  assert.equal(sanitizeFontFamily("   "), null);
});

test("CSS 字串逃脫字元 → null", () => {
  assert.equal(sanitizeFontFamily('Bad"Name'), null);
  assert.equal(sanitizeFontFamily("Back\\slash"), null);
  assert.equal(sanitizeFontFamily("a;b"), null);
  assert.equal(sanitizeFontFamily("a{}b"), null);
  assert.equal(sanitizeFontFamily("a\nb"), null);
  assert.equal(sanitizeFontFamily("a\u0000b"), null);
});

test("過長字族名 → null", () => {
  assert.equal(sanitizeFontFamily("A".repeat(65)), null);
  assert.equal(sanitizeFontFamily("A".repeat(64)), "A".repeat(64));
});
