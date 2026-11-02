import { useEffect, useRef } from "react";

import { mountEntryPainting } from "./mountPainting.js";
import logoMark from "../assets/logo-mark-reverse.svg";
import styles from "./Painting.module.css";

const poster = new URL("./poster.webp", import.meta.url).href;

// The living painting from the site: a still poster, and over it the same
// painting drawn live, its figures turning to the pointer. Space pauses it,
// R replays it; with reduced motion the poster stays. Its credit stands in the
// corner: the artist and the work, without the museum, since this painting is
// our adaptation, not the museum's (the owner, 2026-10-26).
export function Painting() {
	const panel = useRef(null);

	useEffect(() => mountEntryPainting(panel.current), []);

	return (
		<figure ref={panel} className={styles.art}>
			<img
				className={styles.poster}
				src={poster}
				alt="A painterly interpretation of Pieter Brueghel the Younger, The Village Lawyer, 1621: a crowded legal office filled with papers, clients and clerks."
			/>
			<canvas
				className={styles.live}
				aria-hidden="true"
				tabIndex={0}
				role="img"
				aria-label="Animated Village Lawyer. Move the pointer to explore. Press Space to pause and R to replay."
			/>
			<div className={styles.brand}>
				<img src={logoMark} alt="" width="20" height="20" />
				<span>Legal Machines</span>
			</div>
			<figcaption className={styles.label}>
				<span className={styles.artist}>Pieter Brueghel the Younger</span>
				<cite className={styles.piece}>Village Lawyer, 1621</cite>
			</figcaption>
		</figure>
	);
}
