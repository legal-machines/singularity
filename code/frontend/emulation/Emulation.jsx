import { useEffect, useRef, useState } from "react";

import { App } from "../App.jsx";
import { directorOfThisFrame } from "./director.js";

import styles from "./Emulation.module.css";

// The frame's page: the app itself, driven by its director, and the pointer
// the director moves.
export function Emulation() {
	const director = directorOfThisFrame();
	const [scene, setScene] = useState(null);
	useEffect(() => director.useScene(setScene), [director]);
	return (
		<div className={styles.feature} data-scene={scene}>
			<App emulation={director} />
			<Pointer director={director} />
		</div>
	);
}

// A pointer, as the system draws one, for the hand the script plays.
function Pointer({ director }) {
	const ref = useRef(null);
	useEffect(() => director.usePointer(ref.current), [director]);
	return (
		<span ref={ref} className={styles.pointer} aria-hidden="true">
			<svg width="22" height="22" viewBox="0 0 22 22">
				<path d="M4 2.5v15.2l3.9-3.7 2.6 5.8 2.7-1.2-2.6-5.7h5.4z" fill="#111" stroke="#fff" strokeWidth="1.3" strokeLinejoin="round" />
			</svg>
		</span>
	);
}
