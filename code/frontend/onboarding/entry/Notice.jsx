import { useEffect, useLayoutEffect, useRef, useState } from "react";
import styles from "./Notice.module.css";

// How long a passing notice stays: long enough to read, never lingering.
const STAY_MS = { least: 2600, most: 6000, base: 1400, perCharacter: 60 };
// The leave, as in Notice.module.css.
const LEAVE_MS = 220;

// The G2 outline of a w × h box (styles/tokens.css, --g2): each corner eases
// out of its edge over 19.2 pixels, as the buttons' and cards' corners do.
// One path at the notice's own size, so no slice seams show while it moves.
const CORNER = 19.2;
const g2Path = (w, h) =>
	`M${w - CORNER} 0c6.721 0 10.081 0 12.648 1.308a12 12 0 0 1 5.244 5.244c1.308 2.567 1.308 5.927 1.308 12.648` +
	`L${w} ${h - CORNER}c0 6.721 0 10.081 -1.308 12.648a12 12 0 0 1 -5.244 5.244c-2.567 1.308 -5.927 1.308 -12.648 1.308` +
	`L${CORNER} ${h}c-6.721 0 -10.081 0 -12.648 -1.308a12 12 0 0 1 -5.244 -5.244c-1.308 -2.567 -1.308 -5.927 -1.308 -12.648` +
	`L0 ${CORNER}c0 -6.721 0 -10.081 1.308 -12.648a12 12 0 0 1 5.244 -5.244c2.567 -1.308 5.927 -1.308 12.648 -1.308Z`;

// What the entry says at its top, under the window's buttons, never as text
// under a field (the owner, 2026-10-30): what went wrong ("problem"), what to
// mind ("warning") or what just happened ("done"). `message` is
// { key, text, tone, stays } or null. One that stays remains until it is taken
// back, as an error until its field changes; any other goes by itself, once.
// A new key says the same words again. It stands over the page and never
// moves it.
export function Notice({ message }) {
	const [shown, setShown] = useState(null);
	const [leaving, setLeaving] = useState(false);
	const [size, setSize] = useState(null);
	const passed = useRef(new Set());
	const box = useRef(null);
	const current = message && !(!message.stays && passed.current.has(message.key)) ? message : null;
	const key = current?.key ?? null;

	useEffect(() => {
		if (!current) {
			setLeaving(true);
			return undefined;
		}
		setShown(current);
		setLeaving(false);
		if (current.stays) return undefined;
		const stay = Math.min(STAY_MS.most, Math.max(STAY_MS.least, STAY_MS.base + current.text.length * STAY_MS.perCharacter));
		const timer = setTimeout(() => setLeaving(true), stay);
		// A passing notice is said once: replaced or gone, it does not return.
		return () => {
			clearTimeout(timer);
			passed.current.add(current.key);
		};
	}, [key]);

	useEffect(() => {
		if (!leaving) return undefined;
		const timer = setTimeout(() => setShown(null), LEAVE_MS);
		return () => clearTimeout(timer);
	}, [leaving]);

	// Its ground is drawn at its laid-out size, before it is painted.
	useLayoutEffect(() => {
		const element = box.current;
		setSize(element ? { w: element.offsetWidth, h: element.offsetHeight } : null);
	}, [shown?.key]);

	return (
		<div className={styles.notices} role="status" aria-live="polite">
			{shown ? (
				<p ref={box} key={shown.key} id="entry-notice" className={styles.notice} data-tone={shown.tone} data-leaving={leaving || undefined}>
					{size ? (
						<svg className={styles.ground} width={size.w} height={size.h} viewBox={`0 0 ${size.w} ${size.h}`} aria-hidden="true" focusable="false">
							<path d={g2Path(size.w, size.h)} />
						</svg>
					) : null}
					<span className={styles.text}>{shown.text}</span>
				</p>
			) : null}
		</div>
	);
}
