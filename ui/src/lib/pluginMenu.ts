import type { RackSlot } from "./types";

/** Plugin 右鍵選單項目:純計算,run 由呼叫端接線。與 row 控制同一組規則
 *  (placeholder 不可編輯、鏈端不可再移、Monitor Bypass 隨 capability 出現)。 */
export type PluginMenuTo = "first" | "last" | "up" | "down";
export type PluginMenuItem = { label: string; disabled?: boolean; checked?: boolean; run: () => void };

export function pluginMenuItems(input: {
  slot: Pick<RackSlot, "name" | "availability">;
  index: number;
  chainLength: number;
  latencyEnabled: boolean;
  monitorBypassShown: boolean;
  monitorBypassPending: boolean;
  copy?: { disabled: boolean; run: () => void };
  paste?: { disabled: boolean; run: () => void };
  rename?: { run: () => void };
  run: { editor: () => void; move: (to: PluginMenuTo) => void; monitorBypass: () => void; remove: () => void };
}): PluginMenuItem[] {
  const { slot, index, chainLength, latencyEnabled, run } = input;
  const placeholder = slot.availability !== undefined && slot.availability !== "ok";
  const atStart = index <= 0;
  const atEnd = index >= chainLength - 1;
  return [
    { label: "編輯", disabled: placeholder, run: run.editor },
    ...(input.rename ? [{ label: "重新命名", run: input.rename.run }] : []),
    ...(input.copy ? [{ label: "複製", disabled: placeholder || input.copy.disabled, run: input.copy.run }] : []),
    ...(input.paste ? [{ label: "在此插件後貼上", ...input.paste }] : []),
    { label: "上移", disabled: atStart, run: () => run.move("up") },
    { label: "下移", disabled: atEnd, run: () => run.move("down") },
    { label: "移到最前", disabled: atStart, run: () => run.move("first") },
    { label: "移到最後", disabled: atEnd, run: () => run.move("last") },
    ...(latencyEnabled
      ? [{ label: "監聽略過（Monitor Bypass）",
          // 選單開啟期間仍要追蹤 engine 確認與回滾。
          get checked() { return input.monitorBypassShown; },
          get disabled() { return input.monitorBypassPending; },
          run: run.monitorBypass }]
      : []),
    { label: "移除插件…", run: run.remove },
  ];
}
