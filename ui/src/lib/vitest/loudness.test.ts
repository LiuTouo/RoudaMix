import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";
const { command } = vi.hoisted(() => ({ command: vi.fn() }));
vi.mock("../protocol-commands.generated", async (importOriginal) => ({
  ...await importOriginal<typeof import("../protocol-commands.generated")>(), engineCommand: command,
}));
import { LoudnessController, emptyLoudnessView, loudnessDuration, loudnessValue, type LoudnessReport } from "../loudness";

const report = (measurementId = "one", trackId = 1): LoudnessReport => ({
  measurementId, trackId, state: "measuring", error: null, elapsedSeconds: 0,
  momentaryLufs: null, shortTermLufs: null, integratedLufs: null, truePeakDbtp: null, loudnessRangeLu: null,
});
function deferred<T>() {
  let resolve!: (value: T) => void;
  let reject!: (error: unknown) => void;
  const promise = new Promise<T>((yes, no) => { resolve = yes; reject = no; });
  return { promise, resolve, reject };
}
async function settle() { for (let i = 0; i < 15; i++) await Promise.resolve(); }
beforeEach(() => { vi.useFakeTimers(); command.mockReset(); });
afterEach(() => vi.useRealTimers());
function setup() {
  let view = emptyLoudnessView();
  const cleanupFailed = vi.fn();
  const controller = new LoudnessController((next) => { view = next; }, cleanupFailed);
  command.mockImplementation((kind: string, payload: { trackId?: number; measurementId?: string }) =>
    Promise.resolve(kind === "stop_loudness" ? {} : { report: report(payload.measurementId ?? "one", payload.trackId ?? 1) }));
  return { controller, cleanupFailed, view: () => view };
}

