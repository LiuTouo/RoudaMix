import { mount } from "svelte";
import "./app.css";
import "./flat-theme.css";
import "./console.css";
import "./motion.css";
import App from "./App.svelte";
import { installTooltip } from "./lib/tooltip";

// 建置目標(chrome87 等)不支援 top-level await:動態 root 用 async bootstrap。
// 響度分析獨立視窗(main 以 WebviewWindow 開啟,URL ?window=loudness):
// 生產/開發都要能進,優先於 prototype 判斷。
async function resolveRoot() {
  const params = new URLSearchParams(location.search);
  if (params.get("window") === "loudness")
    return (await import("./lib/LoudnessWindow.svelte")).default;
  // Throwaway studies: DEV-only, isolated from engine mutations and persistence.
  if (import.meta.env.DEV) {
    const prototype = params.get("prototype");
    if (prototype === "console") return (await import("./ConsolePrototype.svelte")).default;
    if (prototype === "palette") return (await import("./PalettePrototype.svelte")).default;
    if (prototype === "containers") return (await import("./ContainerPrototype.svelte")).default;
  }
  return App;
}

void (async () => {
  const app = mount(await resolveRoot(), { target: document.getElementById("app")! });
  const removeTooltip = installTooltip();
  if (import.meta.hot) import.meta.hot.dispose(removeTooltip);
  return app;
})();
