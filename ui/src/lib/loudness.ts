import { engineCommand, type CommandResults } from "./protocol-commands.generated";
import { errorText } from "./errors";

export type LoudnessReport = CommandResults["get_loudness_report"]["report"];
export interface LoudnessView {
  report: LoudnessReport | null;
  busy: boolean;
  error: string;
  restarted: boolean;
}
interface Target {
  trackId: number;
  running: boolean;
  sampleRate: number;
  epoch: number;
}
export const emptyLoudnessView = (): LoudnessView => ({ report: null, busy: false, error: "", restarted: false });

/** One owner serializes start/reset/read/stop, including cleanup after a popup is removed. */
export class LoudnessController {
  private target: Target | null = null;
  private measurementId: string | null = null;
  private generation = 0;
  private tail: Promise<void> = Promise.resolve();
  private timer: ReturnType<typeof setTimeout> | undefined;
  private view = emptyLoudnessView();

  constructor(
    private readonly changed: (view: LoudnessView) => void,
    private readonly cleanupFailed: (message: string) => void,
  ) {}

  private publish(patch: Partial<LoudnessView>) {
    this.view = { ...this.view, ...patch };
    this.changed(this.view);
  }

  private cancelPoll() {
    clearTimeout(this.timer);
    this.timer = undefined;
  }

  private enqueue(job: () => Promise<void>) {
    this.tail = this.tail.then(job);
    return this.tail;
  }

  private async stopCurrent() {
    const measurementId = this.measurementId;
    this.measurementId = null;
    if (!measurementId) return;
    try {
      await engineCommand("stop_loudness", { measurementId });
    } catch (error) {
      this.cleanupFailed(`停止響度分析失敗：${errorText(error)}`);
    }
  }

  configure(trackId: number | null, running: boolean, sampleRate: number, epoch: number) {
    const next = trackId === null ? null : { trackId, running, sampleRate, epoch };
    const previous = this.target;
    if (next?.trackId === previous?.trackId && next?.running === previous?.running &&
        next?.sampleRate === previous?.sampleRate && next?.epoch === previous?.epoch) return;
    this.target = next;
    const generation = ++this.generation;
    this.cancelPoll();
    const sameTrack = next !== null && previous !== null && next.trackId === previous.trackId && next.epoch === previous.epoch;
    const frozen: LoudnessReport | null = sameTrack && !next.running && this.view.report
      ? { ...this.view.report, state: this.view.report.state === "error" ? "error" : "stopped" } : null;
    this.publish({ report: frozen, busy: !!next?.running, error: "", restarted: sameTrack && !!next?.running });

    void this.enqueue(async () => {
      if (generation !== this.generation) return;
      // On stop, fetch the engine's final drained snapshot before releasing it.
      if (sameTrack && !next?.running && this.measurementId) {
        try {
          const { report } = await engineCommand("get_loudness_report", { measurementId: this.measurementId });
          if (generation === this.generation) this.publish({ report: { ...report, state: report.state === "error" ? "error" : "stopped" } });
        } catch {
          // The last visible snapshot remains explicitly frozen if the engine has already discarded it.
        }
      }
      await this.stopCurrent();
      if (generation !== this.generation || !next?.running) return;
      try {
        const { report } = await engineCommand("start_loudness", { trackId: next.trackId });
        this.measurementId = report.measurementId;
        if (generation !== this.generation) return;
        this.publish({ report, busy: false });
        this.schedulePoll(generation);
      } catch (error) {
        if (generation === this.generation) this.publish({ busy: false, error: errorText(error) });
      }
    });
  }

  reset() {
    if (!this.target?.running || this.view.busy) return;
    // Also provides an explicit retry after a failed start.
    if (!this.measurementId) {
      const target = this.target;
      this.target = null;
      this.configure(target.trackId, target.running, target.sampleRate, target.epoch);
      return;
    }
    const generation = ++this.generation;
    this.cancelPoll();
    this.publish({ busy: true, error: "" });
    void this.enqueue(async () => {
      if (generation !== this.generation || !this.measurementId) return;
      try {
        const { report } = await engineCommand("reset_loudness", { measurementId: this.measurementId });
        this.measurementId = report.measurementId;
        if (generation === this.generation) this.publish({ report, busy: false, restarted: false });
      } catch (error) {
        if (generation === this.generation) this.publish({ busy: false, error: errorText(error) });
      }
      if (generation === this.generation) this.schedulePoll(generation);
    });
  }

  private schedulePoll(generation: number) {
    if (!this.target?.running || this.view.report?.state !== "measuring") return;
    this.timer = setTimeout(() => {
      void this.enqueue(async () => {
        if (generation !== this.generation || !this.measurementId) return;
        const measurementId = this.measurementId;
        try {
          const { report } = await engineCommand("get_loudness_report", { measurementId });
          if (generation !== this.generation || report.measurementId !== measurementId) return;
          this.publish({ report, error: "" });
          this.schedulePoll(generation);
        } catch (error) {
          if (generation === this.generation) this.publish({ error: errorText(error) });
        }
      });
    }, 200);
  }

  close() {
    this.configure(null, false, 0, 0);
    return this.tail;
  }
}

export function loudnessValue(report: LoudnessReport | null, field: keyof Pick<LoudnessReport,
  "momentaryLufs" | "shortTermLufs" | "integratedLufs" | "truePeakDbtp" | "loudnessRangeLu">): string {
  if (!report || report.state === "error") return "—";
  const value = report[field];
  if (value !== null && Number.isFinite(value)) return value.toFixed(1);
  const minimum = field === "truePeakDbtp" ? 0 :
    field === "shortTermLufs" || field === "loudnessRangeLu" ? 3 : 0.4;
  if (report.elapsedSeconds <= 0 || report.elapsedSeconds < minimum ||
      field === "loudnessRangeLu" || field === "integratedLufs") return "—";
  return "−∞";
}

export function loudnessDuration(seconds: number): string {
  const total = Math.max(0, Math.floor(seconds));
  return `${Math.floor(total / 60).toString().padStart(2, "0")}:${(total % 60).toString().padStart(2, "0")}`;
}
