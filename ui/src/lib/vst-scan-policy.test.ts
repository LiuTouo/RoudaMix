import assert from "node:assert";
import { readFileSync } from "node:fs";
import { join } from "node:path";
import { test } from "node:test";

test("加入 VST 只開清單；掃描入口位於頂欄", () => {
  const app = readFileSync(join(process.cwd(), "src", "App.svelte"), "utf8");
  const strip = readFileSync(
    join(process.cwd(), "src", "lib", "TrackStrip.svelte"),
    "utf8",
  );

  assert.match(app, /: "掃描 VST"}<\/button/);
  assert.match(app, /scanRunning \? void cancelScan\(\) : void startScan\(\)/);
  assert.match(app, /await startScan\(\);/);
  assert.match(app, /scanRequestPending/);
  assert.match(strip, />＋ 加入<\/button/);
  assert.doesNotMatch(strip, /onScan|掃描加入|>重新掃描<\/button/);
});
