import styles from "./Segmented.module.css";

// Views of one list, side by side in a sunk track; the one shown stands up
// as a white tab. Left and Right move between them.
export function Segmented({ label, options, value, onChange, size = "medium" }) {
	function onKeyDown(event) {
		if (event.key != "ArrowLeft" && event.key != "ArrowRight") return;
		event.preventDefault();
		const index = options.findIndex(option => option.key == value);
		const next = options[(index + (event.key == "ArrowRight" ? 1 : -1) + options.length) % options.length];
		onChange(next.key);
		event.currentTarget.querySelector(`[data-key="${next.key}"]`)?.focus();
	}

	return (
		<div className={size == "small" ? `${styles.track} ${styles.small}` : styles.track} role="tablist" aria-label={label} onKeyDown={onKeyDown}>
			{options.map(option => (
				<button
					key={option.key}
					className={option.key == value ? `${styles.option} ${styles.option_on}` : styles.option}
					type="button"
					role="tab"
					aria-selected={option.key == value}
					tabIndex={option.key == value ? 0 : -1}
					data-key={option.key}
					onClick={() => onChange(option.key)}
				>
					{option.icon}
					{option.label}
					{option.count != null && option.count > 0 ? <span className={styles.count}>{option.count}</span> : null}
				</button>
			))}
		</div>
	);
}
