<script lang="ts">
  import TrackStrip from "../TrackStrip.svelte";
  import type { Track } from "../types";

  let { initialTracks }: { initialTracks: Track[] } = $props();
  let updates = $state<Track[] | null>(null);
  const tracks = $derived(updates ?? initialTracks);
  export function setTracks(next: Track[]) { updates = next; }
</script>

<TrackStrip track={tracks[0]} {tracks} devices={[]} selectedDeviceKey=""
  metered={false} onCancelScan={() => {}} openMenu={() => {}} />
