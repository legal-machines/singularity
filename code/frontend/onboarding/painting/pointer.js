import { SIZE } from "./scene.js";

export function bindPaintingCursor(canvas, picture, renderer, running, seconds, measure = () => picture.getBoundingClientRect()) {
  const frieze = picture.parentElement;
  function point(event) {
    const box = measure();
    return [(event.clientX-box.left)/box.width*SIZE[0], (event.clientY-box.top)/box.height*SIZE[1]];
  }
  function move(event) {
    if (!running() || event.pointerType !== "mouse" || !renderer()) return;
    canvas.dataset.hover = renderer().setPointer(...point(event), seconds());
  }
  function leave() { renderer()?.clearPointer(); canvas.dataset.hover = ""; }
  function click(event) {
    if (!running() || !renderer()) return;
    canvas.dataset.hover = renderer().setPointer(...point(event), seconds());
    renderer().activate();
  }
  function wheel(event) {
    if (!running() || event.ctrlKey) return;
    const unit = event.deltaMode === 1 ? 16 : event.deltaMode === 2 ? canvas.clientHeight : 1;
    const delta = Math.abs(event.deltaY) >= Math.abs(event.deltaX) ? event.deltaY : event.deltaX;
    const kick = Math.max(-1, Math.min(1, delta*unit/180));
    renderer()?.nudge(kick);
    canvas.dataset.scrollImpulse = kick.toFixed(3);
  }
  let previousScroll = frieze.scrollLeft;
  function pan() {
    const delta = frieze.scrollLeft-previousScroll;
    previousScroll = frieze.scrollLeft;
    if (!running()) return;
    const kick = Math.max(-.5, Math.min(.5, delta/240));
    renderer()?.nudge(kick);
    canvas.dataset.panImpulse = kick.toFixed(3);
  }
  canvas.addEventListener("pointermove", move);
  canvas.addEventListener("pointerleave", leave);
  canvas.addEventListener("click", click);
  canvas.addEventListener("wheel", wheel, { passive: true });
  frieze.addEventListener("scroll", pan, { passive: true });
  return () => {
    canvas.removeEventListener("pointermove", move);
    canvas.removeEventListener("pointerleave", leave);
    canvas.removeEventListener("click", click);
    canvas.removeEventListener("wheel", wheel);
    frieze.removeEventListener("scroll", pan);
  };
}
