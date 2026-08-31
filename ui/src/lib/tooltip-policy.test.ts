import { readdirSync, readFileSync } from "node:fs";
import { extname, join, relative } from "node:path";
import assert from "node:assert";
import { test } from "node:test";

const UI_ROOT = process.cwd();
const SCANNED_EXTENSIONS = new Set([".svelte", ".html"]);

function sourceFiles(directory: string): string[] {
  return readdirSync(directory, { withFileTypes: true }).flatMap((entry) => {
    const path = join(directory, entry.name);
    if (entry.isDirectory()) return sourceFiles(path);
    return SCANNED_EXTENSIONS.has(extname(entry.name)) ? [path] : [];
  });
}

test("UI 提示統一使用 data-tooltip，不得使用瀏覽器原生 title", () => {
  const files = [...sourceFiles(join(UI_ROOT, "src")), join(UI_ROOT, "index.html")];
  const offenders = files
    .filter((file) => /\btitle\s*=/.test(readFileSync(file, "utf8")))
    .map((file) => relative(UI_ROOT, file));

  assert.deepEqual(
    offenders,
    [],
    `請將原生 title 提示改為 data-tooltip：${offenders.join(", ")}`,
  );
});
