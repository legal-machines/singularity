import styles from "../../components/FileKind/FileKind.module.css";

// Outlook's simple mail mark (owner, 2026-10-30): blue @ on the same
// 26 × 32 canvas as the Office file marks, without an envelope or a badge.
export function OutlookIcon() {
	return <span className={`${styles.icon} ${styles.row}`} data-family="mail" aria-hidden="true">
		<svg viewBox="0 0 32 32" className={styles.drawing} fill="none" stroke="var(--outlook-ink, var(--kind-mail))" strokeWidth="3.2" strokeLinecap="round" strokeLinejoin="round">
			<circle cx="16" cy="16" r="5.25" />
			<path d="M21.25 10.75V17.5c0 2.4 1.1 3.75 3.1 3.75 2.65 0 4.15-2.15 4.15-5.75C28.5 8.6 22.9 3.5 16 3.5S3.5 9.1 3.5 16 9.1 28.5 16 28.5c3.4 0 6.4-1.2 8.8-3.4" />
		</svg>
	</span>;
}
