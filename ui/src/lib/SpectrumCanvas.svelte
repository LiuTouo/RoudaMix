<script lang="ts">
  // 主輸出頻譜 bar(對數頻率軸)。資料 = SHM 30Hz dB bins(線性 0..Nyquist),
  // 這裡做 log-freq 映射 + rAF 繪圖。64 bar × 60fps 用 2D canvas 就夠 — GL 版本
  // 已移除(兩套渲染路徑的維護成本 > 收益)。刻度:100Hz/1k/10k 直線 +
  // -20/-40/-60 dB 橫線,頂 = 0 dBFS 滿幅。
  import { onMount } from "svelte";

  let {
    spectrum,
    sampleRate,
    height = 120,
  }: {
    spectrum: number[] | null;
    sampleRate: number;
    height?: number;
  } = $props();

  const BARS = 64;
  const MIN_FREQ = 20;
  const DB_FLOOR = -60; // 顯示下限(SHM floor -120,視覺壓到 -60)
  const FREQ_MARKS = [100, 1000, 10000];
  const DB_MARKS = [-20, -40, -60];
  const LABEL_COLOR = "#8a93a3";

  let canvas: HTMLCanvasElement | null = null;

  // rAF 讀的最新資料(prop 更新只寫這裡,繪圖統一在 rAF,避免每 event 一次 draw)
  let latest: number[] | null = $state(null);
  let latestRate = 0;
  let barVals = new Float32Array(BARS); // 平滑後 0..1

  // 頻譜 bin(線性)→ 每 bar 的 [lo, hi) bin 範圍(log-freq)。bin 數/率變動才重算
  let mapBins: Array<[number, number]> = [];
  let mappedBins = -1;
  let mappedRate = 0;

  // 靜態層(底色、格線、刻度+數字):尺寸/取樣率變才重繪,每幀 drawImage 貼回
  let staticLayer: HTMLCanvasElement | null = null;
  let staticKey = "";
  // 上一幀繪製指紋:量化 bar 高 + 尺寸;相同 → 跳過本幀(靜音時停畫)
  let lastKey = "";

  $effect(() => {
    latest = spectrum;
    latestRate = sampleRate;
    // 不在這裡清 barVals:空資料的緩降交給 sampleBars 的 decay 路徑,
    // 這裡 fill(0) 會殺掉 ballistics(bar 瞬間歸零而非緩降)
  });

  onMount(() => {
    if (!canvas) return;
    const ctx = canvas.getContext("2d");
    if (!ctx) return;
    let raf = requestAnimationFrame(draw);
    return () => cancelAnimationFrame(raf);

    function draw() {
      if (!canvas) return;
      sizeCanvas();
      render(ctx!);
      raf = requestAnimationFrame(draw);
    }
  });

  // 尺寸/DPR:每幀比對,變了才重設 canvas.width(重設會清 drawing buffer,
  // 下一個 draw 全重畫,無所謂)
  function sizeCanvas() {
    if (!canvas) return;
    const dpr = window.devicePixelRatio || 1;
    const cw = Math.round(canvas.clientWidth * dpr);
    const ch = Math.round(canvas.clientHeight * dpr);
    if (cw > 0 && ch > 0 && (canvas.width !== cw || canvas.height !== ch)) {
      canvas.width = cw;
      canvas.height = ch;
    }
  }

  function rebuildMap(bins: number, rate: number) {
    const nyquist = rate / 2;
    mapBins = [];
    for (let b = 0; b < BARS; b++) {
      const f0 = MIN_FREQ * Math.pow(nyquist / MIN_FREQ, b / BARS);
      const f1 = MIN_FREQ * Math.pow(nyquist / MIN_FREQ, (b + 1) / BARS);
      const i0 = Math.min(bins - 1, Math.floor((f0 / nyquist) * bins));
      const i1 = Math.max(i0 + 1, Math.min(bins, Math.ceil((f1 / nyquist) * bins)));
      mapBins.push([i0, i1]);
    }
    mappedBins = bins;
  }

  function sampleBars(): Float32Array {
    const out = barVals;
    const data = latest;
    if (!data || data.length === 0 || latestRate <= 0) {
      for (let b = 0; b < BARS; b++) out[b] *= 0.8; // 資料停了就緩降
      return out;
    }
    if (mappedBins !== data.length || mappedRate !== latestRate) {
      rebuildMap(data.length, latestRate); // 率變了(硬體面板換率)log-freq 映射也要重算
      mappedBins = data.length;
    }
    for (let b = 0; b < BARS; b++) {
      const [i0, i1] = mapBins[b];
      let peak = -120;
      for (let i = i0; i < i1 && i < data.length; i++) if (data[i] > peak) peak = data[i];
      const v = Math.max(0, Math.min(1, (peak - DB_FLOOR) / -DB_FLOOR));
      // 上升即時、下降緩(ballistics;30Hz 資料 + 60Hz 繪圖)
      out[b] = v > out[b] ? v : out[b] * 0.86 + v * 0.14;
    }
    return out;
  }

  // bar 顏色:高度漸層(綠 → 黃 → 紅)
  function barColor(v: number): [number, number, number] {
    if (v < 0.5) {
      const t = v / 0.5;
      return [0.16 + 0.5 * t, 0.62 + 0.28 * t, 0.25];
    }
    const t = (v - 0.5) / 0.5;
    return [0.66 + 0.34 * t, 0.9 - 0.5 * t, 0.25 - 0.13 * t];
  }

  function render(ctx: CanvasRenderingContext2D) {
    const dpr = window.devicePixelRatio || 1;
    const w = canvas!.clientWidth;
    const h = canvas!.clientHeight;
    const plotH = h - 12; // 底部留給頻率標籤

    // 未變幀跳過:量化 bar 高(px)+ 尺寸,與上一幀相同 → 零繪製
    let key = `${w}x${h}@${dpr}`;
    for (let b = 0; b < BARS; b++) key += "," + Math.round(barVals[b] * plotH);
    if (key === lastKey) return;
    lastKey = key;

    ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
    ctx.clearRect(0, 0, w, h);
    ctx.drawImage(staticLayerFor(w, h, dpr), 0, 0, w, h);

    // bars:對數軸均勻切 64 段,bar b 的左緣恰 = b/BARS × w
    const bw = w / BARS;
    for (let b = 0; b < BARS; b++) {
      const bh = barVals[b] * plotH;
      if (bh < 1) continue;
      const [r, g, bl] = barColor(barVals[b]);
      ctx.fillStyle = `rgb(${(r * 255) | 0},${(g * 255) | 0},${(bl * 255) | 0})`;
      ctx.fillRect(b * bw + bw * 0.15, plotH - bh, bw * 0.7, bh);
    }
  }

  function staticLayerFor(w: number, h: number, dpr: number): HTMLCanvasElement {
    const key = `${w}x${h}@${dpr}#${latestRate}`;
    if (staticLayer && staticKey === key) return staticLayer;
    const c = document.createElement("canvas");
    c.width = Math.max(1, Math.round(w * dpr));
    c.height = Math.max(1, Math.round(h * dpr));
    const s = c.getContext("2d")!;
    s.setTransform(dpr, 0, 0, dpr, 0, 0);
    const plotH = h - 12;

    s.fillStyle = "#0e0f12";
    s.fillRect(0, 0, w, h);

    // dB 橫線 + 右側標籤(頂 = 0 dBFS 滿幅,不畫 0 線以免跟頂緣混淆)
    s.font = "9px monospace";
    s.textBaseline = "middle";
    for (const db of DB_MARKS) {
      const y = Math.round((db / DB_FLOOR) * plotH); // db/-60 → 0..1 比例
      s.fillStyle = "rgba(255, 255, 255, 0.06)";
      s.fillRect(0, y, w, 1);
      s.fillStyle = LABEL_COLOR;
      s.fillText(String(db), w - 26, y);
    }

    // 頻率直線 + 底部標籤(與 bar 同一對數映射:x = log(f/20)/log(nyq/20) × w)
    const nyquist = latestRate > 0 ? latestRate / 2 : 0;
    s.fillStyle = LABEL_COLOR;
    s.fillText("20", 3, h - 6);
    if (nyquist > MIN_FREQ) {
      for (const f of FREQ_MARKS) {
        if (f >= nyquist) continue;
        const x = Math.round((Math.log(f / MIN_FREQ) / Math.log(nyquist / MIN_FREQ)) * w);
        s.fillStyle = "rgba(255, 255, 255, 0.06)";
        s.fillRect(x, 0, 1, plotH);
        s.fillStyle = LABEL_COLOR;
        s.fillText(f >= 1000 ? `${f / 1000}k` : String(f), x + 3, h - 6);
      }
    }
    staticLayer = c;
    staticKey = key;
    return c;
  }
</script>

<canvas
  bind:this={canvas}
  style="width:100%;height:{height}px;display:block;border-radius:6px"
  data-tooltip="主輸出頻譜:橫軸頻率(對數,20Hz 起,左低音右高音),縱軸音量 dB(頂=滿幅)。bar 越高代表該頻段越響。"
></canvas>
