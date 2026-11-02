import { Icon } from "../Icon/Icon.jsx";

import styles from "./Tray.module.css";

// A tray (DESIGN.md, FOLIO-14): a faint grey holder with its heading in small
// capitals, what belongs beside the heading on its right, a white card with
// the content, and an optional line under the card. Lists of rows, facts and
// short notes all stand in trays, so every page reads in the same blocks.
export function Tray({ icon, label, count, countTone, aside, footer, children, className, bodyClassName, flush = false }) {
	return (
		<section className={[styles.tray, className].filter(Boolean).join(" ")} aria-label={typeof label == "string" ? label : undefined}>
			{label != null ? (
				<header className={styles.head}>
					{icon != null ? <span className={styles.icon}><Icon name={icon} size={16} /></span> : null}
					<h2 className={styles.label}>{label}</h2>
					{count != null ? <span className={countTone == "accent" ? `${styles.count} ${styles.count_accent}` : styles.count}>{count}</span> : null}
					{aside != null ? <span className={styles.aside}>{aside}</span> : null}
				</header>
			) : null}
			<div className={[styles.body, flush ? styles.flush : null, bodyClassName].filter(Boolean).join(" ")}>{children}</div>
			{footer != null ? <footer className={styles.foot}>{footer}</footer> : null}
		</section>
	);
}

// A row of a tray's list: a face or a mark, a line and the line under it,
// and what stands on its right. Pressed anywhere, it opens what it names;
// the controls on its right stay controls of their own.
export function Row({ lead, title, line, aside, onOpen, isSelected, wrap = false, children, ...rest }) {
	const text = (
		<span className={styles.row_text}>
			<span className={wrap ? `${styles.row_title} ${styles.row_title_wrap}` : styles.row_title}>{title}</span>
			{line != null ? <span className={styles.row_line}>{line}</span> : null}
			{children}
		</span>
	);

	return (
		<li className={styles.row_item} {...rest}>
			<div className={[styles.row, onOpen != null ? styles.row_open : null, isSelected ? styles.row_selected : null].filter(Boolean).join(" ")}>
				{lead != null ? <span className={styles.row_lead}>{lead}</span> : null}
				{onOpen != null ? (
					<button className={styles.row_button} type="button" aria-current={isSelected ? "true" : undefined} onClick={onOpen}>{text}</button>
				) : text}
				{aside != null ? <span className={styles.row_aside}>{aside}</span> : null}
			</div>
		</li>
	);
}

export function Rows({ children, label }) {
	return <ul className={styles.rows} aria-label={label}>{children}</ul>;
}

// A small heading inside a tray's card, over a group of its rows.
export function RowsHeading({ children }) {
	return <li className={styles.rows_heading} role="presentation">{children}</li>;
}

// Facts as a firm's file sets them: the name of each on the left, its value
// on the right, a fine line between.
// Facts read label beside value. A value of many words - a remit's list - is
// set from the left, under itself, never right-aligned across lines.
export function Facts({ items, isLong = false }) {
	return (
		<dl className={isLong ? `${styles.facts} ${styles.facts_long}` : styles.facts}>
			{items.filter(Boolean).map(item => (
				<div key={item.label} className={styles.fact}>
					<dt className={styles.fact_label}>{item.label}</dt>
					<dd className={styles.fact_value}>{item.value}</dd>
				</div>
			))}
		</dl>
	);
}
