import { mount } from "svelte";
import "./app.css";
import App from "./App.svelte";
import { installTooltip } from "./lib/tooltip";

const app = mount(App, { target: document.getElementById("app")! });
const removeTooltip = installTooltip();

if (import.meta.hot) import.meta.hot.dispose(removeTooltip);

export default app;
