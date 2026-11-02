// What the member is naming as they type: an @ at the start of the words or
// after a space or an opening mark, and the letters typed after it up to the
// caret. Nothing while a stretch is selected, or once a space ends the name.
export function mentionAt(text, caret, selectionEnd = caret) {
	if (caret == null || caret != selectionEnd) return null;
	const before = (text ?? "").slice(0, caret);
	const match = before.match(/(^|[\s([{"'«“])@([\p{L}\p{M}'-]{0,30})$/u);
	if (match == null) return null;
	return { start: caret - match[2].length - 1, query: match[2] };
}

// Whom the typed letters can name, in the order the list shows them: "all"
// first where it is offered, then each member whose first name, whole name or
// any word of it begins with them, people and AI colleagues together. The
// word put into the message is the first name, as a message names a member.
export function mentionOptions(query, members, { allowsAll = false, allLine = "", firstNameOf, lineOf, most = 8 } = {}) {
	const typed = (query ?? "").toLocaleLowerCase();
	const options = [];
	if (allowsAll == true && "all".startsWith(typed)) options.push({ key: "all", member: null, label: "all", word: "all", line: allLine });
	for (const member of members ?? []) {
		const name = (member.name ?? "").trim();
		const words = name.toLocaleLowerCase().split(/\s+/).filter(Boolean);
		if (typed != "" && words.some(word => word.startsWith(typed)) == false) continue;
		options.push({ key: member.id, member, label: name, word: firstNameOf(member), line: lineOf(member) });
		if (options.length >= most) break;
	}
	return options;
}

// The words with the name typed so far replaced by the member's, and where the
// caret goes after it: past one space, which is added unless the words go on
// with a space or a closing mark already.
export function withMention(text, mention, caret, word) {
	const rest = text.slice(caret);
	const name = `@${word}`;
	if (/^\s/u.test(rest)) return { text: text.slice(0, mention.start) + name + rest, caret: mention.start + name.length + 1 };
	if (/^[,.;:!?)\]}»”]/u.test(rest)) return { text: text.slice(0, mention.start) + name + rest, caret: mention.start + name.length };
	return { text: text.slice(0, mention.start) + name + " " + rest, caret: mention.start + name.length + 1 };
}

// Whether a message names everyone with @all, as its own word.
export function namesAll(text) {
	return /(^|[^\p{L}\p{N}_])@all(?![\p{L}\p{N}_])/iu.test(text ?? "");
}
