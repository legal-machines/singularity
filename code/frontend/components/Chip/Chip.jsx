import styles from "./Chip.module.css";

// A chip says one state in a few words, in its tone. A status is a coloured
// word (the owner): no status dot the references do not have (DESIGN.md
// UI-17).
// - yours: it waits for the member - a decision, a review, an answer - in
//   plain ink, a little firmer than a fact, never an alarm (D-174);
// - accent: it failed or stopped, the brick that marks a real problem;
// - time: it is due, or something was not checked;
// - done: done, sent, closed;
// - work: a colleague is at work on it;
// - plain: a fact with no urgency, on white with a fine frame.
export function Chip({ tone = "plain", children }) {
	return <span className={`${styles.chip} ${styles[tone]}`}>{children}</span>;
}
