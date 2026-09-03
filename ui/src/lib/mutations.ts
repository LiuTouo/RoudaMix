// P1-B:快速連續命令的序列化 + coalesce。
// 問題:toggleDest/mute/bypass 等以「目前 track props」計算新值送 engine;連點時
// 前一命令還在飛,拿到 stale props 會蓋掉使用者最後意圖。engine 端 dispatch 本身
// 序列化(g_engine_mutex),client 端只需要:(1) 同資源命令依序送(不平行)、
// (2) 同資源同性質的新意圖覆蓋還沒開始跑的舊意圖(latest-wins)。
// 失敗不回滾本地 —— engine 權威 status event 會把實際值帶回(onError 只顯示)。
// rejection 傳遞結構化 CommandError(onError 端以 code 分支,不做字串解析)。

import { toCommandError, type CommandError } from "./protocol-commands.generated.ts";

export interface MutationJob {
  tag?: string; // 同 key 內同性質(bypass/mute/dests...)才互蓋;undefined = 不可蓋
  fn: () => Promise<unknown>;
  cancelled: false;
  settle?: (result: MutationRunResult<unknown>) => void;
}

export type MutationRunResult<T> =
  | { status: "completed"; value: T }
  | { status: "failed"; error: CommandError }
  | { status: "superseded" };

/** 純邏輯:把 queue 中尚未執行、tag 相同的 job 標 superseded(發動者負責移除)。
 *  回傳仍要執行的 job 序(保留順序)。可測核心 —— MutationQueue.run 用它。 */
export function supersedeQueued(jobs: MutationJob[], tag: string | undefined): MutationJob[] {
  if (tag === undefined) return jobs;
  return jobs.filter((j) => j.tag !== tag);
}

/** 同 key(資源)序列化執行;不同 key 平行。失敗交 onError(顯示),佇列續跑。 */
export class MutationQueue {
  private queues = new Map<string, MutationJob[]>();
  private running = new Set<string>();
  private idleWaiters = new Map<string, Set<() => void>>();
  private onError: (key: string, err: CommandError, tag: string | undefined) => void;

  constructor(onError: (key: string, err: CommandError, tag: string | undefined) => void) {
    this.onError = onError;
  }

  /** 同 key + 同 tag 的等待中 job 被新 job 蓋掉(latest-wins) */
  run<T>(
    key: string,
    tag: string | undefined,
    fn: () => Promise<T>,
  ): Promise<MutationRunResult<T>> {
    return new Promise((resolve) => {
      let q = this.queues.get(key);
      if (!q) {
        q = [];
        this.queues.set(key, q);
      }
      if (tag !== undefined) {
        for (const job of q) {
          if (job.tag === tag) job.settle?.({ status: "superseded" });
        }
      }
      const kept = supersedeQueued(q, tag);
      q.length = 0;
      q.push(...kept);
      q.push({
        tag,
        fn,
        cancelled: false,
        settle: (result) => resolve(result as MutationRunResult<T>),
      });
      void this.pump(key);
    });
  }

  /** 該 key 有命令在跑或等待中(UI 可顯示處理中/disabled) */
  busy(key: string): boolean {
    const q = this.queues.get(key);
    return this.running.has(key) || (q?.length ?? 0) > 0;
  }

  /** 等候指定資源現有的執行中與排隊工作全部完成。 */
  whenIdle(key: string): Promise<void> {
    if (!this.busy(key)) return Promise.resolve();
    return new Promise((resolve) => {
      let waiters = this.idleWaiters.get(key);
      if (!waiters) {
        waiters = new Set();
        this.idleWaiters.set(key, waiters);
      }
      waiters.add(resolve);
    });
  }

  private async pump(key: string): Promise<void> {
    if (this.running.has(key)) return;
    this.running.add(key);
    try {
      for (;;) {
        const job = this.queues.get(key)?.shift();
        if (!job) break;
        try {
          const value = await job.fn();
          job.settle?.({ status: "completed", value });
        } catch (e) {
          const error = toCommandError(e);
          this.onError(key, error, job.tag);
          job.settle?.({ status: "failed", error });
        }
      }
    } finally {
      this.running.delete(key);
      const q = this.queues.get(key);
      if (q && q.length === 0) this.queues.delete(key);
      // pump 期間又有 push 但 racing running flag:再泵一次保險
      if ((this.queues.get(key)?.length ?? 0) > 0) void this.pump(key);
      else {
        const waiters = this.idleWaiters.get(key);
        this.idleWaiters.delete(key);
        for (const resolve of waiters ?? []) resolve();
      }
    }
  }
}

/** key helpers:資源識別(一眼看出作用範圍) */
export const mutKey = {
  track: (id: number) => `track:${id}`,
  plugin: (id: number) => `plugin:${id}`,
  device: "audio:device", // 裝置/buffer 切換:全 engine 單一序列
  settings: "app:settings",
  session: "session:lifecycle",
};
