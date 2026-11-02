import { useEffect, useRef } from "react";
import { createAnalyticField } from "./heatAnalytic.js";
import { createFluidField } from "./heatFluid.js";
import styles from "./ThermalField.module.css";

const STEP = 1 / 60;
const QUALITY = { sim: 96, heat: 96, iterations: 14 };

function seeded() {
	let state = 5;
	return () => {
		state = (state + 0x6d2b79f5) | 0;
		let value = Math.imul(state ^ (state >>> 15), 1 | state);
		value = (value + Math.imul(value ^ (value >>> 7), 61 | value)) ^ value;
		return ((value ^ (value >>> 14)) >>> 0) / 4294967296;
	};
}

// Public-site Navier-Stokes / Boussinesq solver, pressure projection,
// semi-Lagrangian advection and heat conduction, with its physical constants
// unchanged. A fixed 60 Hz simulation is displayed at 30 Hz. The analytic
// renderer is only a fallback when half-float render targets are unavailable.
export function ThermalField() {
	const ref = useRef(null);

	useEffect(() => {
		const canvas = ref.current;
		const host = canvas.parentElement;
		const options = { alpha: false, antialias: false, depth: false, stencil: false, powerPreference: "low-power" };
		const gl = canvas.getContext("webgl2", options) || canvas.getContext("webgl", options);
		if (!gl) return;
		const webgl2 = typeof WebGL2RenderingContext !== "undefined" && gl instanceof WebGL2RenderingContext;
		const reduced = matchMedia("(prefers-reduced-motion: reduce)");
		const debug = gl.getExtension("WEBGL_debug_renderer_info");
		const renderer = debug ? gl.getParameter(debug.UNMASKED_RENDERER_WEBGL) : "";
		const software = /swiftshader|llvmpipe|softpipe|software|basic render/i.test(renderer);
		const field = (!software && createFluidField(gl, webgl2, reduced.matches ? seeded() : Math.random)) || createAnalyticField(gl);
		if (!field) return;
		canvas.dataset.solver = field.kind;

		const view = { width: 1, height: 1, scale: 1, cell: 20, grain: 0.65, copy: [] };
		let frame = 0;
		let previous = 0;
		let accumulator = 0;
		let warm = field.kind === "fluid" ? 8 : 0;
		let pointer = null;
		let budget = 6;
		let disposed = false;
		let lost = false;
		const still = () => reduced.matches || software;
		const deposit = (dt) => {
			budget = Math.min(6, budget + dt * 2.4);
			if (!pointer || pointer.y < 84) return;
			const overCopy = view.copy.some(([l, t, r, b]) => pointer.x >= l - 16 && pointer.x <= r + 16 && pointer.y >= t - 16 && pointer.y <= b + 16);
			const share = overCopy ? 0.2 : 1;
			const speed = Math.hypot(pointer.vx, pointer.vy);
			const heat = Math.min(budget, (3.6 + Math.min(8, speed * 0.0045)) * dt) * share;
			budget -= heat;
			field.splat({ x: pointer.x, y: pointer.y, vx: pointer.vx * share, vy: pointer.vy * share, heat });
			pointer.vx *= Math.exp(-12 * dt);
			pointer.vy *= Math.exp(-12 * dt);
		};
		const draw = (now) => {
			frame = 0;
			if (disposed || lost || document.hidden) return;
			if (!previous || now - previous >= 1000 / 30 - 1) {
				const elapsed = previous ? Math.min((now - previous) / 1000, 4 * STEP) : 0;
				previous = now;
				if (warm > 0) {
					// Bounded warm-up batches leave the window and its controls live.
					field.warm(Math.min(warm, 4 / 30), 1 / 30);
					warm = Math.max(0, warm - 4 / 30);
				} else if (!still()) {
					accumulator += elapsed;
					while (accumulator >= STEP) {
						deposit(STEP);
						field.step(STEP);
						accumulator -= STEP;
					}
				}
				if (warm <= 0) {
					view.blend = still() ? 1 : accumulator / STEP;
					field.render(view);
					canvas.dataset.ready = "";
				}
			}
			if (warm > 0 || !still()) frame = requestAnimationFrame(draw);
		};
		const restart = () => {
			cancelAnimationFrame(frame);
			frame = 0;
			previous = 0;
			accumulator = 0;
			pointer = null;
			if (!document.hidden && !lost) frame = requestAnimationFrame(draw);
		};
		const resize = () => {
			const box = host.getBoundingClientRect();
			view.width = Math.max(1, box.width);
			view.height = Math.max(1, box.height);
			view.scale = Math.min(devicePixelRatio || 1, 2, Math.sqrt(1_200_000 / (view.width * view.height)));
			view.copy = [...host.querySelectorAll("h1, button")].map((element) => {
				const bounds = element.getBoundingClientRect();
				return [bounds.left - box.left, bounds.top - box.top, bounds.right - box.left, bounds.bottom - box.top];
			});
			canvas.width = Math.round(view.width * view.scale);
			canvas.height = Math.round(view.height * view.scale);
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
			const cap = Math.min(1, 2000 / Math.max(1, Math.hypot(vx, vy)));
			pointer = { x, y, vx: vx * cap, vy: vy * cap, time: event.timeStamp };
		};
		const leave = () => { pointer = null; };
		const contextLost = (event) => {
			event.preventDefault();
			lost = true;
			cancelAnimationFrame(frame);
			delete canvas.dataset.ready;
		};
		const observer = new ResizeObserver(resize);
		observer.observe(host);
		host.addEventListener("pointermove", move);
		host.addEventListener("pointerleave", leave);
		canvas.addEventListener("webglcontextlost", contextLost);
		document.addEventListener("visibilitychange", restart);
		reduced.addEventListener("change", restart);

		return () => {
			disposed = true;
			cancelAnimationFrame(frame);
			observer.disconnect();
			host.removeEventListener("pointermove", move);
			host.removeEventListener("pointerleave", leave);
			canvas.removeEventListener("webglcontextlost", contextLost);
			document.removeEventListener("visibilitychange", restart);
			reduced.removeEventListener("change", restart);
			delete canvas.dataset.ready;
			delete canvas.dataset.solver;
			field.dispose();
		};
	}, []);

	return <canvas ref={ref} className={styles.field} aria-hidden="true" />;
}
