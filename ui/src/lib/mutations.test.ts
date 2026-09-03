// P1-B 測試:序列化順序、latest-wins coalesce、失敗不斷佇列(node:test)
import { test } from "node:test";
import assert from "node:assert";
import { MutationQueue, supersedeQueued, type MutationJob } from "./mutations.ts";

function job(tag: string | undefined): MutationJob {
  return { tag, fn: async () => {}, cancelled: false };
}

test("supersedeQueued:同 tag 的等待中 job 被移除,其他保留順序", () => {
  const jobs = [job("mute"), job("gain"), job("mute"), job(undefined), job("gain")];
  const kept = supersedeQueued(jobs, "mute");
  assert.equal(kept.length, 3);
  assert.deepEqual(
    kept.map((j) => j.tag),
    ["gain", undefined, "gain"],
  );
  // undefined tag = 不互蓋(全保留)
  assert.equal(supersedeQueued(jobs, undefined).length, 5);
});

test("MutationQueue:同 key 序列化(先進先完成),不同 key 平行", async () => {
  const order: string[] = [];
  const q = new MutationQueue(() => {});
  const slow = () => new Promise<void>((res) => setTimeout(() => (order.push("a1"), res()), 20));
  q.run("k", undefined, slow);
  q.run("k", undefined, async () => order.push("a2"));
  q.run("other", undefined, async () => order.push("b1"));
  await new Promise((r) => setTimeout(r, 50));
  assert.equal(order[0], "b1"); // different key 不等 k
  assert.deepEqual(order.slice(1), ["a1", "a2"]); // same key 依序
});

test("MutationQueue:latest-wins —— 等待中的同 tag job 被覆蓋不執行", async () => {
  const ran: number[] = [];
  const q = new MutationQueue(() => {});
  const block = new Promise<void>((res) => setTimeout(res, 15));
  q.run("k", "mute", async () => {
    await block;
    ran.push(1);
  }); // 佔住 pipeline
  q.run("k", "mute", async () => ran.push(2)); // 等待中 → 會被 3 蓋掉
  q.run("k", "mute", async () => ran.push(3)); // 最新意圖
  q.run("k", "gain", async () => ran.push(9)); // 不同 tag:保留
  await new Promise((r) => setTimeout(r, 50));
  assert.deepEqual(ran, [1, 3, 9]); // 2 被蓋掉
});

test("MutationQueue:失敗交 onError、佇列續跑;busy 反映等待中", async () => {
  const errs: Array<[string, { code: string; message: string }]> = [];
  const q = new MutationQueue((k, e) => errs.push([k, e]));
  const p = new Promise<void>((r) => setTimeout(r, 10));
  q.run("k", undefined, async () => {
    await p;
    throw new Error("boom");
  });
  q.run("k", undefined, async () => {});
  assert.equal(q.busy("k"), true);
  await new Promise((r) => setTimeout(r, 40));
  assert.equal(errs.length, 1);
  assert.equal(errs[0][0], "k");
  // rejection 一律正規化成 {code, message}:非 bridge rejection 也帶 fallback code
  assert.match(errs[0][1].message, /boom/);
  assert.equal(typeof errs[0][1].code, "string");
  assert.equal(q.busy("k"), false);
  // busy:等待中(未開始)也算
  q.run("k2", undefined, async () => {});
  assert.equal(q.busy("k2"), true);
  await new Promise((r) => setTimeout(r, 5));
});
