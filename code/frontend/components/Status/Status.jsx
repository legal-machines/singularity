import styles from "./Status.module.css";

// One status per row (PRODUCT.md, "One status per row"), drawn as a mark and
// its words, in the order a member reads them:
// - yours: it waits for you (a judgment, an answer, an acceptance);
// - waiting: it waits for someone else;
// - underway: a member is at work on it;
// - delivered: a result is ready and waits for its acceptance;
// - settled: done, accepted, sent or closed with a conclusion;
// - due: a deadline near, or a point not checked;
// - failed: it stopped, and says what can be done - the one state drawn in
//   the brick that marks a real problem (D-174);
// - draft: nothing has left the firm.
// The words always say it; the mark only helps the eye find it.
export function Status({ kind, children, size = "small" }) {
	return (
		<span className={[styles.status, styles[`status_${kind}`], size == "medium" ? styles.medium : null].filter(Boolean).join(" ")}>
			<StatusMark kind={kind} />
			<span className={styles.words}>{children}</span>
		</span>
	);
}

export function StatusMark({ kind, size = 14 }) {
	return (
		<svg className={[styles.mark, styles[`mark_${kind}`]].join(" ")} width={size} height={size} viewBox="0 0 14 14" aria-hidden="true" focusable="false">
			{marks[kind] ?? marks.waiting}
		</svg>
	);
}

const marks = {
	yours: <><circle cx="7" cy="7" r="6" fill="var(--ink)" /><circle cx="7" cy="7" r="2.9" fill="none" stroke="#fff" strokeWidth="1.3" /></>,
	waiting: <circle cx="7" cy="7" r="5.25" fill="none" stroke="var(--ink-4)" strokeWidth="1.5" />,
	underway: <><circle cx="7" cy="7" r="5.25" fill="none" stroke="var(--underway-tint)" strokeWidth="1.75" /><path className="arc" d="M7 1.75A5.25 5.25 0 0 1 12.25 7" fill="none" stroke="var(--underway)" strokeWidth="1.75" strokeLinecap="round" /></>,
	delivered: <><circle cx="7" cy="7" r="5.25" fill="none" stroke="var(--ink-2)" strokeWidth="1.5" /><path d="M7 1.75A5.25 5.25 0 0 0 7 12.25Z" fill="var(--ink-2)" /></>,
	settled: <><circle cx="7" cy="7" r="6" fill="var(--settled)" /><path d="M4.4 7.2L6.2 9L9.7 5.2" fill="none" stroke="#fff" strokeWidth="1.5" strokeLinecap="round" strokeLinejoin="round" /></>,
	due: <><circle cx="7" cy="7" r="5.25" fill="none" stroke="var(--time)" strokeWidth="1.5" /><path d="M7 4.2V7.2L8.9 8.3" fill="none" stroke="var(--time)" strokeWidth="1.4" strokeLinecap="round" strokeLinejoin="round" /></>,
	failed: <><circle cx="7" cy="7" r="6" fill="var(--accent)" /><path d="M7 3.9V7.6" stroke="#fff" strokeWidth="1.6" strokeLinecap="round" /><circle cx="7" cy="10" r="0.95" fill="#fff" /></>,
	draft: <circle cx="7" cy="7" r="5.25" fill="none" stroke="var(--ink-4)" strokeWidth="1.5" strokeDasharray="2.2 2.2" />,
};

// A count beside a place or a group: brick when what it counts waits for
// the member, ink otherwise.
export function Count({ children, tone = "quiet", label }) {
	return (
		<span className={tone == "seal" ? `${styles.count} ${styles.count_seal}` : styles.count} aria-label={label} title={label}>
			{children}
		</span>
	);
}
