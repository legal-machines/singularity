// What a paste hands over: the files copied with it - a picture, or a file
// copied in Finder or Explorer - which go with the message as a dropped file
// does; and its words, without the object marks a copied file leaves in them
// (U+FFFC, which the system draws as a broken "OBJ").
export function pastedFiles(event) {
	return [...(event.clipboardData?.files ?? [])];
}

const OBJECT_MARKS = /\uFFFC/g;

// The clipboard's words pasted without their object marks where they hold
// any, as typed at the cursor (so undo takes them back); whether it did.
export function pasteWordsWithoutObjects(event) {
	const text = event.clipboardData?.getData("text/plain") ?? "";
	if (text.includes("\uFFFC") == false) return false;
	event.preventDefault();
	const words = text.replace(OBJECT_MARKS, "").trim();
	if (words != "") document.execCommand("insertText", false, words);
	return true;
}
