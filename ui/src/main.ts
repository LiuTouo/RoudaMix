import { mount } from "svelte";
import "./app.css";
import "./flat-theme.css";
import App from "./App.svelte";
import { installTooltip } from "./lib/tooltip";

// Throwaway palette study; never included in production builds or connected to IPC.
const palettePrototype = import.meta.env.DEV && new URLSearchParams(location.search).get("prototype") === "palette";
const Root = palettePrototype ? (await import("./PalettePrototype.svelte")).default : App;
const app = mount(Root, { target: document.getElementById("app")! });
const removeTooltip = installTooltip();

if (import.meta.hot) import.meta.hot.dispose(removeTooltip);

export default app;
