// One parabolic flight, sampled once for a compositor animation. Nearby
// colleagues exchange a small arc; a long crossing has a higher, slower arc.
export function fileFlight(start, end, ceiling = -Infinity) {
	const [x1, y1] = start;
	const [x2, y2] = end;
	const dx = x2 - x1;
	const dy = y2 - y1;
	const distance = Math.hypot(dx, dy);
	const rise = Math.min(150, distance * 0.22, Math.max(0, Math.min(y1, y2) - ceiling - 12));
	// Almost vertical exchanges bend inward instead of tracing a straight line.
	const bend = Math.abs(dx) < Math.abs(dy) * 0.5 ? -Math.sign(x1 || -1) * Math.min(48, distance * 0.16) : 0;
	return {
		duration: Math.max(900, Math.min(1500, 650 + distance * 1.4)),
		frames: Array.from({ length: 61 }, (_, index) => {
			const t = index / 60;
			const arc = 4 * t * (1 - t);
			const x = x1 + dx * t + bend * arc;
			const y = y1 + dy * t - rise * arc;
			return {
				offset: t,
				transform: `translate(${x}px, ${y}px) rotate(${-5 + t * 10}deg) scale(${0.86 + 0.14 * Math.sin(Math.PI * t)})`,
				opacity: Math.min(1, t / 0.12, (1 - t) / 0.16),
			};
		}),
	};
}
