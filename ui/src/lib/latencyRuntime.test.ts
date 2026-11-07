// P1 觀察狀態測試:plugin 執行狀態通知去重、恢復通知、驅逐已移除 instance
import { test } from "node:test";
import assert from "node:assert";
import { observeLatencyRuntimeStates } from "./latencyRuntime.ts";
import type { RackSlot, Track } from "./types.ts";

function trackWith(plugins: Array<Partial<RackSlot> & { instanceId: number }>): Track[] {
  return [
    {
      trackId: 1,
      kind: "audio",
      name: "T",
      color: 0,
      source: null,
      dests: [],
      output: null,
      gain: 1,
      mute: false,
      plugins: plugins.map((p) => ({
        name: `Plugin ${p.instanceId}`,
        pluginPath: "x.vst3",
        classId: "",
        bypassed: false,
        params: [],
        ...p,
      })),
    },
  ];
}

test("首次 suspended 回 error 通知;同狀態重跑不重複通知", () => {
  const states = new Map<string, string>();
  const first = observeLatencyRuntimeStates(
    states,
    trackWith([{ instanceId: 7, runtimeState: "suspended" }]),
  );
  assert.equal(first.length, 1);
  assert.equal(first[0].level, "error");
  assert.match(first[0].text, /Plugin 7 的 Primary 已暫停/);
  const again = observeLatencyRuntimeStates(
    states,
    trackWith([{ instanceId: 7, runtimeState: "suspended" }]),
  );
  assert.equal(again.length, 0);
});

test("suspended → active 回 info 恢復通知(自動消失時效由呼叫端決定)", () => {
  const states = new Map<string, string>();
  observeLatencyRuntimeStates(states, trackWith([{ instanceId: 7, runtimeState: "suspended" }]));
  const recovered = observeLatencyRuntimeStates(
    states,
    trackWith([{ instanceId: 7, runtimeState: "active" }]),
  );
  assert.equal(recovered.length, 1);
  assert.equal(recovered[0].level, "info");
  assert.match(recovered[0].text, /已恢復/);
});

test("monitor 與 primary 分開觀察", () => {
  const states = new Map<string, string>();
  const notices = observeLatencyRuntimeStates(
    states,
    trackWith([{ instanceId: 7, monitorState: "degraded" }]),
  );
  assert.equal(notices.length, 1);
  assert.match(notices[0].text, /Monitor Shadow 已降級/);
  assert.deepEqual([...states.keys()].sort(), ["7:monitor", "7:primary"]);
});

test("移除的 plugin 被驅逐:instanceId 跨代重用時不得誤發「已恢復」", () => {
  const states = new Map<string, string>();
  observeLatencyRuntimeStates(states, trackWith([{ instanceId: 7, runtimeState: "suspended" }]));
  assert.equal(states.size, 2);
  // plugin 7 已從圖上移除 → 觀察狀態清空,Map 不隨新增/移除無限成長
  observeLatencyRuntimeStates(states, []);
  assert.equal(states.size, 0);
  // instanceId 重用的新 plugin active:previous 已清,不得誤發恢復通知
  const reused = observeLatencyRuntimeStates(
    states,
    trackWith([{ instanceId: 7, runtimeState: "active" }]),
  );
  assert.equal(reused.length, 0);
});