describe("single loudness measurement owner", () => {
  it("does not run without a selected track or while the engine is stopped", async () => {
    const { controller } = setup();
    controller.configure(null, true, 48000, 1);
    controller.configure(1, false, 48000, 1);
    await settle();
    await vi.advanceTimersByTimeAsync(1000);
    expect(command).not.toHaveBeenCalled();
    await controller.close();
  });

  it("starts once, polls at 5Hz, and repeated same-track selection preserves integration", async () => {
    const { controller } = setup();
    controller.configure(1, true, 48000, 1);
    await settle();
    controller.configure(1, true, 48000, 1);
    await vi.advanceTimersByTimeAsync(600);
    expect(command.mock.calls.filter(([kind]) => kind === "start_loudness")).toHaveLength(1);
    expect(command.mock.calls.filter(([kind]) => kind === "get_loudness_report")).toHaveLength(3);
    await controller.close();
    const count = command.mock.calls.length;
    await vi.advanceTimersByTimeAsync(1000);
    expect(command).toHaveBeenCalledTimes(count);
    expect(command.mock.calls.at(-1)).toEqual(["stop_loudness", { measurementId: "one" }]);
  });

  it("never overlaps polling requests", async () => {
    const { controller } = setup();
    controller.configure(1, true, 48000, 1);
    await settle();
    const pending = deferred<{ report: LoudnessReport }>();
    command.mockReturnValueOnce(pending.promise);
    await vi.advanceTimersByTimeAsync(2000);
    expect(command).toHaveBeenCalledTimes(2);
    pending.resolve({ report: report() });
    await settle();
    await controller.close();
  });

  it("cleans up a start that completes after the popup closed", async () => {
    const { controller, view } = setup();
    const pending = deferred<{ report: LoudnessReport }>();
    command.mockReturnValueOnce(pending.promise);
    controller.configure(1, true, 48000, 1);
    await settle();
    const closed = controller.close();
    pending.resolve({ report: report("late") });
    await closed;
    expect(command.mock.calls.at(-1)).toEqual(["stop_loudness", { measurementId: "late" }]);
    expect(view().report).toBeNull();
    expect(vi.getTimerCount()).toBe(0);
  });

  it("serializes a rapid track switch behind pending start and stops its exact token", async () => {
    const { controller, view } = setup();
    const pending = deferred<{ report: LoudnessReport }>();
    command.mockReturnValueOnce(pending.promise);
    controller.configure(1, true, 48000, 1);
    await settle();
    controller.configure(2, true, 48000, 1);
    controller.configure(3, true, 48000, 1);
    pending.resolve({ report: report("late", 1) });
    await settle();
    expect(command.mock.calls).toEqual([
      ["start_loudness", { trackId: 1 }], ["stop_loudness", { measurementId: "late" }],
      ["start_loudness", { trackId: 3 }],
    ]);
    expect(view().report?.trackId).toBe(3);
    await controller.close();
  });

  it("ignores late poll data during switch", async () => {
    const { controller, view } = setup();
    controller.configure(1, true, 48000, 1);
    await settle();
    const pending = deferred<{ report: LoudnessReport }>();
    command.mockReturnValueOnce(pending.promise);
    await vi.advanceTimersByTimeAsync(200);
    controller.configure(2, true, 48000, 1);
    pending.resolve({ report: { ...report(), integratedLufs: -12 } });
    await settle();
    expect(view().report?.trackId).toBe(2);
    expect(view().report?.integratedLufs).toBeNull();
    await controller.close();
  });

  it("reset replaces the token and clears all measurement fields", async () => {
    const { controller, view } = setup();
    controller.configure(1, true, 48000, 1);
    await settle();
    command.mockResolvedValueOnce({ report: report("reset") });
    controller.reset();
    await settle();
    expect(view().report).toEqual(report("reset"));
    await controller.close();
    expect(command.mock.calls.at(-1)).toEqual(["stop_loudness", { measurementId: "reset" }]);
  });

  it("cleans up a reset result received after close without repopulating the panel", async () => {
    const { controller, view } = setup();
    controller.configure(1, true, 48000, 1);
    await settle();
    const pending = deferred<{ report: LoudnessReport }>();
    command.mockReturnValueOnce(pending.promise);
    controller.reset();
    await settle();
    const closed = controller.close();
    pending.resolve({ report: report("new-token") });
    await closed;
    expect(view().report).toBeNull();
    expect(command.mock.calls.at(-1)).toEqual(["stop_loudness", { measurementId: "new-token" }]);
  });

  it("preserves measurement after reset fails and allows explicit retry after start fails", async () => {
    const { controller, view } = setup();
    command.mockRejectedValueOnce(new Error("start failed"));
    controller.configure(1, true, 48000, 1);
    await settle();
    expect(view().error).toContain("start failed");
    controller.reset();
    await settle();
    command.mockRejectedValueOnce(new Error("reset failed"));
    controller.reset();
    await settle();
    expect(view().report?.measurementId).toBe("one");
    expect(view().busy).toBe(false);
    expect(view().error).toContain("reset failed");
    await controller.close();
  });

  it("freezes the final snapshot at stop and restarts for engine/sample-rate changes", async () => {
    const { controller, view } = setup();
    controller.configure(1, true, 48000, 1);
    await settle();
    command.mockResolvedValueOnce({ report: { ...report(), elapsedSeconds: 12, integratedLufs: -17 } });
    controller.configure(1, false, 48000, 1);
    await settle();
    expect(view().report?.state).toBe("stopped");
    expect(view().report?.integratedLufs).toBe(-17);
    expect(vi.getTimerCount()).toBe(0);
    controller.configure(1, true, 96000, 1);
    await settle();
    expect(view().restarted).toBe(true);
    expect(view().report?.elapsedSeconds).toBe(0);
    controller.configure(1, true, 96000, 2);
    await settle();
    expect(command.mock.calls.filter(([kind]) => kind === "start_loudness")).toHaveLength(3);
    await controller.close();
  });

  it.each([false, true])("keeps failed measurements invalid after engine stop (previously failed: %s)", async (alreadyFailed) => {
    const { controller, view } = setup();
    controller.configure(1, true, 48000, 1);
    await settle();
    const failed: LoudnessReport = { ...report(), state: "error", error: "overflow", elapsedSeconds: 4 };
    if (alreadyFailed) {
      command.mockResolvedValueOnce({ report: failed });
      await vi.advanceTimersByTimeAsync(200);
      command.mockRejectedValueOnce(new Error("final snapshot unavailable"));
    } else {
      command.mockResolvedValueOnce({ report: failed });
    }
    controller.configure(1, false, 48000, 1);
    if (alreadyFailed) expect(view().report?.state).toBe("error");
    await settle();
    expect(view().report).toEqual(failed);
    expect(loudnessValue(view().report, "momentaryLufs")).toBe("—");
    expect(loudnessValue(view().report, "truePeakDbtp")).toBe("—");
    expect(vi.getTimerCount()).toBe(0);
    await controller.close();
  });

  it("stops polling on measurement or transport error and reports cleanup failure", async () => {
    const { controller, view, cleanupFailed } = setup();
    controller.configure(1, true, 48000, 1);
    await settle();
    command.mockResolvedValueOnce({ report: { ...report(), state: "error", error: "overflow" } });
    await vi.advanceTimersByTimeAsync(1000);
    expect(view().report?.state).toBe("error");
    expect(command).toHaveBeenCalledTimes(2);
    command.mockRejectedValueOnce(new Error("stop failed"));
    await controller.close();
    expect(cleanupFailed).toHaveBeenCalledExactlyOnceWith(expect.stringContaining("stop failed"));
    controller.configure(1, true, 48000, 1);
    await settle();
    command.mockRejectedValueOnce(new Error("connection lost"));
    await vi.advanceTimersByTimeAsync(1000);
    expect(view().error).toContain("connection lost");
    expect(vi.getTimerCount()).toBe(0);
    await controller.close();
  });
});

describe("loudness display values", () => {
  it("distinguishes warmup, silence and unavailable gated statistics", () => {
    expect(loudnessValue(null, "momentaryLufs")).toBe("—");
    expect(loudnessValue(report(), "momentaryLufs")).toBe("—");
    const silent = { ...report(), elapsedSeconds: 4 };
    expect(loudnessValue(silent, "momentaryLufs")).toBe("−∞");
    expect(loudnessValue(silent, "shortTermLufs")).toBe("−∞");
    expect(loudnessValue(silent, "truePeakDbtp")).toBe("−∞");
    expect(loudnessValue(silent, "integratedLufs")).toBe("—");
    expect(loudnessValue(silent, "loudnessRangeLu")).toBe("—");
    expect(loudnessValue({ ...silent, truePeakDbtp: 1.25 }, "truePeakDbtp")).toBe("1.3");
    expect(loudnessValue({ ...silent, state: "error", momentaryLufs: -20 }, "momentaryLufs")).toBe("—");
    expect(loudnessDuration(3671.9)).toBe("61:11");
  });
});
