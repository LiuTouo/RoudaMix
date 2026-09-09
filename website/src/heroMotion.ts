import { workflowMotion } from './workflowMotion';

/** The opening scene demonstrates a complete input → effect → two-output path. */
export function heroMotion(phase: number) {
  const preview = workflowMotion(phase * 3.9 / 7);
  return { ...preview, stage: 0, local: phase, progress: phase / 7 };
}
