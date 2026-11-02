import styles from "./Parts.module.css";

// A page's parts, one under another, each a panel in Apple's grouped manner
// (the owner, 2026-11-02): a person's or a colleague's profile, a Matter. The
// page's own words in a panel use `parts.paragraph`, `parts.paragraph_quiet`
// and `parts.paragraph_lines`.
export const parts = styles;

export function Stack({ children }) {
	return <div className={styles.stack}>{children}</div>;
}

// A part of the page: its name in plain words, with its count and, on its
// right, what can be done with it, over a panel that holds it - no frame, no
// capitals, a soft ground and round corners.
export function Part({ title, count, aside, children }) {
	return (
		<section className={styles.part} aria-label={typeof title == "string" ? title : undefined}>
			<div className={styles.part_head}>
				<h2 className={styles.part_title}>{title}{count != null ? <span className={styles.part_count}>{count}</span> : null}</h2>
				{aside != null ? <span className={styles.part_aside}>{aside}</span> : null}
			</div>
			<div className={styles.panel}>{children}</div>
		</section>
	);
}

export function PartRows({ children }) {
	return <ul className={styles.rows}>{children}</ul>;
}

// A row of a part's list, as Team's: what it names and the line under it,
// with no line between rows; pressed, it opens what it names, its ground
// showing under the pointer. What stands on its right stays a control.
export function PartRow({ lead, title, line, aside, onOpen, wrap = false }) {
	const text = (
		<>
			{lead != null ? <span className={styles.row_lead}>{lead}</span> : null}
			<span className={styles.row_text}>
				<span className={wrap ? `${styles.row_title} ${styles.row_wrap}` : styles.row_title}>{title}</span>
				{line != null ? <span className={wrap ? `${styles.row_line} ${styles.row_wrap}` : styles.row_line}>{line}</span> : null}
			</span>
		</>
	);
	return (
		<li className={onOpen != null ? `${styles.row} ${styles.row_opens}` : styles.row}>
			{onOpen != null ? <button className={styles.row_open} type="button" onClick={onOpen}>{text}</button> : <span className={styles.row_open}>{text}</span>}
			{aside != null ? <span className={styles.row_aside}>{aside}</span> : null}
		</li>
	);
}

// What is kept on file: each thing's name on the left, what it is on the
// right of it, with no lines between them.
export function FactList({ items, isLong = false }) {
	return (
		<dl className={isLong ? `${styles.facts} ${styles.facts_long}` : styles.facts}>
			{items.filter(Boolean).map(item => (
				<div key={item.label} className={styles.fact}>
					<dt>{item.label}</dt>
					<dd>{item.value}</dd>
				</div>
			))}
		</dl>
	);
}

// A state said as a word in its own colour, never in a frame.
export function Word({ tone = "plain", children }) {
	return <span className={styles.word} data-tone={tone}>{children}</span>;
}
