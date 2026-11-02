import { useEffect, useRef, useState } from "react";
import styles from "./Confetti.module.css";

// Paper takes its colours from the entry's brick and portrait backgrounds.
// One canvas, a bounded pixel buffer, and no React work on animation frames.
const COLOURS = ["#ae493f", "#d1a75c", "#7e9c88", "#96aabc", "#c7b8a6", "#414746"];
const PER_SIDE = 160;
// The first wave leaves at once; the rest follow a moment later.
const FIRST_WAVE = 112;
const START_MS = 100;
const DURATION = 4.3;
const between = (low, high) => low + Math.random() * (high - low);

function piece(left, width, height, index) {
	const round = Math.random() < 0.12;
	return {
		x: left ? width * 0.035 : width * 0.965,
		y: height * between(0.91, 0.97),
		vx: (left ? 1 : -1) * width * between(0.55, 1.2),
		vy: -height * between(1.6, 2.6),
		delay: index < FIRST_WAVE ? between(0, 0.08) : between(0.16, 0.30),
		life: between(3.5, 4.3),
		colour: COLOURS[index % COLOURS.length],
		round,
		width: round ? between(2.5, 3.5) : between(6, 10),
		height: between(3, 5),
		turn: between(0, Math.PI * 2),
		turning: between(-3, 3),
		flip: between(0, Math.PI * 2),
		flipping: between(4, 8),
		flutter: between(0, Math.PI * 2),
	};
}

// A short, two-sided paper burst only for a completed registration. Nothing
// loops or intercepts a press. Hiding the page or asking for reduced motion
// ends it immediately; all listeners, timers and frames are then released.
export function Confetti() {
	const canvasRef = useRef(null);
	const [over, setOver] = useState(false);

	useEffect(() => {
		const media = window.matchMedia("(prefers-reduced-motion: reduce)");
		if (media.matches) {
			setOver(true);
			return undefined;
		}
		const canvas = canvasRef.current;
		const context = canvas?.getContext("2d");
		if (!context) {
			setOver(true);
			return undefined;
		}
		let width = 0;
		let height = 0;
		let ratio = 1;
		let pieces = [];
		let frame = 0;
		let start = 0;
		let stopped = false;
		const fit = () => {
			const oldWidth = width;
			const oldHeight = height;
			width = Math.max(1, canvas.clientWidth);
			height = Math.max(1, canvas.clientHeight);
			ratio = Math.min(2, window.devicePixelRatio || 1, Math.sqrt(4_000_000 / (width * height)));
			canvas.width = Math.round(width * ratio);
			canvas.height = Math.round(height * ratio);
			context.setTransform(ratio, 0, 0, ratio, 0, 0);
			if (oldWidth && oldHeight) for (const each of pieces) {
				each.x *= width / oldWidth;
				each.y *= height / oldHeight;
				each.vx *= width / oldWidth;
				each.vy *= height / oldHeight;
			}
		};
		const stop = () => {
			stopped = true;
			clearTimeout(start);
			cancelAnimationFrame(frame);
			window.removeEventListener("resize", fit);
			document.removeEventListener("visibilitychange", visibility);
			media.removeEventListener("change", motion);
		};
		const finish = () => { stop(); setOver(true); };
		const visibility = () => { if (document.hidden) finish(); };
		const motion = () => { if (media.matches) finish(); };
		if (document.hidden) {
			setOver(true);
			return undefined;
		}
		fit();
		window.addEventListener("resize", fit);
		document.addEventListener("visibilitychange", visibility);
		media.addEventListener("change", motion);
		let began = 0;
		let last = 0;
		const draw = (now) => {
			if (stopped) return;
			const elapsed = (now - began) / 1000;
			if (elapsed >= DURATION) return finish();
			const dt = Math.min((now - last) / 1000, 1 / 30);
			last = now;
			context.setTransform(ratio, 0, 0, ratio, 0, 0);
			context.clearRect(0, 0, width, height);
			// All paper softens together as it settles, before leaving the window.
			const settling = Math.max(0, Math.min(1, (DURATION - elapsed) / 2.7));
			let alive = false;
			for (const each of pieces) {
				const age = elapsed - each.delay;
				if (age < 0) { alive = true; continue; }
				if (age > each.life || each.y > height + 24) continue;
				alive = true;
				each.vx *= Math.exp(-2.4 * dt);
				each.vy = each.vy * Math.exp(-1.6 * dt) + height * 0.84 * dt;
				each.x += (each.vx + Math.sin(age * 5 + each.flutter) * 18) * dt;
				each.y += each.vy * dt;
				each.turn += each.turning * dt;
				each.flip += each.flipping * dt;
				// One transform per piece (the pixel ratio, its place, its turn and
				// its flip), set outright: no state is saved and restored per piece.
				const cos = Math.cos(each.turn);
				const sin = Math.sin(each.turn);
				const flip = Math.cos(each.flip);
				context.setTransform(ratio * cos * flip, ratio * sin * flip, -ratio * sin, ratio * cos, ratio * each.x, ratio * each.y);
				context.globalAlpha = settling * Math.max(0, Math.min(1, age / 0.09, (each.life - age) / 0.7));
				context.fillStyle = each.colour;
				if (each.round) {
					context.beginPath();
					context.arc(0, 0, each.width, 0, Math.PI * 2);
					context.fill();
				} else context.fillRect(-each.width / 2, -each.height / 2, each.width, each.height);
			}
			if (alive) frame = requestAnimationFrame(draw);
			else finish();
		};
		start = setTimeout(() => {
			for (let index = 0; index < PER_SIDE; index++) pieces.push(piece(true, width, height, index), piece(false, width, height, index));
			began = last = performance.now();
			frame = requestAnimationFrame(draw);
		}, START_MS);
		return stop;
	}, []);

	return over ? null : <canvas ref={canvasRef} className={styles.confetti} aria-hidden="true" />;
}
