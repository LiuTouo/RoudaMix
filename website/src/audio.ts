import type { DemoAudio } from './config';
import { motion } from './story';

/** Two decoded files share the same AudioContext clock and sample offset. */
export class DualAudio {
  private context: AudioContext | null = null;
  private buffers: AudioBuffer[] = [];
  private sources: AudioBufferSourceNode[] = [];
  private gains: GainNode[] = [];
  private offset = 0;
  private startedAt = 0;
  private generation = 0;
  private abort: AbortController | null = null;
  private selected = 0;
  playing = false;

  get duration() { return this.buffers[0]?.duration ?? 0; }
  get position() {
    return Math.min(this.duration, this.offset + (this.playing ? this.context!.currentTime - this.startedAt : 0));
  }

  async play(files: DemoAudio) {
    const generation = ++this.generation;
    this.context ??= new AudioContext();
    const ctx = this.context;
    await ctx.resume();
    if (generation !== this.generation) return;
    if (!this.buffers.length) {
      this.abort?.abort();
      this.abort = new AbortController();
      const signal = this.abort.signal;
      const buffers = await Promise.all([files.monitor, files.stream].map(async (url) => {
        const response = await fetch(url, { signal });
        if (!response.ok) throw new Error('Audio download failed');
        return ctx.decodeAudioData(await response.arrayBuffer());
      }));
      if (generation !== this.generation) return;
      if (Math.abs(buffers[0].duration - buffers[1].duration) > 0.02) throw new Error('Audio samples must have aligned durations');
      this.buffers = buffers;
    }
    if (generation !== this.generation || this.playing) return;
    if (this.offset >= this.duration) this.offset = 0;
    this.startedAt = ctx.currentTime + 0.015;
    this.sources = this.buffers.map((buffer, i) => {
      const source = ctx.createBufferSource();
      const gain = ctx.createGain();
      source.buffer = buffer;
      gain.gain.setValueAtTime(i === this.selected ? 1 : 0, ctx.currentTime);
      source.connect(gain).connect(ctx.destination);
      this.gains[i] = gain;
      source.start(this.startedAt, this.offset);
      return source;
    });
    this.playing = true;
    this.sources[0].onended = () => {
      if (generation === this.generation) this.pause();
    };
  }

  select(index: 0 | 1) {
    this.selected = index;
    const ctx = this.context;
    if (!ctx || !this.playing) return;
    this.gains.forEach((node, i) => {
      node.gain.cancelAndHoldAtTime(ctx.currentTime);
      node.gain.linearRampToValueAtTime(i === index ? 1 : 0, ctx.currentTime + motion.crossfadeSeconds);
    });
  }

  pause() {
    this.offset = Math.max(0, this.position);
    this.playing = false;
    ++this.generation;
    this.abort?.abort();
    this.sources.forEach(source => { source.onended = null; source.stop(); source.disconnect(); });
    this.gains.forEach(gain => gain.disconnect());
    this.sources = [];
    this.gains = [];
  }

  destroy() { this.pause(); void this.context?.close(); this.context = null; }
}
