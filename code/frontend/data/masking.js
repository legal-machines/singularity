import { nativeApp } from "./native.js";

// What Papinian Guard masks before an AI provider reads it. The previews are
// this computer's Guard run on the text with the Project's identities;
// nothing is sent, and what comes back lives only in this window's memory.

// Each text asked once while the window is open, the latest few hundred kept.
const asked = new Map();
const MOST = 400;

// A message or a draft as it would go out from `projectId`:
// { isOn, protectedText, replacements: [{ placeholder, category, forms }] }.
export function maskText(projectId, text) {
	const key = `${projectId}\n${text}`;
	const known = asked.get(key);
	if (known != null) return known;
	const answer = nativeApp().previewProtectedPassage(projectId, text).then(result => ({
		isOn: result?.protectionMode == "on",
		protectedText: typeof result?.protectedText == "string" ? result.protectedText : text,
		replacements: (result?.replacements ?? []).filter(entry => (entry.placeholder ?? "") != ""),
	}));
	answer.catch(() => asked.delete(key));
	asked.set(key, answer);
	if (asked.size > MOST) asked.delete(asked.keys().next().value);
	return answer;
}

// Whether client data protection masks what leaves this computer
// (protectedApi.getPolicy). Protection masks words, not pictures: a picture a
// member attaches then goes to the AI model as it is, and says so before it
// goes. Unknown counts as on, so the note is never missing.
export async function isProtectionOn() {
	try {
		const policy = await nativeApp().getProtectedApiPolicy?.();
		return (policy?.mode ?? "standard") != "off";
	} catch {
		return true;
	}
}

// A whole document as a model receives it (protectedApi.previewDocument):
// asked once at a time, and its last answer kept, so a document opened again
// shows at once while it is asked again. Singularity keeps the answer ready
// for a document that has not changed, so asking again is quick.
const documents = new Map();

export function maskDocument(projectId, documentId) {
	const key = `${projectId}\n${documentId}`;
	const known = documents.get(key);
	if (known?.asking != null) return known.asking;
	const asking = nativeApp().previewProtectedDocument(projectId, documentId).then(result => {
		documents.set(key, { result });
		return result;
	}, error => {
		documents.set(key, { result: documents.get(key)?.result ?? null });
		throw error;
	});
	documents.set(key, { asking, result: known?.result ?? null });
	return asking;
}

// The document's last masked reading in this window, or null.
export function keptDocumentMask(projectId, documentId) {
	return documents.get(`${projectId}\n${documentId}`)?.result ?? null;
}

const escaped = text => text.replace(/[.*+?^${}()|[\]\\]/g, "\\$&");

// The text as it goes out: its words, and each symbol where its words stood,
// with the words it stands for. A symbol stands whole ([[Org1]]) or as one
// of its parts - an organisation's name written alone ([[Org1.short]]), a
// figure in words ([[Amount2.words]]) - and is the same entry either way.
const SYMBOL_PART = /\.[A-Za-z]+\]\]$/;

export function sentSegments(protectedText, replacements) {
	const byTag = new Map((replacements ?? []).map(entry => [entry.placeholder, entry]));
	if (byTag.size == 0 || (protectedText ?? "") == "") return [{ text: protectedText ?? "" }];
	const choices = [...byTag.keys()].sort((first, second) => second.length - first.length).map(tag => {
		const name = /^\[\[(.+)\]\]$/.exec(tag)?.[1];
		return name == null ? escaped(tag) : `\\[\\[${escaped(name)}(?:\\.[A-Za-z]+)?\\]\\]`;
	});
	const pattern = new RegExp(choices.join("|"), "g");
	const segments = [];
	let last = 0;
	for (const match of protectedText.matchAll(pattern)) {
		if (match.index > last) segments.push({ text: protectedText.slice(last, match.index) });
		const entry = byTag.get(match[0]) ?? byTag.get(match[0].replace(SYMBOL_PART, "]]"));
		segments.push({ tag: match[0], category: entry.category, words: entry.forms?.[0] ?? "" });
		last = match.index + match[0].length;
	}
	if (last < protectedText.length) segments.push({ text: protectedText.slice(last) });
	return segments;
}

const WORD_CHARACTER = /[\p{L}\p{N}]/u;

// The text as written, each spelling the Guard masks marked with the symbol
// it goes out as. A spelling counts only as a whole word: "Ann" is never the
// start of "Annual".
export function writtenSegments(text, replacements) {
	const byForm = new Map();
	for (const entry of replacements ?? []) {
		for (const form of entry.forms ?? []) {
			if (form.trim() != "" && byForm.has(form) == false) byForm.set(form, entry);
		}
	}
	if (byForm.size == 0 || (text ?? "") == "") return [{ text: text ?? "" }];
	const pattern = new RegExp([...byForm.keys()].sort((first, second) => second.length - first.length).map(escaped).join("|"), "gu");
	const segments = [];
	let last = 0;
	for (const match of text.matchAll(pattern)) {
		const end = match.index + match[0].length;
		const before = match.index > 0 ? text[match.index - 1] : "";
		const after = end < text.length ? text[end] : "";
		const isWhole = (before == "" || WORD_CHARACTER.test(before) == false || WORD_CHARACTER.test(match[0][0]) == false)
			&& (after == "" || WORD_CHARACTER.test(after) == false || WORD_CHARACTER.test(match[0][match[0].length - 1]) == false);
		if (isWhole == false) continue;
		if (match.index > last) segments.push({ text: text.slice(last, match.index) });
		const entry = byForm.get(match[0]);
		segments.push({ words: match[0], tag: entry.placeholder, category: entry.category });
		last = end;
	}
	if (last < text.length) segments.push({ text: text.slice(last) });
	return segments;
}

// The kind of value each of the Guard's categories is, in a word.
const KINDS = {
	PERSON: ["person", "people"], JUDGE: ["person", "people"],
	LEGAL_ENTITY: ["company", "companies"], ORGANISATION: ["organisation", "organisations"], COURT: ["court", "courts"],
	LOCATION: ["place", "places"], ADDRESS: ["address", "addresses"],
	EMAIL: ["email address", "email addresses"], PHONE: ["phone number", "phone numbers"],
	BANK_ACCOUNT: ["bank account", "bank accounts"], TAX_ID: ["tax number", "tax numbers"],
	CASE_NUMBER: ["case number", "case numbers"], CONTRACT_IDENTIFIER: ["contract number", "contract numbers"],
	DATE: ["date", "dates"], MONEY_AMOUNT: ["amount", "amounts"], PERCENTAGE: ["percentage", "percentages"], NUMBER: ["number", "numbers"],
};

// A symbol's kind in a word or two, for the table of what is masked.
export function kindWord(category) {
	return KINDS[category]?.[0] ?? "value";
}
