import { useEffect, useRef } from "react";
import { createAnalyticField } from "./heatAnalytic.js";
import { createFluidField } from "./heatFluid.js";
import styles from "./ThermalField.module.css";

const STEP = 1 / 60;
const QUALITY = { sim: 96, heat: 96, iterations: 14 };

// The original fluid solver runs behind a glyph mask. Only the letters are
// composited onto the page; the surrounding surface remains pure white.
export function ThermalWordmark() {
	const ref = useRef(null);

	useEffect(() => {
		const canvas = ref.current;
		const host = canvas.parentElement;
		const text = host.getAttribute("aria-label");
		const output = canvas.getContext("2d");
		const source = document.createElement("canvas");
		const mask = document.createElement("canvas");
		const ink = mask.getContext("2d");
		const options = { alpha: false, antialias: false, depth: false, stencil: false, powerPreference: "low-power" };
		const gl = source.getContext("webgl2", options) || source.getContext("webgl", options);
		if (!gl || !output || !ink) return;
		const webgl2 = typeof WebGL2RenderingContext !== "undefined" && gl instanceof WebGL2RenderingContext;
		const reduced = matchMedia("(prefers-reduced-motion: reduce)");
		const debug = gl.getExtension("WEBGL_debug_renderer_info");
		const renderer = debug ? gl.getParameter(debug.UNMASKED_RENDERER_WEBGL) : "";
		const software = /swiftshader|llvmpipe|softpipe|software|basic render/i.test(renderer);
		let seed = 5;
		const random = () => ((seed = (Math.imul(seed, 1664525) + 1013904223) >>> 0) / 4294967296);
		const field = (!software && createFluidField(gl, webgl2, reduced.matches ? random : Math.random)) || createAnalyticField(gl);
		if (!field) return;
		canvas.dataset.solver = field.kind;

		const view = { width: 1, height: 1, scale: 1, cell: 5, grain: 0.3, copy: [] };
		let boxHeight = 1;
		let crop = 0;
		let frame = 0;
		let previous = 0;
		let accumulator = 0;
		let time = 0;
		let warm = field.kind === "fluid" ? 8 : 0;
		let pointer = null;
		let disposed = false;
		let lost = false;
		let fontsReady = false;
		const still = () => reduced.matches || software;
		const font = () => {
			const style = getComputedStyle(host);
			return `${style.fontStyle} ${style.fontWeight} ${style.fontSize} ${style.fontFamily}`;
		};
		const clearReady = () => {
			delete host.dataset.heatReady;
			delete canvas.dataset.ready;
		};
		const paint = () => {
			field.render(view);
			output.globalCompositeOperation = "source-over";
			output.clearRect(0, 0, canvas.width, canvas.height);
			output.drawImage(source, 0, crop * view.scale, source.width, boxHeight * view.scale, 0, 0, canvas.width, canvas.height);
			output.globalCompositeOperation = "destination-in";
			output.drawImage(mask, 0, 0);
			output.globalCompositeOperation = "source-over";
			if (fontsReady) {
				host.dataset.heatReady = "";
				canvas.dataset.ready = "";
			}
		};
		const step = () => {
			time += STEP;
			// A slow moving heat source starts a physical plume; advection,
			// buoyancy and cooling, rather than a sliding gradient, carry it on.
			const phase = (time % 12) / 12;
			if (phase < 0.4) field.splat({ x: view.width * (-0.1 + 3 * phase), y: crop + boxHeight * 0.7, vy: -8, heat: STEP * 14 });
			if (pointer) {
				const speed = Math.hypot(pointer.vx, pointer.vy);
				field.splat({ x: pointer.x, y: pointer.y + crop, vx: pointer.vx, vy: pointer.vy, heat: (2.4 + Math.min(6, speed * 0.004)) * STEP });
				pointer.vx *= Math.exp(-12 * STEP);
				pointer.vy *= Math.exp(-12 * STEP);
			}
			field.step(STEP);
		};
		const draw = (now) => {
			frame = 0;
			if (disposed || lost || document.hidden) return;
			if (!previous || now - previous >= 1000 / 30 - 1) {
				const elapsed = previous ? Math.min((now - previous) / 1000, 4 * STEP) : 0;
				previous = now;
				if (warm > 0) {
					field.warm(Math.min(warm, 4 / 30), 1 / 30);
					warm = Math.max(0, warm - 4 / 30);
				} else if (!still()) {
					accumulator += elapsed;
					while (accumulator >= STEP) {
						step();
						accumulator -= STEP;
					}
				}
				if (warm <= 0) {
					view.blend = still() ? 1 : accumulator / STEP;
					paint();
				}
			}
			if (warm > 0 || !still()) frame = requestAnimationFrame(draw);
		};
		const restart = () => {
			cancelAnimationFrame(frame);
			previous = 0;
			accumulator = 0;
			pointer = null;
			if (!disposed && !document.hidden && !lost) frame = requestAnimationFrame(draw);
		};
		const resize = () => {
			const box = host.getBoundingClientRect();
			view.width = Math.max(1, box.width);
			boxHeight = Math.max(1, box.height);
			view.height = boxHeight * 3;
			crop = boxHeight;
			view.scale = Math.min(devicePixelRatio || 1, 2);
			canvas.width = mask.width = Math.round(view.width * view.scale);
			canvas.height = mask.height = Math.round(boxHeight * view.scale);
			source.width = canvas.width;
			source.height = Math.round(view.height * view.scale);
			ink.setTransform(view.scale, 0, 0, view.scale, 0, 0);
			ink.font = font();
			ink.fontKerning = "normal";
			ink.letterSpacing = getComputedStyle(host).letterSpacing;
			ink.textAlign = "center";
			ink.fillStyle = "#000000";
			const metrics = ink.measureText(text);
			const ascent = metrics.fontBoundingBoxAscent ?? metrics.actualBoundingBoxAscent;
			const descent = metrics.fontBoundingBoxDescent ?? metrics.actualBoundingBoxDescent;
			ink.fillText(text, view.width / 2, (boxHeight - ascent - descent) / 2 + ascent);
			field.resize(view.width, view.height, QUALITY, { left: view.height * 2.2, right: view.height * 1.7, from: 0, to: 1 });
			restart();
		};
		const move = (event) => {
			if (still()) return;
			const box = host.getBoundingClientRect();
			const x = event.clientX - box.left;
			const y = event.clientY - box.top;
			const dt = pointer ? (event.timeStamp - pointer.time) / 1000 : 0;
			const vx = dt > 0 && dt < 0.2 ? (x - pointer.x) / dt : 0;
			const vy = dt > 0 && dt < 0.2 ? (y - pointer.y) / dt : 0;
			const cap = Math.min(1, 1200 / Math.max(1, Math.hypot(vx, vy)));
			pointer = { x, y, vx: vx * cap, vy: vy * cap, time: event.timeStamp };
		};
		const leave = () => { pointer = null; };
		const contextLost = (event) => {
			event.preventDefault();
			lost = true;
			cancelAnimationFrame(frame);
			clearReady();
		};
		const observer = new ResizeObserver(resize);
		observer.observe(host);
		host.addEventListener("pointermove", move);
		host.addEventListener("pointerleave", leave);
		source.addEventListener("webglcontextlost", contextLost);
		document.addEventListener("visibilitychange", restart);
		reduced.addEventListener("change", restart);
		document.fonts.load(font(), text).then(() => {
			if (disposed) return;
			fontsReady = true;
			resize();
		}).catch(() => { if (!disposed) clearReady(); });

		return () => {
			disposed = true;
			cancelAnimationFrame(frame);
			observer.disconnect();
			host.removeEventListener("pointermove", move);
			host.removeEventListener("pointerleave", leave);
			source.removeEventListener("webglcontextlost", contextLost);
			document.removeEventListener("visibilitychange", restart);
			reduced.removeEventListener("change", restart);
			clearReady();
			field.dispose();
		};
	}, []);

	return <canvas ref={ref} className={styles.field} aria-hidden="true" />;
}
