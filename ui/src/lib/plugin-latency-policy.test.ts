import assert from "node:assert";
import { readFileSync } from "node:fs";
import { join } from "node:path";
import { test } from "node:test";

test("Plugin Latency UI 由 capability 啟用並提供摘要、drawer 與軌條控制", () => {
  const app = readFileSync(join(process.cwd(), "src", "App.svelte"), "utf8");
  const strip = readFileSync(
    join(process.cwd(), "src", "lib", "TrackStrip.svelte"),
    "utf8",
  );
  const drawer = readFileSync(
    join(process.cwd(), "src", "lib", "LatencyDrawer.svelte"),
    "utf8",
  );
  const ipc = readFileSync(join(process.cwd(), "src", "lib", "ipc.ts"), "utf8");

  assert.match(app, /pluginLatencyPdcV1/);
  assert.match(app, /plug M/);
  assert.match(app, /LatencyDrawer/);
  assert.match(strip, /set_monitor_bypass/);
  assert.match(strip, /track_set_latency_policy/);
  assert.match(strip, /Monitor Bypass/);
  assert.match(strip, /pendingMonitorIds/);
  assert.match(strip, /engine 確認前不改變目前狀態/);
  assert.match(strip, /select\.value = previous/);
  assert.match(strip, /disabled=\{pendingLatencyPolicy\}/);
  assert.match(ipc, /telemetry-abi-mismatch/);
  assert.match(app, /Plugin Process Load 暫時不可用；Plugin Latency 與 PDC 仍正常/);
  assert.match(app, /observeLatencyRuntime/);
  assert.match(app, /受影響路徑改送 dry audio/);
  assert.match(app, /已恢復/);
  assert.match(drawer, /!status\?\.running[\s\S]*?\? "—"/);
  assert.match(strip, /右鍵選單.*Monitor Bypass|Monitor Bypass.*右鍵選單/s);
  assert.doesNotMatch(
    drawer,
    /\$effect\(\(\) => \{[\s\S]*?\.\.\.loadView[\s\S]*?loadView\s*=/,
    "telemetry effect 不得同時讀寫 loadView，否則 Svelte 會無限重新排程並凍結 UI",
  );
});
