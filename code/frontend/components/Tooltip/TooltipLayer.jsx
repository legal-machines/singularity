import { useEffect, useRef, useState } from "react";
import { createPortal } from "react-dom";

import styles from "./Tooltip.module.css";

// One layer shows the label of whatever the pointer rests on or the keyboard
// reaches, for every element that carries `data-tooltip` - its words alone,
// never a keyboard shortcut. It waits a moment under the pointer and none
// under the keyboard, stands below the element or above it where there is no
// room, and never covers an open menu. A screen reader hears the element's
// own label, so the layer is hidden from it.
const DELAY = 450;
const GAP = 6;
const MARGIN = 8;

export function TooltipLayer() {
	const [tip, setTip] = useState(null);
	const timer = useRef(0);
	const current = useRef(null);
	const bubble = useRef(null);
	const [place, setPlace] = useState(null);

	useEffect(() => {
		function show(element, wait) {
			clearTimeout(timer.current);
			current.current = element;
			timer.current = setTimeout(() => {
				if (current.current != element || element.isConnected == false) return;
				if (element.getAttribute("aria-expanded") == "true") return;
				setPlace(null);
				setTip({ element, text: element.dataset.tooltip });
			}, wait);
		}

		function hide() {
			clearTimeout(timer.current);
			current.current = null;
			setTip(null);
		}

		function over(event) {
			const element = event.target.closest?.("[data-tooltip]");
			if (element == null) {
				if (current.current != null) hide();
				return;
			}
			if (element != current.current) show(element, DELAY);
		}

		function focus(event) {
			const element = event.target.closest?.("[data-tooltip]");
			if (element == null || element.matches(":focus-visible") == false) return;
			show(element, 0);
		}

		document.addEventListener("pointerover", over);
		document.addEventListener("focusin", focus);
		document.addEventListener("focusout", hide);
		document.addEventListener("pointerdown", hide, true);
		document.addEventListener("keydown", hide, true);
		window.addEventListener("scroll", hide, true);
		window.addEventListener("blur", hide);

		return () => {
			clearTimeout(timer.current);
			document.removeEventListener("pointerover", over);
			document.removeEventListener("focusin", focus);
			document.removeEventListener("focusout", hide);
			document.removeEventListener("pointerdown", hide, true);
			document.removeEventListener("keydown", hide, true);
			window.removeEventListener("scroll", hide, true);
			window.removeEventListener("blur", hide);
		};
	}, []);

	// Placed once its own size is known: centred under the element, kept
	// inside the window.
	useEffect(() => {
		if (tip == null || bubble.current == null) return;
		const target = tip.element.getBoundingClientRect();
		const own = bubble.current.getBoundingClientRect();
		const below = target.bottom + GAP + own.height <= window.innerHeight - MARGIN;
		const top = below ? target.bottom + GAP : target.top - GAP - own.height;
		const left = Math.min(Math.max(MARGIN, target.left + target.width / 2 - own.width / 2), window.innerWidth - MARGIN - own.width);
		setPlace({ top, left, below });
	}, [tip]);

	if (tip == null || tip.text == null || tip.text == "") return null;

	return createPortal(
		<div
			ref={bubble}
			className={place == null ? styles.tooltip : `${styles.tooltip} ${styles.shown} ${place.below ? styles.below : styles.above}`}
			style={place == null ? { top: -999, left: -999 } : { top: place.top, left: place.left }}
			aria-hidden="true"
		>
			{tip.text}
		</div>,
		document.body,
	);
}
