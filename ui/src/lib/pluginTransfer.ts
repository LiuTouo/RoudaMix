import { get, writable } from "svelte/store";
import { engineCommand } from "./protocol-commands.generated";
import { removeDragGhost } from "./ghost";

export const PLUGIN_DRAG_TYPE = "application/x-roudamix-plugin";
type Clipboard = { clipboardId: string; name: string };
type PluginDrag = { instanceId: number; trackId: number; copyable: boolean };
export const pluginTransfer = writable<{
  clipboard: Clipboard | null; drag: PluginDrag | null; busy: boolean;
}>({ clipboard: null, drag: null, busy: false });
let generation = 0;

export function resetPluginTransfer(): void {
  ++generation;
  removeDragGhost();
  pluginTransfer.set({ clipboard: null, drag: null, busy: false });
}
export function beginPluginDrag(drag: PluginDrag): void {
  pluginTransfer.update((s) => ({ ...s, drag }));
}
export function endPluginDrag(): void {
  removeDragGhost();
  pluginTransfer.update((s) => ({ ...s, drag: null }));
}

async function transfer(action: (current: number) => Promise<void>): Promise<void> {
  if (get(pluginTransfer).busy) return;
  const current = generation;
  pluginTransfer.update((s) => ({ ...s, busy: true }));
  try { await action(current); }
  finally {
    if (generation === current) pluginTransfer.update((s) => ({ ...s, busy: false }));
  }
}
export function copyPlugin(instanceId: number): Promise<void> {
  return transfer(async (current) => {
    const clipboard = await engineCommand("copy_plugin", { instanceId });
    if (generation === current) pluginTransfer.update((s) => ({ ...s, clipboard }));
  });
}
export function pastePlugin(trackId: number, newIndex: number): Promise<void> {
  const clipboard = get(pluginTransfer).clipboard;
  if (!clipboard) return Promise.resolve();
  return transfer(async () => {
    await engineCommand("paste_plugin", { clipboardId: clipboard.clipboardId, trackId, newIndex });
  });
}
export function duplicatePlugin(instanceId: number, trackId: number, newIndex: number): Promise<void> {
  return transfer(async () => {
    await engineCommand("duplicate_plugin", { instanceId, trackId, newIndex });
  });
}
