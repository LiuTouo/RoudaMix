<script lang="ts">
  import type { MeterStrip } from "./types";

  let {
    strip,
    width = 46,
  }: { strip: MeterStrip | undefined; width?: number } = $props();

  let canvas: HTMLCanvasElement | undefined = $state();

  // prop → 非響應式槽(主 rAF effect 不依賴 strip,45Hz 更新不重啟 rAF 迴圈)
  let latest: MeterStrip | undefined;
  $effect(() => {
    latest = strip;
  });

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
      disp.peakL = follow(disp.peakL, db(latest?.peakL ?? 0));
      disp.peakR = follow(disp.peakR, db(latest?.peakR ?? 0));
      disp.rmsL = Math.min(disp.peakL, db(latest?.rmsL ?? 0));
      disp.rmsR = Math.min(disp.peakR, db(latest?.rmsR ?? 0));

      render(ctx);
    };
    raf = requestAnimationFrame(draw);
    return () => cancelAnimationFrame(raf);
  });

  const FLOOR_DB = -60;
  function db(amp: number): number {
    // 上限夾 0dB:超過 1.0 的 peak 釘在頂,不會畫過紅色 0dB 標
    return amp > 0 ? Math.min(0, Math.max(FLOOR_DB, 20 * Math.log10(amp))) : FLOOR_DB;
  }
  // dB → y(0dB 頂、FLOOR_DB 底)
  function y(dbv: number, top: number, plotH: number): number {
    return top + (1 - (dbv - FLOOR_DB) / -FLOOR_DB) * plotH;
  }

  const TICKS = [0, -6, -12, -24, -36, -48, -60];

  function render(ctx: CanvasRenderingContext2D) {
    const dpr = window.devicePixelRatio || 1;
    const w = canvas!.clientWidth;
    const h = canvas!.clientHeight;
    if (canvas!.width !== w * dpr || canvas!.height !== h * dpr) {
      canvas!.width = w * dpr;
      canvas!.height = h * dpr;
    }
    ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
    ctx.clearRect(0, 0, w, h);

    const gutter = 24; // 右側 dB 刻度
    const plotW = w - gutter;
    if (plotW < 4) return;
    const gap = 3;
    const barW = Math.floor((plotW - gap) / 2);
    const top = 5;
    const plotH = h - 10; // 上下留白:0/-60 的 tick 與數字不裁切

    // 軌底 + 淡格線(每個 tick 一條,讀數好對位)
    for (const b of [
      { x: 0 },
      { x: barW + gap },
    ]) {
      ctx.fillStyle = "#171a1f";
      ctx.fillRect(b.x, top, barW, plotH);
    }
    ctx.fillStyle = "rgba(255, 255, 255, 0.06)";
    for (const v of TICKS) ctx.fillRect(0, Math.round(y(v, top, plotH)), plotW, 1);

    const bars = [
      { x: 0, rms: disp.rmsL, peak: disp.peakL },
      { x: barW + gap, rms: disp.rmsR, peak: disp.peakR },
    ];
    for (const b of bars) {
      // rms(亮)
      const ry = y(b.rms, top, plotH);
      ctx.fillStyle = "#4da3ff";
      ctx.fillRect(b.x, ry, barW, top + plotH - ry);
      // peak(半透明疊)
      const py = y(b.peak, top, plotH);
      ctx.fillStyle = "rgba(255, 255, 255, 0.22)";
      ctx.fillRect(b.x, py, barW, top + plotH - py);
      // 過載區標記 0 dB(頂端)
      ctx.fillStyle = "#e2533b";
      ctx.fillRect(b.x, top, barW, 2);
    }

    // dB 刻度:短 tick + 數字(中線對位,線不橫穿數字)
    ctx.font = "9px monospace";
    ctx.textBaseline = "middle";
    for (const v of TICKS) {
      const ty = Math.round(y(v, top, plotH));
      const col = v === 0 ? "#e2533b" : "#8a93a3";
      ctx.fillStyle = col;
      ctx.fillRect(plotW, ty, 4, 1);
      ctx.fillText(String(v), plotW + 6, ty);
    }
  }
</script>

<canvas bind:this={canvas} style="width:{width}px; height:100%; display:block"></canvas>
