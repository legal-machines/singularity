import { Button } from "../Button/Button.jsx";
import { FileTile, kindOf, wordOf } from "../FileTile/FileTile.jsx";

import styles from "./Result.module.css";

// What is inside a file, said in words under its name: "9 pages with tracked
// changes", "4 pages", "PDF document" where nothing more is known; for one a
// later version replaced, only that.
export function fileLine(file, replacedWord) {
	if (replacedWord != null) return replacedWord;
	const subject = file.pages != null ? `${file.pages} ${file.pages == 1 ? "page" : "pages"}` : wordOf(kindOf(file.name));
	return file.detail != null ? `${subject} ${file.detail}` : subject;
}

// A result as anyone in the firm hands it in, a person or an AI colleague
// alike: its files, as lawyers send them, and what its author did not check.
// A version a later one replaced stands back.
export function Result({ files, notChecked, replacedWord, onOpenFile }) {
	return (
		<div className={styles.result}>
			<div className={styles.files}>
				{files.map(file => (
					<FileTile
						key={file.name}
						name={file.name}
						line={fileLine(file, replacedWord)}
						size={file.size}
						isOld={replacedWord != null}
						onOpen={onOpenFile != null ? () => onOpenFile(file) : undefined}
					/>
				))}
			</div>
			{replacedWord == null ? <NotChecked text={notChecked} /> : null}
		</div>
	);
}

// What the member who handed files in did not check, under them.
export function NotChecked({ text }) {
	if (text == null || text == "") return null;
	return <p className={styles.unchecked}>Not checked: {text.replace(/\.$/, "")}</p>;
}

// A question a member asks with its answers under it, as an AI colleague asks
// what to do next: the others answer with a press, and the answer stands as
// their reply; the one who asked sees the answers it offered until then.
export function Answers({ options, answer, isOwn, onAnswer }) {
	if (answer != null) return null;
	if (isOwn) {
		return (
			<div className={styles.offered} aria-label="The answers you offered">
				{options.map(option => <span key={option} className={styles.offered_option}>{option}</span>)}
			</div>
		);
	}
	return (
		<div className={styles.answers}>
			{options.map((option, index) => (
				<Button key={option} variant={index == 0 ? "primary" : "secondary"} onClick={() => onAnswer(option)}>{option}</Button>
			))}
		</div>
	);
}
