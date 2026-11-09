import { members, viewerId } from "../../data/workspace.js";
import { emphasised, findMember, firstName } from "../../pages/words.js";
import { useShowPerson } from "../PersonCard/showPerson.js";

import styles from "./Mentions.module.css";

// Words with their @mentions set apart, in a thread's post or a chat's message,
// whoever wrote them, a person or an AI colleague: a member named with @ stands
// in medium weight, the member at this computer in the accent's ink, and a press
// on it opens that member's card. A model's emphasis, written as markdown, is
// drawn as emphasis and never as its asterisks. @all names everyone in the
// thread, so it stands in the accent for every reader.
export function Mentioned({ text }) {
	return emphasised(text).map((piece, index) => piece.isStrong
		? <strong key={index} className={styles.strong}><Named text={piece.text} /></strong>
		: <Named key={index} text={piece.text} />);
}

// Words with their @mentions alone, no emphasis read: for MessageText, which
// reads a message's markdown itself.
export function MentionedWords({ text }) {
	return <Named text={text} />;
}

function Named({ text }) {
	const viewer = firstName(findMember(viewerId));
	return text.split(/(@[A-Z][a-z]+|@[Aa][Ll][Ll](?![\p{L}\p{N}_]))/gu).map((part, index) => {
		if (!part.startsWith("@")) return part;
		// @all names everyone here, the reader among them.
		if (part.toLowerCase() == "@all") return <span key={index} className={`${styles.mention} ${styles.mention_you}`}>{part}</span>;
		const className = part.slice(1) == viewer ? `${styles.mention} ${styles.mention_you}` : styles.mention;
		const member = members.find(item => firstName(item) == part.slice(1));
		if (member == null) return <span key={index} className={className}>{part}</span>;
		return <Mention key={index} member={member} className={className}>{part}</Mention>;
	});
}

function Mention({ member, className, children }) {
	const show = useShowPerson();
	return (
		<button
			className={`${styles.mention_button} ${className}`}
			type="button"
			aria-haspopup="dialog"
			onClick={event => {
				event.stopPropagation();
				show(member.id, event.currentTarget);
			}}
		>
			{children}
		</button>
	);
}
