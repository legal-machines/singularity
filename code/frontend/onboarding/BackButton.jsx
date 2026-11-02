import styles from "./entry/EntryForm.module.css";

// A quiet navigation control with a full hit target and an accessible name.
export function BackButton({ onClick, disabled = false, className = "" }) {
	return (
		<button className={`${styles.back} ${className}`} type="button" aria-label="Back" title="Back" disabled={disabled} onClick={onClick}>
			<svg width="16" height="16" viewBox="0 0 20 20" fill="none" aria-hidden="true" focusable="false">
				<path d="m12 4-6 6 6 6" stroke="currentColor" strokeWidth="1.75" strokeLinecap="round" strokeLinejoin="round" />
			</svg>
			<span>Back</span>
		</button>
	);
}
