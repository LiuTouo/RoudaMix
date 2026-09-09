import { storyState } from './story.ts';

/** All causal motion is a pure function of scroll: reverse and jump need no replay. */
export function segment(value: number, start: number, end: number) {
  const t = Math.max(0, Math.min(1, (value - start) / (end - start)));
  return t * t * (3 - 2 * t);
}

export function workflowMotion(progress: number) {
  const state = storyState(progress);
  const time = state.progress * 7;
  const ramp = (start: number, end: number) => segment(time, start, end);
  const window = (start: number, peak: number, hold: number, end: number) => ramp(start, peak) * (1 - ramp(hold, end));
  return {
    ...state,
    launch: ramp(0, 0.72),
    sources: [ramp(0, 0.23), ramp(0.08, 0.34), ramp(0.18, 0.46)],
    deviceMenu: window(1.04, 1.13, 1.29, 1.39),
    deviceSelected: ramp(1.21, 1.33),
    inputReveal: ramp(1.39, 1.62),
    pluginMenu: window(2.04, 2.16, 2.36, 2.54),
    pluginInsert: ramp(2.38, 2.61),
    monitorRoute: ramp(3.10, 3.35),
    streamRoute: ramp(3.37, 3.65),
    bypassMix: ramp(4.22, 4.53),
    obsReveal: ramp(5.02, 5.21),
    cableRoute: ramp(5.20, 5.62),
    obsMenu: window(5.14, 5.24, 5.44, 5.58),
    voiceReveal: ramp(6.08, 6.34),
    voiceRoute: ramp(6.32, 6.60),
    focus: window(state.stage + 0.02, state.stage + 0.19, state.stage + 0.66, state.stage + 0.94),
    // Pointer windows include approach, press ripple, then withdrawal.
    devicePointer: (time - 1.10) / 0.28,
    inputPointer: (time - 1.38) / 0.27,
    pluginPointer: (time - 2.16) / 0.37,
    monitorPointer: (time - 3.03) / 0.31,
    streamPointer: (time - 3.33) / 0.31,
    bypassPointer: (time - 4.10) / 0.40,
    obsPointer: (time - 5.24) / 0.31,
    voicePointer: (time - 6.24) / 0.32,
  };
}
