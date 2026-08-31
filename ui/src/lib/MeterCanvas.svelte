<script lang="ts">
  import type { MeterStrip } from "./types";

  let {
    strip,
    width = 46,
  }: { strip: MeterStrip | undefined; width?: number } = $props();

  let canvas: HTMLCanvasElement | undefined = $state();

  const FLOOR_DB = -60;
  const WARNING_DB = -6;
  const NORMAL_COLOR = "#39c978";
  const WARNING_COLOR = "#f0a33a";
  const CLIP_COLOR = "#ef5350";
  const TICK_COLOR = "#8a93a3";

  // prop → 非響應式槽(主 rAF effect 不依賴 strip,45Hz 更新不重啟 rAF 迴圈)
  let latest: MeterStrip | undefined;
  $effect(() => {
    latest = strip;
  });

  // 顯示端狀態(rAF 內更新,不進 $state — 60fps 不值得觸發響應式)
  const disp = {
    peakL: FLOOR_DB,
    peakR: FLOOR_DB,
    rmsL: FLOOR_DB,
    rmsR: FLOOR_DB,
  };
  // 0 dBFS 過載鎖定；只由使用者右鍵清除，持續過載時會立即重新亮起。
  const clipHold = { left: false, right: false };

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
      const peakL = latest?.peakL ?? 0;
      const peakR = latest?.peakR ?? 0;
      if (peakL >= 1) clipHold.left = true;
      if (peakR >= 1) clipHold.right = true;
      disp.peakL = follow(disp.peakL, db(peakL));
      disp.peakR = follow(disp.peakR, db(peakR));
      disp.rmsL = Math.min(disp.peakL, db(latest?.rmsL ?? 0));
      disp.rmsR = Math.min(disp.peakR, db(latest?.rmsR ?? 0));

      render(ctx);
    };
    raf = requestAnimationFrame(draw);
    return () => cancelAnimationFrame(raf);
  });

  function db(amp: number): number {
    // 顯示上限夾 0 dBFS；是否過載由夾限前的 amplitude 另行鎖定。
    return amp > 0 ? Math.min(0, Math.max(FLOOR_DB, 20 * Math.log10(amp))) : FLOOR_DB;
  }
  // dB → y(0dB 頂、FLOOR_DB 底)
  function y(dbv: number, top: number, plotH: number): number {
    return top + (1 - (dbv - FLOOR_DB) / -FLOOR_DB) * plotH;
  }

  const TICKS = [0, -6, -12, -24, -36, -48, -60];

  function clearClipHold(event: MouseEvent) {
    event.preventDefault();
    event.stopPropagation();
    clipHold.left = false;
    clipHold.right = false;
  }

  function fillLevel(
    ctx: CanvasRenderingContext2D,
    x: number,
    barW: number,
    level: number,
    top: number,
    plotH: number,
  ) {
    const levelY = y(level, top, plotH);
    const warningY = y(WARNING_DB, top, plotH);
    const bottom = top + plotH;

    // 正常區：-60 至 -6 dBFS。
    const normalTop = Math.max(levelY, warningY);
    if (normalTop < bottom) {
      ctx.fillStyle = NORMAL_COLOR;
      ctx.fillRect(x, normalTop, barW, bottom - normalTop);
    }

    // 接近峰值區：-6 至 0 dBFS；真正過載另以頂端紅線表示。
    if (levelY < warningY) {
      ctx.fillStyle = WARNING_COLOR;
      ctx.fillRect(x, levelY, barW, warningY - levelY);
    }
  }

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
      { x: 0, rms: disp.rmsL, peak: disp.peakL, clipped: clipHold.left },
      { x: barW + gap, rms: disp.rmsR, peak: disp.peakR, clipped: clipHold.right },
    ];
    for (const b of bars) {
      // RMS 主錶依區間著色。
      fillLevel(ctx, b.x, barW, b.rms, top, plotH);

      // 瞬時 peak 保留衰減，以亮線顯示目前最高位置。
      if (b.peak > FLOOR_DB) {
        const py = y(b.peak, top, plotH);
        ctx.fillStyle = b.peak >= WARNING_DB ? WARNING_COLOR : NORMAL_COLOR;
        ctx.fillRect(b.x, Math.round(py), barW, 2);
      }

      // 只有實際達到 0 dBFS 才鎖定紅線；右鍵可清除。
      if (b.clipped) {
        ctx.fillStyle = CLIP_COLOR;
        ctx.fillRect(b.x, top, barW, 3);
      }
    }

    // dB 刻度:短 tick + 數字(中線對位,線不橫穿數字)
    ctx.font = "9px monospace";
    ctx.textBaseline = "middle";
    for (const v of TICKS) {
      const ty = Math.round(y(v, top, plotH));
      ctx.fillStyle = TICK_COLOR;
      ctx.fillRect(plotW, ty, 4, 1);
      ctx.fillText(String(v), plotW + 6, ty);
    }
  }
</script>

<canvas
  bind:this={canvas}
  style="width:{width}px; height:100%; display:block"
  oncontextmenu={clearClipHold}
  aria-label="立體聲音頻錶；綠色為正常、橘色接近峰值、紅線為已過載。右鍵清除過載狀態。"
  data-tooltip="音頻錶：綠色為正常、橘色接近峰值、紅線表示曾經過載；按右鍵可清除 peak 狀態。"
></canvas>
