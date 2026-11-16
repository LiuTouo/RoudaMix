/**
 * Marks mouse-driven sessions on <body> so CSS can hide keyboard focus rings.
 * ":focus-visible" still matches mouse clicks on inputs/sliders (and any click
 * after keyboard use); Tab clears the mark so keyboard navigation keeps rings.
 */
export function installFocusMode(root: Document = document): () => void {
  const body = root.body;
  const markMouse = () => body.classList.add("using-mouse");
  const markKeyboard = (event: KeyboardEvent) => {
    if (event.key === "Tab") body.classList.remove("using-mouse");
  };
  root.addEventListener("mousedown", markMouse);
  root.addEventListener("keydown", markKeyboard);
  return () => {
    root.removeEventListener("mousedown", markMouse);
    root.removeEventListener("keydown", markKeyboard);
    body.classList.remove("using-mouse");
  };
}
