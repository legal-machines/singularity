import styles from "./Badges.module.css";

// What waits unread, in Legal Machines' brick: an "@" square where something
// names the member, and the count beside it. Nothing when nothing waits.
export function Badges({ count = 0, namesViewer = false, label }) {
	if (count <= 0 && namesViewer != true) return null;
	const said = [namesViewer ? "names you" : null, count > 0 ? `${count} unread` : null].filter(Boolean).join(", ");
	return (
		<span className={styles.badges} aria-label={label != null ? `${label}: ${said}` : said}>
			{namesViewer ? <span className={`${styles.badge} ${styles.at}`} aria-hidden="true">@</span> : null}
			{count > 0 ? <span className={styles.badge} aria-hidden="true">{count > 99 ? "99+" : count}</span> : null}
		</span>
	);
}
