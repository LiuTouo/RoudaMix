// @vitest-environment jsdom
import { expect, it } from "vitest";
import { flushSync, mount, unmount } from "svelte";
import PalettePrototype from "../../PalettePrototype.svelte";

it("palette preview supports deep links, wrapping, keyboard safety and local mute", async () => {
  history.replaceState(null, "", "/?prototype=palette&variant=E");
  const component = mount(PalettePrototype, { target: document.body });
  try {
    flushSync();
    expect(document.querySelectorAll(".theme-card")).toHaveLength(5);
    expect(document.documentElement.style.getPropertyValue("--accent")).toBe("#496579");
    document.querySelector<HTMLButtonElement>('[aria-label="下一組配色"]')!.click();
    flushSync();
    expect(location.search).toContain("variant=A");
    window.dispatchEvent(new KeyboardEvent("keydown", { key: "ArrowLeft" }));
    flushSync();
    expect(location.search).toContain("variant=E");
    document.querySelector("input")!.dispatchEvent(new KeyboardEvent("keydown", { key: "ArrowRight", bubbles: true }));
    expect(location.search).toContain("variant=E");
    document.querySelector<HTMLButtonElement>(".mute")!.click();
    flushSync();
    expect(document.querySelector(".mute")!.getAttribute("aria-pressed")).toBe("true");
    for (const card of document.querySelectorAll<HTMLButtonElement>(".theme-card")) {
      card.click();
      flushSync();
      const luminance = (hex: string) => {
        const channels = hex.match(/[a-f\d]{2}/gi)!.map(c => parseInt(c, 16) / 255).map(c => c <= .04045 ? c / 12.92 : ((c + .055) / 1.055) ** 2.4);
        return channels[0] * .2126 + channels[1] * .7152 + channels[2] * .0722;
      };
      const color = (name: string) => luminance(document.documentElement.style.getPropertyValue(`--${name}`));
      for (const surface of ["flat-0", "flat-1", "flat-2", "flat-3", "flat-4"]) {
        const rgb = document.documentElement.style.getPropertyValue(`--${surface}`).match(/[a-f\d]{2}/gi)!.map(c => parseInt(c, 16));
        expect(Math.max(...rgb) - Math.min(...rgb)).toBeLessThanOrEqual(5);
      }
      expect(new Set(["accent", "ok", "track-a", "track-b"].map(name => document.documentElement.style.getPropertyValue(`--${name}`))).size).toBe(4);
      for (const text of ["text", "text-dim"]) for (const surface of ["flat-0", "flat-1", "flat-2", "flat-3"]) {
        const a = color(text), b = color(surface);
        expect((Math.max(a, b) + .05) / (Math.min(a, b) + .05)).toBeGreaterThanOrEqual(4.5);
      }
    }
  } finally {
    await unmount(component);
    history.replaceState(null, "", "/");
  }
  expect(document.documentElement.style.getPropertyValue("--accent")).toBe("");
});
