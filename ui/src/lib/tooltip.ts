const TOOLTIP_ID = "roudamix-tooltip";
const TOOLTIP_SELECTOR = "[data-tooltip]";
const SHOW_DELAY_MS = 450;
const VIEWPORT_GAP = 8;
const POINTER_OFFSET_X = 14;
const POINTER_OFFSET_Y = 18;

type Anchor = { x: number; y: number };

function tooltipTarget(value: EventTarget | null): HTMLElement | null {
  if (!(value instanceof Element)) return null;
  const target = value.closest<HTMLElement>(TOOLTIP_SELECTOR);
  return target?.dataset.tooltip?.trim() ? target : null;
}

/**
 * Installs one delegated, desktop-style tooltip layer for the complete UI.
 * Keeping the layer global also lets dynamically rendered dialogs participate
 * without mounting a tooltip instance for every control.
 */
export function installTooltip(root: Document = document): () => void {
  const tooltip = root.createElement("div");
  tooltip.id = TOOLTIP_ID;
  tooltip.className = "app-tooltip";
  tooltip.setAttribute("role", "tooltip");
  tooltip.setAttribute("popover", "manual");
  tooltip.hidden = true;
  root.body.append(tooltip);

  const popoverSupported = typeof tooltip.showPopover === "function";

  let activeTarget: HTMLElement | null = null;
  let pendingTarget: HTMLElement | null = null;
  let showTimer: number | undefined;
  let keyboardFocusPending = false;

  function clearTimer() {
    if (showTimer !== undefined) {
      window.clearTimeout(showTimer);
      showTimer = undefined;
    }
  }

  function removeDescription(target: HTMLElement | null) {
    if (!target) return;
    const ids = (target.getAttribute("aria-describedby") ?? "")
      .split(/\s+/)
      .filter((id) => id && id !== TOOLTIP_ID);
    if (ids.length > 0) target.setAttribute("aria-describedby", ids.join(" "));
    else target.removeAttribute("aria-describedby");
  }

  function hide() {
    clearTimer();
    pendingTarget = null;
    removeDescription(activeTarget);
    activeTarget = null;
    if (popoverSupported && tooltip.matches(":popover-open")) tooltip.hidePopover();
    tooltip.hidden = true;
    tooltip.textContent = "";
  }

  function position(anchor: Anchor) {
    tooltip.style.left = "0px";
    tooltip.style.top = "0px";
    const rect = tooltip.getBoundingClientRect();
    const viewportWidth = root.documentElement.clientWidth;
    const viewportHeight = root.documentElement.clientHeight;

    const left = Math.min(
      viewportWidth - rect.width - VIEWPORT_GAP,
      Math.max(VIEWPORT_GAP, anchor.x + POINTER_OFFSET_X),
    );
    let top = anchor.y + POINTER_OFFSET_Y;
    if (top + rect.height > viewportHeight - VIEWPORT_GAP) {
      top = anchor.y - rect.height - POINTER_OFFSET_Y;
    }

    tooltip.style.left = `${Math.max(VIEWPORT_GAP, left)}px`;
    tooltip.style.top = `${Math.max(VIEWPORT_GAP, top)}px`;
  }

  function show(target: HTMLElement, anchor: Anchor) {
    const text = target.dataset.tooltip?.trim();
    if (!text || pendingTarget !== target) return;

    removeDescription(activeTarget);
    activeTarget = target;
    pendingTarget = null;
    target.setAttribute(
      "aria-describedby",
      [target.getAttribute("aria-describedby"), TOOLTIP_ID].filter(Boolean).join(" "),
    );

    tooltip.textContent = text;
    tooltip.hidden = false;
    if (popoverSupported) tooltip.showPopover();
    position(anchor);
  }

  function schedule(target: HTMLElement, anchor: Anchor, immediate = false) {
    if (target === activeTarget || target === pendingTarget) return;
    hide();
    pendingTarget = target;
    showTimer = window.setTimeout(() => show(target, anchor), immediate ? 0 : SHOW_DELAY_MS);
  }

  function onPointerOver(event: PointerEvent) {
    const target = tooltipTarget(event.target);
    if (target) schedule(target, { x: event.clientX, y: event.clientY });
  }

  function onPointerOut(event: PointerEvent) {
    const from = tooltipTarget(event.target);
    const to = tooltipTarget(event.relatedTarget);
    if (from && from !== to && (from === activeTarget || from === pendingTarget)) hide();
  }

  function onFocusIn(event: FocusEvent) {
    if (!keyboardFocusPending) return;
    keyboardFocusPending = false;
    const target = tooltipTarget(event.target);
    if (!target) return;
    const rect = target.getBoundingClientRect();
    schedule(target, { x: rect.left + rect.width / 2, y: rect.bottom }, true);
  }

  function onFocusOut(event: FocusEvent) {
    const from = tooltipTarget(event.target);
    const to = tooltipTarget(event.relatedTarget);
    if (from && from !== to && (from === activeTarget || from === pendingTarget)) hide();
  }

  function onKeyDown(event: KeyboardEvent) {
    keyboardFocusPending = event.key === "Tab";
    if (event.key === "Escape") hide();
  }

  function onPointerDown() {
    keyboardFocusPending = false;
    hide();
  }

  root.addEventListener("pointerover", onPointerOver);
  root.addEventListener("pointerout", onPointerOut);
  root.addEventListener("pointerdown", onPointerDown, true);
  root.addEventListener("focusin", onFocusIn);
  root.addEventListener("focusout", onFocusOut);
  root.addEventListener("keydown", onKeyDown);
  window.addEventListener("blur", hide);
  window.addEventListener("resize", hide);
  window.addEventListener("scroll", hide, true);

  return () => {
    hide();
    root.removeEventListener("pointerover", onPointerOver);
    root.removeEventListener("pointerout", onPointerOut);
    root.removeEventListener("pointerdown", onPointerDown, true);
    root.removeEventListener("focusin", onFocusIn);
    root.removeEventListener("focusout", onFocusOut);
    root.removeEventListener("keydown", onKeyDown);
    window.removeEventListener("blur", hide);
    window.removeEventListener("resize", hide);
    window.removeEventListener("scroll", hide, true);
    tooltip.remove();
  };
}
