import { mount } from "svelte";
import "./app.css";
import "./flat-theme.css";
import "./console.css";
import App from "./App.svelte";
import { installTooltip } from "./lib/tooltip";

// Throwaway studies: DEV-only, isolated from engine mutations and persistence.
const prototype = import.meta.env.DEV ? new URLSearchParams(location.search).get("prototype") : null;
const Root = prototype === "console"
  ? (await import("./ConsolePrototype.svelte")).default
  : prototype === "palette" ? (await import("./PalettePrototype.svelte")).default : App;
const app = mount(Root, { target: document.getElementById("app")! });
const removeTooltip = installTooltip();

if (import.meta.hot) import.meta.hot.dispose(removeTooltip);

export default app;
