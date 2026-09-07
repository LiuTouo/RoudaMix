<script lang="ts">
  import TrackStrip from "../TrackStrip.svelte";
  import ContextMenu from "../ContextMenu.svelte";
  import type { Track } from "../types";
  import type { PluginMenuItem } from "../pluginMenu";
  let { initialTracks, enabled = true }: { initialTracks: Track[]; enabled?: boolean } = $props();
  let menu = $state<{ x: number; y: number; label: string; items: PluginMenuItem[] } | null>(null);
  let updated = $state<Track[] | null>(null);
  const tracks = $derived(updated ?? initialTracks);
  export function setTracks(next: Track[]) { updated = next; }
</script>
{#each tracks as track (track.trackId)}
  <TrackStrip {track} {tracks} devices={[]} selectedDeviceKey="" metered={false}
    pluginCopyEnabled={enabled} onCancelScan={() => {}}
    openMenu={(x, y, label, items) => { menu = { x, y, label, items }; }} />
{/each}
<ContextMenu {menu} onClose={() => { menu = null; }} />
