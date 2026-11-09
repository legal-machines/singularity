// The reply box's formatting keys: the box keeps plain words, and a key puts
// the markdown marks around what is selected, which every message then draws
// as what they mean (MessageText) - or takes them away when they are already
// there. With nothing selected, the marks go in with the caret between them.
// The command key is Command on a Mac and Ctrl elsewhere.
export const FORMAT_KEYS = { KeyB: "**", KeyI: "_", KeyE: "`" };
export const SHIFT_FORMAT_KEYS = { KeyX: "~~" };

export function formatMarkOf(event, isMac) {
	const command = isMac ? event.metaKey : event.ctrlKey;
	if (command == false || event.altKey || (isMac && event.ctrlKey)) return null;
	return (event.shiftKey ? SHIFT_FORMAT_KEYS : FORMAT_KEYS)[event.code] ?? null;
}

// The words with `mark` put around the selection [start, end), or taken away
// from it, and the selection after: { from, to, insert, start, end } - the
// span replaced, what replaces it, and where the selection stands then.
export function toggledMark(value, start, end, mark) {
	const size = mark.length;
	// The marks stand just outside the selection: they go.
	if (start >= size && value.slice(start - size, start) == mark && value.slice(end, end + size) == mark
			&& (mark != "_" || (value[start - size - 1] != "_" && value[end + size] != "_"))
			&& (mark != "**" || (value[start - size - 1] != "*" && value[end + size] != "*"))) {
		return { from: start - size, to: end + size, insert: value.slice(start, end), start: start - size, end: end - size };
	}
	const picked = value.slice(start, end);
	// The selection holds the marks at its ends: they go.
	if (picked.length >= size * 2 && picked.startsWith(mark) && picked.endsWith(mark)) {
		const inner = picked.slice(size, picked.length - size);
		return { from: start, to: end, insert: inner, start, end: start + inner.length };
	}
	// The marks hug the words, never the spaces around them.
	const lead = /^\s*/.exec(picked)[0];
	const trail = picked.length > lead.length ? /\s*$/.exec(picked)[0] : "";
	const core = picked.slice(lead.length, picked.length - trail.length);
	const insert = `${lead}${mark}${core}${mark}${trail}`;
	const at = start + lead.length + size;
	return { from: start, to: end, insert, start: at, end: at + core.length };
}

// Whether the keys are a Mac's: there Command formats, elsewhere Ctrl.
export function isMacKeyboard() {
	return document.documentElement.dataset.nativePlatform == "darwin" || navigator.platform.startsWith("Mac");
}

// `mark` put around a text field's selection, or taken away from it, through
// the editor itself, so Undo takes it back in one step. `onChange` is given
// the words only where the editor cannot type them.
export function formatField(area, mark, onChange) {
	const change = toggledMark(area.value, area.selectionStart, area.selectionEnd, mark);
	area.focus();
	area.setSelectionRange(change.from, change.to);
	const isTyped = typeof document.execCommand == "function" && document.execCommand("insertText", false, change.insert) == true;
	if (isTyped == false) onChange(area.value.slice(0, change.from) + change.insert + area.value.slice(change.to));
	requestAnimationFrame(() => area.setSelectionRange(change.start, change.end));
}

// Whether the caret stands inside a code block begun with ``` and not yet
// closed: there Enter starts a new line, never sends.
export function isInCodeBlock(value, caret) {
	const before = value.slice(0, caret);
	return ((before.match(/^ {0,3}(```|~~~)/gm) ?? []).length % 2) == 1;
}
