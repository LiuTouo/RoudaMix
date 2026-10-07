import { readdirSync, readFileSync } from "node:fs";
import { extname, join, relative } from "node:path";
import assert from "node:assert";
import { test } from "node:test";

const UI_ROOT = process.cwd();
const SCANNED_EXTENSIONS = new Set([".css", ".svelte", ".ts", ".html"]);

function sourceFiles(directory: string): string[] {
  return readdirSync(directory, { withFileTypes: true }).flatMap((entry) => {
    const path = join(directory, entry.name);
    if (entry.isDirectory()) return sourceFiles(path);
    return SCANNED_EXTENSIONS.has(extname(entry.name)) ? [path] : [];
  });
}

test("動畫一律播放：全 UI 禁用 prefers-reduced-motion（不連動 Win11 動畫設定，ADR 0007）", () => {
  // 測試自身（說明文字含關鍵字）不在掃描範圍
  const files = [...sourceFiles(join(UI_ROOT, "src")), join(UI_ROOT, "index.html")]
    .filter((file) => !file.endsWith("motion-policy.test.ts"));
  const offenders = files
    .filter((file) => readFileSync(file, "utf8").includes("prefers-reduced-motion"))
    .map((file) => relative(UI_ROOT, file));

  assert.deepEqual(
    offenders,
    [],
    `WebView2 的 OS「動畫效果」開關只會翻動 prefers-reduced-motion；` +
      `出現該字樣代表動畫又會被 Win11 設定關掉，請改走 motion.css token：${offenders.join(", ")}`,
  );
});

test("motion.css 定義完整動畫 token（4 時長 + 3 曲線）", () => {
  const css = readFileSync(join(UI_ROOT, "src", "motion.css"), "utf8");
  for (const token of [
    "--motion-dur-1",
    "--motion-dur-2",
    "--motion-dur-3",
    "--motion-dur-out",
    "--motion-ease-out",
    "--motion-ease-in",
    "--motion-ease-standard",
  ]) {
    assert.match(css, new RegExp(`${token}\\s*:`), `motion.css 缺少 token ${token}`);
  }
});
