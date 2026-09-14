<script lang="ts">
  // WebGL 頻譜 bar(對數頻率軸)。資料 = SHM 30Hz dB bins(線性 0..Nyquist),
  // 這裡做 log-freq 映射 + rAF 繪圖;WebGL 拿不到時退 2D canvas(WebView2 常在)
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

  let canvas: HTMLCanvasElement | null = null;
  let gl: WebGLRenderingContext | null = null;
  let gl2d: CanvasRenderingContext2D | null = null;
  let vbo: WebGLBuffer | null = null;
  let cbo: WebGLBuffer | null = null;

  // rAF 讀的最新資料(prop 更新只寫這裡,繪圖統一在 rAF,避免每 event 一次 draw)
  let latest: number[] | null = $state(null);
  let latestRate = 0;
  let barVals = new Float32Array(BARS); // 平滑後 0..1

  // 頻譜 bin(線性)→ 每 bar 的 [lo, hi) bin 範圍(log-freq)。bin 數/率變動才重算
  let mapBins: Array<[number, number]> = [];
  let mappedBins = -1;
  let mappedRate = 0;

  $effect(() => {
    latest = spectrum;
    latestRate = sampleRate;
    // 不在這裡清 barVals:空資料的緩降交給 sampleBars 的 decay 路徑,
    // 這裡 fill(0) 會殺掉 ballistics(bar 瞬間歸零而非緩降)
  });

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

  onMount(() => {
    if (!canvas) return;
    gl = canvas.getContext("webgl", { antialias: false });
    if (gl) initGl(gl);
    else gl2d = canvas.getContext("2d");
    let raf = requestAnimationFrame(draw);
    return () => cancelAnimationFrame(raf);

    function draw() {
      if (!canvas) return;
      if (gl) drawGl();
      else if (gl2d) draw2d(gl2d);
      raf = requestAnimationFrame(draw);
    }
  });

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

  function initGl(g: WebGLRenderingContext) {
    const vs = g.createShader(g.VERTEX_SHADER)!;
    g.shaderSource(vs, "attribute vec2 p; attribute vec3 c; varying vec3 vc; void main(){ gl_Position = vec4(p,0.0,1.0); vc = c; }");
    g.compileShader(vs);
    const fs = g.createShader(g.FRAGMENT_SHADER)!;
    g.shaderSource(fs, "precision mediump float; varying vec3 vc; void main(){ gl_FragColor = vec4(vc,1.0); }");
    g.compileShader(fs);
    const prog = g.createProgram()!;
    g.attachShader(prog, vs);
    g.attachShader(prog, fs);
    g.linkProgram(prog);
    g.useProgram(prog);
    vbo = g.createBuffer();
    g.bindBuffer(g.ARRAY_BUFFER, vbo);
    const pLoc = g.getAttribLocation(prog, "p");
    g.enableVertexAttribArray(pLoc);
    g.vertexAttribPointer(pLoc, 2, g.FLOAT, false, 0, 0);
    cbo = g.createBuffer();
    g.bindBuffer(g.ARRAY_BUFFER, cbo);
    const cLoc = g.getAttribLocation(prog, "c");
    g.enableVertexAttribArray(cLoc);
    g.vertexAttribPointer(cLoc, 3, g.FLOAT, false, 0, 0);
  }

  function drawGl() {
    const g = gl!;
    const w = canvas!.width, h = canvas!.height;
    g.viewport(0, 0, w, h);
    g.clearColor(0.055, 0.06, 0.07, 1);
    g.clear(g.COLOR_BUFFER_BIT);
    const vals = sampleBars();
    const verts = new Float32Array(BARS * 8);
    const colors = new Float32Array(BARS * 12);
    const bw = 2 / BARS;
    for (let b = 0; b < BARS; b++) {
      const x = -1 + b * bw;
      const y = vals[b] * 2 - 1;
      const o = b * 8;
      verts[o] = x + 0.15 * bw; verts[o + 1] = -1;
      verts[o + 2] = x + 0.85 * bw; verts[o + 3] = -1;
      verts[o + 4] = x + 0.85 * bw; verts[o + 5] = y;
      verts[o + 6] = x + 0.15 * bw; verts[o + 7] = y;
      const [r, gg, bl] = barColor(vals[b]);
      const co = b * 12;
      for (let k = 0; k < 4; k++) {
        colors[co + k * 3] = r;
        colors[co + k * 3 + 1] = gg;
        colors[co + k * 3 + 2] = bl;
      }
    }
    g.bindBuffer(g.ARRAY_BUFFER, vbo);
    g.bufferData(g.ARRAY_BUFFER, verts, g.DYNAMIC_DRAW);
    g.bindBuffer(g.ARRAY_BUFFER, cbo);
    g.bufferData(g.ARRAY_BUFFER, colors, g.DYNAMIC_DRAW);
    // 每 bar 4 頂點(bl,br,tr,tl)的 strip;TRIANGLES 需 6 頂點會越界讀 buffer =
    // GL_INVALID_OPERATION、整批 draw 被丟棄(實測踩過)
    g.drawArrays(g.TRIANGLE_STRIP, 0, BARS * 4);
  }

  function draw2d(c2d: CanvasRenderingContext2D) {
    const w = canvas!.width, h = canvas!.height;
    c2d.fillStyle = "#0e0f12";
    c2d.fillRect(0, 0, w, h);
    const vals = sampleBars();
    const bw = w / BARS;
    for (let b = 0; b < BARS; b++) {
      const bh = vals[b] * h;
      const [r, g, bl] = barColor(vals[b]);
      c2d.fillStyle = `rgb(${(r * 255) | 0},${(g * 255) | 0},${(bl * 255) | 0})`;
      c2d.fillRect(b * bw + bw * 0.15, h - bh, bw * 0.7, bh);
    }
  }
</script>

<canvas
  bind:this={canvas}
  width={BARS * 8}
  {height}
  style="width:100%;height:{height}px"
  data-tooltip="主輸出頻譜；頻率軸採對數刻度（20 Hz 起），引擎 FFT 資料自 30 Hz 起。"
></canvas>
