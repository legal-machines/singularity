import { Icon } from "../../components/Icon/Icon.jsx";

import styles from "./SettingsPage.module.css";

// Settings' rows in groups, each named in plain words above them.
export function Group({ title, line, children }) {
	return (
		<section className={styles.group}>
			<h2 className={styles.divider}>{title}</h2>
			{line != null ? <p className={styles.group_line}>{line}</p> : null}
			<ul className={styles.rows}>{children}</ul>
		</section>
	);
}

// One setting, as an Inbox row: its sign, what it is, its state, and the one
// thing to do with it. A row with no icon has no sign.
export function Setting({ icon, title, line, action }) {
	return (
		<li className={icon == null ? `${styles.row} ${styles.row_bare}` : styles.row}>
			{icon != null ? <span className={styles.sign} aria-hidden="true"><Icon name={icon} size={20} /></span> : null}
			<span className={styles.text}>
				<span className={styles.row_title}>{title}</span>
				{line != null && line != "" ? <span className={styles.line}>{line}</span> : null}
			</span>
			{action != null ? <span className={styles.action}>{action}</span> : null}
		</li>
	);
}
