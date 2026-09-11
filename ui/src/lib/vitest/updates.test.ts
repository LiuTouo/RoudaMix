import { afterEach, describe, expect, it, vi } from "vitest";
import { checkRelease, releaseUpdate } from "../updates";

const release = (tag_name: string) => ({ tag_name, draft: false, prerelease: false });
afterEach(() => vi.unstubAllGlobals());

describe("正式版更新檢查", () => {
  it("依數字比較版本，避免降版，並允許預覽版升至同版本正式版", () => {
    expect(releaseUpdate(release("v0.1.10"), "0.1.9").available).toBe(true);
    expect(releaseUpdate(release("v0.1.1"), "0.1.1").available).toBe(false);
    expect(releaseUpdate(release("v0.1.0"), "0.1.1").available).toBe(false);
    expect(releaseUpdate(release("v1.0.0"), "1.0.0-rc.1").available).toBe(true);
  });
  it("拒絕草稿、預覽版本及異常回應", () => {
    for (const raw of [null, {}, { ...release("v1.0.0"), draft: true },
      { ...release("v1.0.0"), prerelease: true }, release("v1.0.0-rc.1"), release("v01.0.0"), release("not-a-version")])
      expect(() => releaseUpdate(raw, "0.1.1")).toThrow();
  });
  it("從正式發布端點讀取版本", async () => {
    const fetcher = vi.fn().mockResolvedValue({ ok: true, status: 200, json: async () => release("v0.1.2") });
    vi.stubGlobal("fetch", fetcher);
    expect(await checkRelease("0.1.1")).toEqual({ version: "0.1.2", available: true });
    expect(fetcher.mock.calls[0][0]).toBe("https://api.github.com/repos/LiuTouo/RoudaMix/releases/latest");
    expect(fetcher.mock.calls[0][1].signal).toBeInstanceOf(AbortSignal);
  });
  it("回報限流、服務錯誤及離線", async () => {
    const fetcher = vi.fn();
    vi.stubGlobal("fetch", fetcher);
    for (const status of [403, 429, 500]) {
      fetcher.mockResolvedValue({ ok: false, status });
      await expect(checkRelease("0.1.1")).rejects.toThrow(status === 500 ? "無法使用" : "限制請求");
    }
    fetcher.mockRejectedValue(new TypeError("Failed to fetch"));
    await expect(checkRelease("0.1.1")).rejects.toThrow("Failed to fetch");
  });
});
