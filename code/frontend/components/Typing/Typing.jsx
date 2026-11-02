import styles from "./Typing.module.css";

// A member writing its answer, as a messenger shows it (the owner, 2026-10-26:
// "the name plus typing, with the running dots, as is usual"): the word, with
// whose it is where the place does not say it already, and three dots that run
// one after another - still under reduced motion.
export function Typing({ who, isMany = false }) {
	return (
		<span className={styles.typing}>
			{who != null && who != "" ? `${who} ${isMany == true ? "are" : "is"} typing` : "typing"}
			<span className={styles.dots} aria-hidden="true">
				<span>.</span>
				<span>.</span>
				<span>.</span>
			</span>
		</span>
	);
}
