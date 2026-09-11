<script lang="ts">
  import type { ComponentProps } from "svelte";
  import PluginPicker from "../PluginPicker.svelte";

  let { initial }: { initial: ComponentProps<typeof PluginPicker> } = $props();
  let changes = $state<Partial<ComponentProps<typeof PluginPicker>>>({});
  export function update(next: Partial<ComponentProps<typeof PluginPicker>>) {
    changes = { ...changes, ...next };
  }
</script>

<PluginPicker {...initial} {...changes} />
