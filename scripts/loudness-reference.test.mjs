// Requires a Release native build and FFmpeg on PATH; failures are not silently skipped.
import assert from "node:assert/strict";
import { spawnSync } from "node:child_process";
import { mkdtempSync, rmSync, writeFileSync } from "node:fs";
import os from "node:os";
import path from "node:path";
import { fileURLToPath } from "node:url";
import test from "node:test";

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..");
function run(command, args) {
  const result = spawnSync(command, args, { encoding: "utf8", maxBuffer: 4 * 1024 * 1024 });
  assert.equal(result.status, 0, `${command}: ${result.error ?? ""}\n${result.stderr}`);
  return result;
}
function close(actual, expected, tolerance, name) {
  assert.ok(Number.isFinite(actual) && Number.isFinite(expected), `${name}: unavailable result`);
  assert.ok(Math.abs(actual - expected) <= tolerance, `${name}: native ${actual}, FFmpeg ${expected}`);
}
for (const { rate, intersample } of [
  ...[44100, 48000, 96000, 192000].map((rate) => ({ rate, intersample: false })),
  { rate: 48000, intersample: true },
]) {
  test(`native loudness agrees with independent FFmpeg ebur128 at ${rate} Hz${intersample ? " (intersample peak)" : ""}`, () => {
    const directory = mkdtempSync(path.join(os.tmpdir(), "roudamix-loudness-"));
    try {
      const file = path.join(directory, "stereo.f32");
      const frames = rate * 20;
      const pcm = Buffer.alloc(frames * 8);
      for (let i = 0; i < frames; ++i) {
        const amplitude = intersample ? .9 : i < rate * 10 ? .02 : .2;
        const frequency = intersample ? 12000 : 997;
        // Smooth endpoints isolate steady intersample peaks from differing FIR startup ringing.
        const edge = Math.min(i, frames - 1 - i, rate / 10) / (rate / 10);
        const envelope = intersample ? (1 - Math.cos(Math.PI * edge)) / 2 : 1;
        const sample = amplitude * envelope * Math.sin(2 * Math.PI * frequency * i / rate + (intersample ? Math.PI / 4 : 0));
        pcm.writeFloatLE(sample, i * 8);
        pcm.writeFloatLE(sample * .5, i * 8 + 4);
      }
      writeFileSync(file, pcm);
      const native = JSON.parse(run(path.join(root, "engine/build/Release/loudness_test.exe"), [String(rate), file]).stdout);
      const ref = run("ffmpeg", ["-hide_banner", "-nostats", "-f", "f32le", "-ar", String(rate), "-ac", "2", "-i", file,
        "-af", "ebur128=peak=true", "-f", "null", "-"]).stderr;
      const summary = ref.slice(ref.lastIndexOf("Summary:"));
      const integrated = Number(summary.match(/I:\s+(-?[\d.]+) LUFS/)[1]);
      const range = Number(summary.match(/LRA:\s+(-?[\d.]+) LU/)[1]);
      const peak = Number(summary.match(/Peak:\s+(-?[\d.]+) dBFS/)[1]);
      const windows = [...ref.matchAll(/M:\s+(-?[\d.]+) S:\s+(-?[\d.]+)/g)].at(-1);
      close(native.integratedLufs, integrated, .15, "I");
      close(native.loudnessRangeLu, range, .2, "LRA");
      close(native.truePeakDbtp, peak, .3, "TP");
      close(native.momentaryLufs, Number(windows[1]), .15, "M");
      close(native.shortTermLufs, Number(windows[2]), .15, "S");
      console.log(`${rate} Hz native=${JSON.stringify(native)}; reference I=${integrated}, LRA=${range}, TP=${peak}`);
    } finally {
      rmSync(directory, { recursive: true, force: true });
    }
  });
}
