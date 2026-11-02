import styles from "./Dots.module.css";

// Where the member is in a sequence, as Apple's page control marks it: a dot
// a step at the foot of the page, the current one dark, those passed a quieter
// grey (the owner, 2026-10-29). A passed step the member can still change
// takes them back to it; the page decides which those are.
export function Dots({ steps, current, label, canGo = () => false, onGo, className = "" }) {
	return (
		<nav className={`${styles.dots} ${className}`} aria-label={label}>
			{steps.map((title, at) => (
				<button
					key={title} type="button" className={styles.dot}
					data-passed={at < current || undefined}
					aria-current={at == current ? "step" : undefined}
					aria-label={`Step ${at + 1} of ${steps.length}: ${title}`}
					disabled={!canGo(at)}
					onClick={() => onGo?.(at)}
				/>
			))}
		</nav>
	);
}
