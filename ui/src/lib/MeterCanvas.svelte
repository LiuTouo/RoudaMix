<script lang="ts">
  import type { MeterStrip } from "./types";

  let {
    strip,
    height = 64,
  }: { strip: MeterStrip | undefined; height?: number } = $props();

  let canvas: HTMLCanvasElement | undefined = $state();

  // 顯示端狀態(rAF 內更新,不進 $state — 60fps 不值得觸發響應式)
  const disp = { peakL: 0, peakR: 0, rmsL: 0, rmsR: 0 };

  $effect(() => {
    if (!canvas) return;
    const ctx = canvas.getContext("2d");
    if (!ctx) return;
    let raf = 0;
    let last = performance.now();

    const draw = (now: number) => {
      raf = requestAnimationFrame(draw);
      const dt = (now - last) / 1000;
      last = now;

      // 新值進來瞬間跳上,之後 12 dB/s 衰減
      const follow = (cur: number, target: number) => Math.max(target, cur - 12 * dt);
      disp.peakL = follow(disp.peakL, db(strip?.peakL ?? 0));
      disp.peakR = follow(disp.peakR, db(strip?.peakR ?? 0));
      disp.rmsL = Math.min(disp.peakL, db(strip?.rmsL ?? 0));
      disp.rmsR = Math.min(disp.peakR, db(strip?.rmsR ?? 0));

      render(ctx);
    };
    raf = requestAnimationFrame(draw);
    return () => cancelAnimationFrame(raf);
  });

  const FLOOR_DB = -60;
  function db(amp: number): number {
    return amp > 0 ? Math.max(FLOOR_DB, 20 * Math.log10(amp)) : FLOOR_DB;
  }
  function x(dbv: number, w: number): number {
    return ((dbv - FLOOR_DB) / -FLOOR_DB) * w;
  }

  function render(ctx: CanvasRenderingContext2D) {
    const dpr = window.devicePixelRatio || 1;
    const w = canvas!.clientWidth;
    const h = canvas!.clientHeight;
    if (canvas!.width !== w * dpr) {
      canvas!.width = w * dpr;
      canvas!.height = h * dpr;
    }
    ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
    ctx.clearRect(0, 0, w, h);

    const compact = h < 40;
    const rowH = h / 2 - 3;
    const rows = [
      { y: 0, peak: disp.peakL, rms: disp.rmsL, label: "L" },
      { y: rowH + 6, peak: disp.peakR, rms: disp.rmsR, label: "R" },
    ];
    ctx.font = "10px monospace";
    for (const r of rows) {
      const left = compact ? 0 : 14;
      const width = w - left;
      // 軌底
      ctx.fillStyle = "#1c2026";
      ctx.fillRect(left, r.y, width, rowH);
      // rms(亮)
      ctx.fillStyle = "#4da3ff";
      ctx.fillRect(left, r.y, Math.min(x(r.rms, width), width), rowH);
      // peak(半透明疊)
      ctx.fillStyle = "rgba(255, 255, 255, 0.22)";
      ctx.fillRect(left, r.y, Math.min(x(r.peak, width), width), rowH);
      // 過載區標記 0 dB
      ctx.fillStyle = "#e2533b";
      ctx.fillRect(w - 3, r.y, 3, rowH);
      if (!compact) {
        ctx.fillStyle = "#8a93a3";
        ctx.fillText(r.label, 2, r.y + rowH - 2);
      }
    }
  }
</script>

<canvas bind:this={canvas} style="width:100%; height:{height}px; display:block"></canvas>
