// Where a floating layer (a menu, a popover) stands beside what opened it:
// on the side asked for, turned to the other side where the window has no
// room, and kept inside the window's edges.
const MARGIN = 8;

export function placeNear(anchor, size, { placement = "bottom-start", gap = 6 } = {}) {
	const viewport = { width: window.innerWidth, height: window.innerHeight };
	const [side, align] = placement.split("-");
	let top;
	let left;
	let finalSide = side;

	if (side == "bottom" || side == "top") {
		const below = anchor.bottom + gap;
		const above = anchor.top - gap - size.height;
		const fitsBelow = below + size.height <= viewport.height - MARGIN;
		const fitsAbove = above >= MARGIN;
		finalSide = side == "bottom" ? (fitsBelow || !fitsAbove ? "bottom" : "top") : (fitsAbove || !fitsBelow ? "top" : "bottom");
		top = finalSide == "bottom" ? below : above;
		left = align == "end" ? anchor.right - size.width : align == "center" ? anchor.left + anchor.width / 2 - size.width / 2 : anchor.left;
	} else {
		const right = anchor.right + gap;
		const leftSide = anchor.left - gap - size.width;
		const fitsRight = right + size.width <= viewport.width - MARGIN;
		finalSide = side == "right" ? (fitsRight || leftSide < MARGIN ? "right" : "left") : (leftSide >= MARGIN ? "left" : "right");
		left = finalSide == "right" ? right : leftSide;
		top = align == "end" ? anchor.bottom - size.height : anchor.top;
	}

	left = Math.min(Math.max(MARGIN, left), viewport.width - MARGIN - size.width);
	top = Math.min(Math.max(MARGIN, top), viewport.height - MARGIN - size.height);
	return { top: Math.round(top), left: Math.round(left), side: finalSide };
}

// Everything in a container the keyboard can reach, in order.
export function focusables(container) {
	if (container == null) return [];
	return [...container.querySelectorAll('button:not([disabled]), [href], input:not([disabled]), select:not([disabled]), textarea:not([disabled]), [tabindex]:not([tabindex="-1"])')]
		.filter(element => element.offsetParent != null || element == document.activeElement);
}
