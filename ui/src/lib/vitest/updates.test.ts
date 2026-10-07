import { afterEach, describe, expect, it, vi } from "vitest";
import { checkRelease } from "../updates";

const mocks = vi.hoisted(() => ({ check: vi.fn() }));
vi.mock("@tauri-apps/plugin-updater", () => ({ check: mocks.check }));
afterEach(() => vi.clearAllMocks());

describe("更新檢查", () => {
  it("有新版時回報版本", async () => {
    mocks.check.mockResolvedValue({ version: "0.1.2" });
    expect(await checkRelease("0.1.1")).toEqual({ version: "0.1.2", available: true });
  });
  it("外掛回傳 null 代表已是最新", async () => {
    mocks.check.mockResolvedValue(null);
    expect(await checkRelease("0.1.1")).toEqual({ version: "0.1.1", available: false });
  });
  it("轉發檢查失敗(離線、限流等)", async () => {
    mocks.check.mockRejectedValue(new Error("rate limit exceeded"));
    await expect(checkRelease("0.1.1")).rejects.toThrow("rate limit");
  });
});
