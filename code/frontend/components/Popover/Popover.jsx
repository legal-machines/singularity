import { useEffect, useLayoutEffect, useRef, useState } from "react";
import { createPortal } from "react-dom";

import { focusables, placeNear } from "../Floating/position.js";

import styles from "./Popover.module.css";

// A paper that floats beside what opened it and holds a small piece of work:
// an event's details, a quick form, a choice with its reasons. It takes the
// focus when it opens, keeps it inside with Tab, and closes on Escape or a
// press outside, giving the focus back to what opened it. `anchor` is an
// element or a rectangle.
export function Popover({ anchor, onClose, placement = "bottom-start", width = 360, label, labelledBy, className, children }) {
	const paperRef = useRef(null);
	const [place, setPlace] = useState(null);
	const opener = useRef(document.activeElement);

	useLayoutEffect(() => {
		if (paperRef.current == null) return;
		const rect = paperRef.current.getBoundingClientRect();
		const from = anchor == null
			? { top: window.innerHeight / 2, bottom: window.innerHeight / 2, left: window.innerWidth / 2, right: window.innerWidth / 2, width: 0, height: 0 }
			: typeof anchor.getBoundingClientRect == "function" ? anchor.getBoundingClientRect() : anchor;
		setPlace(placeNear(from, { width: rect.width, height: rect.height }, { placement, gap: 8 }));
	}, [anchor, placement]);

	useEffect(() => {
		if (place == null) return;
		const paper = paperRef.current;
		if (paper.contains(document.activeElement) == false) (focusables(paper)[0] ?? paper).focus();
	}, [place]);

	useEffect(() => {
		const returnTo = opener.current;
		function outside(event) {
			if (paperRef.current?.contains(event.target)) return;
			if (event.target.closest?.("[data-menu-layer]") != null) return;
			if (anchor?.contains?.(event.target)) return;
			onClose();
		}
		function keys(event) {
			if (event.key == "Escape") {
				event.preventDefault();
				event.stopPropagation();
				onClose();
				returnTo?.focus?.();
			}
			if (event.key == "Tab") {
				const list = focusables(paperRef.current);
				if (list.length == 0) return;
				const first = list[0];
				const last = list[list.length - 1];
				if (event.shiftKey && document.activeElement == first) {
					event.preventDefault();
					last.focus();
				} else if (!event.shiftKey && document.activeElement == last) {
					event.preventDefault();
					first.focus();
				}
			}
		}
		document.addEventListener("pointerdown", outside, true);
		const paper = paperRef.current;
		paper.addEventListener("keydown", keys);
		return () => {
			document.removeEventListener("pointerdown", outside, true);
			paper.removeEventListener("keydown", keys);
		};
	}, [anchor, onClose]);

	return createPortal(
		<div
			ref={paperRef}
			className={[styles.popover, place != null ? styles.shown : null, className].filter(Boolean).join(" ")}
			style={{ top: place?.top ?? -9999, left: place?.left ?? -9999, width }}
			role="dialog"
			aria-label={label}
			aria-labelledby={labelledBy}
			tabIndex={-1}
		>
			{children}
		</div>,
		document.body,
	);
}
