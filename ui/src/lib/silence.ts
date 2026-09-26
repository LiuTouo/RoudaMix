// 輸入看門狗(P1-K):麥克風類輸入持續靜音 → 警示。
// 純狀態機,不碰 DOM/Audio;App.svelte 負責餵 level、發通知與驅動嗶聲。
// 判斷只靠 telemetry 已有的 peak,不動 engine/ABI。

/** 數位靜音門檻:peak 低於此 = 裝置實際上送出全零(被靜音/閘門全關/斷訊),
 *  而非「沒說話」—— 房間噪聲再小也會高於此(計無噪聲抑制的原始 WASAPI 擷取) */
export const PEAK_THRESHOLD = 1e-6;
/** 30Hz telemetry × 900 frame = 連續 30 秒全零才警示(說話間的停頓不誤報) */
export const SILENT_FRAMES = 900;
export const SILENT_SECONDS = 30;
/** 有警示時的嗶聲間隔 */
export const BEEP_INTERVAL_MS = 3000;

export type SilenceTransition = "alert" | "clear" | null;

interface SilenceTrackState {
  silent: number; // 連續靜音 frame 數
  alerted: boolean; // 警示中
  suppressed: boolean; // 手動關閉後抑制,直到訊號恢復才重新武裝
}

/** 每軌靜音 debounce(仿 OverloadDetector:只在狀態轉換時回傳通知) */
export class SilenceWatcher {
  private tracks = new Map<number, SilenceTrackState>();
  private threshold: number;
  private frames: number;

  constructor(threshold: number = PEAK_THRESHOLD, frames: number = SILENT_FRAMES) {
    this.threshold = threshold;
    this.frames = frames;
  }

  /**
   * 餵新 frame;level 為該軌 max(peakL, peakR) 線性振幅。
   * 只有實際的數位靜音(裝置送全零)才會累積計數 —— 沒說話但有房間噪聲不算。
   * monitored=false(被靜音/gain 0/無 meter/來源失效)＝不判斷,警示解除、計數歸零。
   * 回傳 "alert"(剛轉為靜音警示)/ "clear"(剛解除)/ null(無轉換)。
   */
  sample(trackId: number, level: number, monitored: boolean): SilenceTransition {
    const st = this.tracks.get(trackId) ?? { silent: 0, alerted: false, suppressed: false };
    this.tracks.set(trackId, st);
    if (!monitored) {
      const wasAlerted = st.alerted;
      st.alerted = false;
      st.silent = 0;
      return wasAlerted ? "clear" : null;
    }
    if (level < this.threshold) {
      st.silent++;
      if (!st.alerted && !st.suppressed && st.silent >= this.frames) {
        st.alerted = true;
        return "alert";
      }
      return null;
    }
    // 有聲音 → 解除警示並重新武裝(手動關閉的抑制也在此解除)
    const wasAlerted = st.alerted || st.suppressed;
    st.alerted = false;
    st.silent = 0;
    st.suppressed = false;
    return wasAlerted ? "clear" : null;
  }

  /** 使用者關閉通知:抑制到訊號恢復為止(只在警示中時有意義) */
  dismiss(trackId: number): void {
    const st = this.tracks.get(trackId);
    if (st && st.alerted) st.suppressed = true;
  }

  /** 軌道已刪除:移除狀態;若警示中回傳 "clear" 供上層收通知 */
  forget(trackId: number): SilenceTransition {
    const st = this.tracks.get(trackId);
    this.tracks.delete(trackId);
    return st?.alerted ? "clear" : null;
  }

  /** engine 換代/重連:全部歸零 */
  reset(): void {
    this.tracks.clear();
  }

  /** 追蹤中的軌 id(供上層比對軌道刪除) */
  ids(): number[] {
    return [...this.tracks.keys()];
  }
}

export type AlarmKey = string; // "silence:<trackId>" | "srcfail:<trackId>" | "stale"

/** 活躍警示集合:key 查重 + 是否該嗶(有活躍且未被手動關閉的警示) */
export class AlarmSet {
  private active = new Set<AlarmKey>();
  private dismissed = new Set<AlarmKey>();

  /** 新警示回傳 true;重複 raise 同 key = no-op */
  raise(key: AlarmKey): boolean {
    if (this.active.has(key)) return false;
    this.active.add(key);
    this.dismissed.delete(key);
    return true;
  }

  /** 解除;不存在 = no-op 回傳 false */
  clear(key: AlarmKey): boolean {
    this.dismissed.delete(key);
    return this.active.delete(key);
  }

  /** 手動關閉:警示仍在(情況未解除)但不再嗶、不再重複通知 */
  dismiss(key: AlarmKey): void {
    this.dismissed.add(key);
  }

  isDismissed(key: AlarmKey): boolean {
    return this.dismissed.has(key);
  }

  get shouldBeep(): boolean {
    for (const k of this.active) if (!this.dismissed.has(k)) return true;
    return false;
  }

  get activeKeys(): AlarmKey[] {
    return [...this.active];
  }

  reset(): void {
    this.active.clear();
    this.dismissed.clear();
  }
}

export interface AlertBeeper {
  start(): void;
  stop(): void;
}

/** Web Audio 短嗶(880Hz sine 150ms);AudioContext 惰性建立,被擋時靜默失敗(只靠提示框) */
export function createAlertBeeper(intervalMs: number = BEEP_INTERVAL_MS): AlertBeeper {
  let ctx: AudioContext | null = null;
  let timer: ReturnType<typeof setInterval> | null = null;
  function beep(): void {
    try {
      ctx ??= new AudioContext();
      void ctx.resume().catch(() => {}); // autoplay 政策:有使用者手勢後才會成功
      const t = ctx.currentTime;
      const osc = ctx.createOscillator();
      const gain = ctx.createGain();
      osc.type = "sine";
      osc.frequency.value = 880;
      gain.gain.setValueAtTime(0.0001, t);
      gain.gain.exponentialRampToValueAtTime(0.15, t + 0.01);
      gain.gain.exponentialRampToValueAtTime(0.0001, t + 0.15);
      osc.connect(gain).connect(ctx.destination);
      osc.start(t);
      osc.stop(t + 0.16);
    } catch {
      // WebView2 擋音訊/無 AudioContext → 提示框仍可見
    }
  }
  return {
    start() {
      if (timer !== null) return;
      beep();
      timer = setInterval(beep, intervalMs);
    },
    stop() {
      if (timer !== null) {
        clearInterval(timer);
        timer = null;
      }
    },
  };
}
