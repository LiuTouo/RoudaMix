<script lang="ts">
  // 旋鈕:angular drag(繞中心)、dblclick 回 default。value = normalized [0,1]。
  let {
    label,
    value = 0,
    def = 0,
    size = 44,
    onChange,
  }: {
    label: string;
    value?: number;
    def?: number;
    size?: number;
    onChange: (v: number) => void;
  } = $props();

  let svg: SVGSVGElement | undefined = $state();

  const A0 = -135; // 起始角(度)
  const SWEEP = 270;
  const R = 15.5;
  const C = 18; // viewBox 36x36

  function pt(deg: number, r: number): [number, number] {
    const a = ((deg - 90) * Math.PI) / 180;
    return [C + r * Math.cos(a), C + r * Math.sin(a)];
  }
  const [x0, y0] = pt(A0, R);
  const [x1, y1] = pt(A0 + SWEEP, R);
  const large = SWEEP > 180 ? 1 : 0;

  const angle = $derived(A0 + SWEEP * Math.min(1, Math.max(0, value)));
  const [px, py] = $derived(pt(angle, R - 3));
  const [ix, iy] = $derived(pt(angle, R - 8.5));

  function send(v: number) {
    onChange(Math.min(1, Math.max(0, v)));
  }

  function angleFromEvent(e: PointerEvent | MouseEvent): number {
    const r = svg!.getBoundingClientRect();
    const dx = e.clientX - (r.left + r.width / 2);
    const dy = e.clientY - (r.top + r.height / 2);
    let a = (Math.atan2(dx, -dy) * 180) / Math.PI; // 上 = 0°
    if (a < A0) a = Math.abs(a - A0) < 360 - SWEEP - Math.abs(a - A0) ? A0 : A0 + SWEEP;
    if (a > A0 + SWEEP) a = A0 + SWEEP;
    return a;
  }

  function down(e: PointerEvent) {
    e.preventDefault();
    svg!.setPointerCapture(e.pointerId);
    send((angleFromEvent(e) - A0) / SWEEP);
  }
  function move(e: PointerEvent) {
    if (!(e.buttons & 1)) return;
    send((angleFromEvent(e) - A0) / SWEEP);
  }
  function dbl() {
    send(def);
  }
  function wheel(e: WheelEvent) {
    e.preventDefault();
    send(value - Math.sign(e.deltaY) * 0.02);
  }
</script>

<div class="knob">
  <svg
    bind:this={svg}
    viewBox="0 0 36 36"
    width={size}
    height={size}
    onpointerdown={down}
    onpointermove={move}
    ondblclick={dbl}
    onwheel={wheel}
    role="slider"
    aria-label={label}
    aria-valuemin={0}
    aria-valuemax={100}
    aria-valuenow={Math.round(value * 100)}
    tabindex="0"
  >
    <path class="track" d="M {x0} {y0} A {R} {R} 0 {large} 1 {x1} {y1}" />
    <circle class="body" cx={C} cy={C} r={R - 4.5} />
    <line x1={ix} y1={iy} x2={px} y2={py} />
  </svg>
  <span class="v mono">{(value * 100).toFixed(0)}%</span>
  <span class="l" title={label}>{label}</span>
</div>

<style>
  .knob {
    display: flex;
    flex-direction: column;
    align-items: center;
    gap: 2px;
    width: 76px;
  }
  svg {
    cursor: grab;
    touch-action: none;
  }
  svg:active {
    cursor: grabbing;
  }
  svg:focus-visible {
    outline: 1px solid var(--accent);
    border-radius: 50%;
  }
  .track {
    fill: none;
    stroke: var(--border);
    stroke-width: 2.5;
  }
  .body {
    fill: var(--bg-raised);
    stroke: var(--border);
  }
  line {
    stroke: var(--accent);
    stroke-width: 2;
    stroke-linecap: round;
  }
  .v {
    font-size: 11px;
    color: var(--text-dim);
  }
  .l {
    font-size: 11px;
    max-width: 76px;
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
    text-align: center;
  }
</style>
