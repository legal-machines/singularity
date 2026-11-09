import { useLayoutEffect, useRef, useState } from "react";

import { Confetti } from "./Confetti.jsx";
import { ColleagueOrbit } from "./entry/ColleagueOrbit.jsx";
import entry from "./entry/EntryForm.module.css";
import styles from "./Welcome.module.css";

// The moment the organisation is in: the painting is gone and the whole
// window says one thing, its name, the same after a registration, a sign-in
// or joining a firm. Then the tour shows the app and sets it up, or a member
// who has been here goes straight back to work. A registration just finished
// is celebrated with confetti.
export function Welcome({ name, celebrate = false, onContinue }) {
	const headingRef = useRef(null);
	const groupRef = useRef(null);
	const [ready, setReady] = useState(false);
	useLayoutEffect(() => {
		let disposed = false;
		let motion;
		const enter = () => {
			if (disposed) return;
			setReady(true);
			headingRef.current?.focus({ preventScroll: true });
			const calm = window.matchMedia("(prefers-reduced-motion: reduce)").matches;
			// Keep every text frame in its final position. Font metrics settle
			// before reveal, so a fallback font cannot change the line breaks.
			motion = groupRef.current?.animate([{ opacity: 0 }, { opacity: 1 }], {
				duration: calm ? 160 : 400, easing: "ease-out",
			});
		};
		Promise.all([document.fonts?.load('500 48px "Sora Welcome"'), document.fonts?.ready]).then(enter, enter);
		return () => { disposed = true; motion?.cancel(); };
	}, []);

	return (
		<section className={styles.welcome} aria-labelledby="onboarding-welcome">
			{/* The entry's own surface: its type, and its button's fill. */}
			<div className={`${entry.entry} ${styles.surface}`}>
				<ColleagueOrbit layout="welcome" />
				{celebrate ? <Confetti /> : null}
				<div className={styles.group} ref={groupRef} data-ready={ready || undefined} data-welcome-content>
					<h1 id="onboarding-welcome" className={styles.title} tabIndex={-1} ref={headingRef}>
						It’s your future{name ? <>, <span className={styles.company}>{name}</span></> : null}
					</h1>
					<button className={`${entry.primary} ${styles.action}`} type="button" onClick={onContinue}>
						<span className={entry.buttonLabel}>Get started</span>
					</button>
				</div>
			</div>
		</section>
	);
}
