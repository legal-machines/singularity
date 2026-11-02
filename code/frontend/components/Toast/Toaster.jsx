import { useEffect, useRef, useState } from "react";
import { createPortal } from "react-dom";

import { remeasureTitlebarControls } from "../Window/titlebar.js";
import { dismiss, useToasts } from "./toast.js";

import styles from "./Toast.module.css";

// The leave, as in Toast.module.css.
const LEAVE_MS = 220;

// What the window tells the member comes in from the top, centred over the
// sheet in the window's band, as onboarding's notices do (the owner,
// 2026-11-02: "notifications inside the app must come from the top the same
// way as in onboarding"): on a G2 ground in a soft colour - sage for what
// went as asked, Legal Machines' brick for what went wrong - in white words,
// and goes back up the way it came. Read out politely as they come.
export function Toaster() {
	const items = useToasts();
	const shown = useLeaving(items);
	// A notice stands in the window's strip, which drags the window on macOS:
	// its Undo is handed to the window as it comes and taken back as it goes.
	const standing = shown.filter(item => item.isLeaving != true && item.action != null).map(item => item.id).join(",");
	useEffect(() => remeasureTitlebarControls(), [standing]);

	return createPortal(
		<div className={styles.stack} aria-live="polite" aria-relevant="additions">
			{shown.map(item => (
				<div key={item.id} className={styles.toast} data-tone={toneOf(item.tone)} data-leaving={item.isLeaving || undefined} role="status">
					{item.progress != null ? <span className={styles.fill} style={{ "--progress": Math.min(1, Math.max(0, item.progress)) }} aria-hidden="true" /> : null}
					<span className={styles.text}>{item.text}</span>
					{item.action != null ? (
						<button
							className={styles.action}
							type="button"
							onClick={() => {
								item.action.run();
								dismiss(item.id);
							}}
						>
							{item.action.label}
						</button>
					) : null}
				</div>
			))}
		</div>,
		document.body,
	);
}

// A toast's colour: brick for what went wrong; graphite for what is still
// going on or waits (a notice, D-207); sage for what went as asked.
function toneOf(tone) {
	if (tone == "seal" || tone == "problem") return "problem";
	if (tone == "working" || tone == "waiting") return "working";
	return "done";
}

// The toasts shown: those standing, and for a moment those just gone, so each
// can leave the way it came.
function useLeaving(items) {
	const [leaving, setLeaving] = useState([]);
	const previous = useRef(items);

	useEffect(() => {
		const gone = previous.current.filter(item => items.some(next => next.id == item.id) == false);
		previous.current = items;
		if (gone.length == 0) return undefined;
		setLeaving(list => [...list, ...gone.map(item => ({ ...item, isLeaving: true }))]);
		const timer = setTimeout(() => setLeaving(list => list.filter(item => gone.some(left => left.id == item.id) == false)), LEAVE_MS);
		return () => clearTimeout(timer);
	}, [items]);

	return [...items, ...leaving.filter(item => items.some(next => next.id == item.id) == false)]
		.sort((first, second) => second.id - first.id);
}
