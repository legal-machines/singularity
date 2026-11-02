// Keep both faces and both messages readable. Bubbles remain in front and
// below their authors; their single-line copy can grow left or right before moving down.
// Measure only when a dialogue/font/window changes, never on an animation frame.
const intersects = (a, b, gap = 0) => a.left < b.right + gap && a.right > b.left - gap && a.top < b.bottom + gap && a.bottom > b.top - gap;
const shifted = (box, dx, dy) => ({ left: box.left + dx, right: box.right + dx, top: box.top + dy, bottom: box.bottom + dy });

export function placeDialogue(field, messages, ids) {
	const scale = new DOMMatrixReadOnly(getComputedStyle(field).transform).a;
	const section = field.closest("section");
	const bounds = section.getBoundingClientRect();
	const title = section.querySelector("h1").getBoundingClientRect();
	const welcome = section.querySelector("[data-welcome-content]")?.getBoundingClientRect();
	const faces = ids.map(id => field.querySelector(`[data-colleague="${id}"]`).getBoundingClientRect());
	const candidates = messages.map((message, index) => {
		message.style.maxWidth = "none";
		message.style.left = "8px";
		message.style.setProperty("--bubble-drop", "0px");
		const naturalWidth = message.offsetWidth;
		const slot = message.closest("[data-bubble-owner]");
		const size = parseFloat(slot.style.getPropertyValue("--orbit-size"));
		const tilt = new DOMMatrixReadOnly(getComputedStyle(slot).transform);
		const drift = new DOMMatrixReadOnly(getComputedStyle(message.parentElement).transform);
		const angle = Math.atan2(tilt.b, tilt.a) + Math.atan2(drift.b, drift.a);
		const cos = Math.cos(angle) * scale;
		const sin = Math.sin(angle) * scale;
		const widths = [naturalWidth];
		const choices = [];
		for (const width of widths) {
			message.style.maxWidth = `${width}px`;
			const box = message.getBoundingClientRect();
			const minLeft = Math.min(8, 8 - message.offsetWidth);
			const maxLeft = size - 8;
			// The portrait must still overlap the top edge of its own balloon.
			const lefts = [8, minLeft, maxLeft,
				8 + (faces[1 - index].left - 12 * scale - box.right) / cos,
				8 + (bounds.left + 12 - box.left) / cos,
				8 + (bounds.right - 12 - box.right) / cos,
			];
			for (const left of lefts.filter(left => left >= minLeft && left <= maxLeft)) {
				for (const drop of [0, 8, 16, 24, 32]) {
					const placed = shifted(box, (left - 8) * cos - drop * sin, (left - 8) * sin + drop * cos);
					if (placed.left < bounds.left + 10 || placed.right > bounds.right - 10 || placed.bottom > (welcome ? bounds.bottom - 24 : title.top - 10)) continue;
					if (intersects(placed, faces[1 - index], 10 * scale) || welcome && intersects(placed, welcome, 16)) continue;
					choices.push({ width, left, drop, box: placed, cost: Math.abs(left - 8) * 0.35 + drop * 4 });
				}
			}
		}
		return choices.sort((a, b) => a.cost - b.cost);
	});
	let best = null;
	for (const ask of candidates[0]) for (const answer of candidates[1]) {
		const cost = ask.cost + answer.cost;
		if (best && cost >= best.cost) continue;
		if (!intersects(ask.box, answer.box, 10 * scale)) best = { choices: [ask, answer], cost };
	}
	for (const [index, message] of messages.entries()) {
		const choice = best?.choices[index] ?? candidates[index][0];
		message.dataset.placement = best ? "clear" : "unresolved";
		message.style.maxWidth = choice ? `${choice.width}px` : "none";
		message.style.left = `${choice?.left ?? 8}px`;
		message.style.setProperty("--bubble-drop", `${choice?.drop ?? 0}px`);
	}
}
