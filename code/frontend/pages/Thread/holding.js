// What an AI colleague says while it works on a reply that takes a while, in
// place of "is typing". The line fits what was asked, in the language it was
// asked in, and is the same on every computer for the same request.

// After how long a colleague's reply in work is said in words.
export const HOLD_AFTER_MS = 10_000;

const LINES = {
	documents: {
		en: ["Going through the documents now.", "Reading it now - I'll come back with what matters.", "Looking at the documents; back shortly.",
			"Let me read it properly first.", "Checking it against the documents now.", "Reading the relevant sections now."],
		ru: ["\u0421\u0435\u0439\u0447\u0430\u0441 \u043f\u043e\u0441\u043c\u043e\u0442\u0440\u044e \u0434\u043e\u043a\u0443\u043c\u0435\u043d\u0442\u044b.", "\u0418\u0437\u0443\u0447\u0430\u044e \u0434\u043e\u043a\u0443\u043c\u0435\u043d\u0442\u044b, \u0441\u043a\u043e\u0440\u043e \u0432\u0435\u0440\u043d\u0443\u0441\u044c.", "\u0427\u0438\u0442\u0430\u044e, \u0441\u0435\u0439\u0447\u0430\u0441 \u0440\u0430\u0441\u0441\u043a\u0430\u0436\u0443, \u0447\u0442\u043e \u0432\u0430\u0436\u043d\u043e.",
			"\u0421\u0432\u0435\u0440\u044f\u044e \u0441 \u0434\u043e\u043a\u0443\u043c\u0435\u043d\u0442\u0430\u043c\u0438.", "\u0421\u0435\u0439\u0447\u0430\u0441 \u0432\u043d\u0438\u043c\u0430\u0442\u0435\u043b\u044c\u043d\u043e \u043f\u0440\u043e\u0447\u0438\u0442\u0430\u044e.", "\u0421\u043c\u043e\u0442\u0440\u044e \u043d\u0443\u0436\u043d\u044b\u0435 \u0440\u0430\u0437\u0434\u0435\u043b\u044b."],
	},
	drafting: {
		en: ["Drafting it now.", "Working on the draft - give me a moment.", "Putting the draft together now.", "Writing it up now.",
			"On the draft now; I'll share it shortly."],
		ru: ["\u0413\u043e\u0442\u043e\u0432\u043b\u044e \u043f\u0440\u043e\u0435\u043a\u0442.", "\u041f\u0438\u0448\u0443, \u0434\u0430\u0439\u0442\u0435 \u043c\u0438\u043d\u0443\u0442\u0443.", "\u0421\u043e\u0431\u0438\u0440\u0430\u044e \u043f\u0440\u043e\u0435\u043a\u0442, \u0441\u043a\u043e\u0440\u043e \u043f\u0440\u0438\u0448\u043b\u044e.", "\u0421\u0435\u0439\u0447\u0430\u0441 \u043d\u0430\u0431\u0440\u043e\u0441\u0430\u044e.", "\u0420\u0430\u0431\u043e\u0442\u0430\u044e \u043d\u0430\u0434 \u0442\u0435\u043a\u0441\u0442\u043e\u043c."],
	},
	changes: {
		en: ["Making the changes now.", "Updating it now.", "Revising it now - back in a moment.", "On it - making those edits.", "Reworking it now."],
		ru: ["\u0412\u043d\u043e\u0448\u0443 \u0438\u0437\u043c\u0435\u043d\u0435\u043d\u0438\u044f.", "\u0421\u0435\u0439\u0447\u0430\u0441 \u043f\u043e\u043f\u0440\u0430\u0432\u043b\u044e.", "\u0414\u043e\u0440\u0430\u0431\u0430\u0442\u044b\u0432\u0430\u044e, \u043c\u0438\u043d\u0443\u0442\u0443.", "\u041e\u0431\u043d\u043e\u0432\u043b\u044f\u044e.", "\u041f\u0435\u0440\u0435\u0434\u0435\u043b\u044b\u0432\u0430\u044e, \u0441\u043a\u043e\u0440\u043e \u0431\u0443\u0434\u0435\u0442."],
	},
	compare: {
		en: ["Comparing them now.", "Putting them side by side now.", "Checking where they differ.", "Lining them up now - back shortly."],
		ru: ["\u0421\u0440\u0430\u0432\u043d\u0438\u0432\u0430\u044e.", "\u0421\u0435\u0439\u0447\u0430\u0441 \u0441\u043e\u043f\u043e\u0441\u0442\u0430\u0432\u043b\u044e.", "\u0421\u043c\u043e\u0442\u0440\u044e, \u0432 \u0447\u0451\u043c \u0440\u0430\u0437\u043d\u0438\u0446\u0430.", "\u041a\u043b\u0430\u0434\u0443 \u0440\u044f\u0434\u043e\u043c \u0438 \u0441\u0432\u0435\u0440\u044f\u044e."],
	},
	summary: {
		en: ["Pulling the key points together.", "Summarising it now.", "Reading it through for the main points.", "Condensing it now - a moment."],
		ru: ["\u0421\u043e\u0431\u0438\u0440\u0430\u044e \u0433\u043b\u0430\u0432\u043d\u043e\u0435.", "\u0421\u0435\u0439\u0447\u0430\u0441 \u043a\u043e\u0440\u043e\u0442\u043a\u043e \u0438\u0437\u043b\u043e\u0436\u0443.", "\u0412\u044b\u0434\u0435\u043b\u044f\u044e \u0441\u0443\u0442\u044c.", "\u0421\u043e\u043a\u0440\u0430\u0449\u0430\u044e \u0434\u043e \u0433\u043b\u0430\u0432\u043d\u043e\u0433\u043e, \u043c\u0438\u043d\u0443\u0442\u0443."],
	},
	numbers: {
		en: ["Running the numbers now.", "Working through the figures.", "Calculating it now.", "Checking the figures - back in a moment."],
		ru: ["\u0421\u0447\u0438\u0442\u0430\u044e.", "\u0421\u0435\u0439\u0447\u0430\u0441 \u043f\u043e\u0441\u0447\u0438\u0442\u0430\u044e.", "\u041f\u0440\u043e\u0432\u0435\u0440\u044f\u044e \u0446\u0438\u0444\u0440\u044b.", "\u041f\u0435\u0440\u0435\u0441\u0447\u0438\u0442\u044b\u0432\u0430\u044e, \u043c\u0438\u043d\u0443\u0442\u0443."],
	},
	find: {
		en: ["Looking for it now.", "Searching the Matter now.", "Finding it - one moment.", "Tracking it down now."],
		ru: ["\u0418\u0449\u0443.", "\u0421\u0435\u0439\u0447\u0430\u0441 \u043d\u0430\u0439\u0434\u0443.", "\u0418\u0449\u0443 \u0432 \u043c\u0430\u0442\u0435\u0440\u0438\u0430\u043b\u0430\u0445 \u0434\u0435\u043b\u0430.", "\u0418\u0449\u0443, \u043c\u0438\u043d\u0443\u0442\u0443."],
	},
	translate: {
		en: ["Translating it now.", "Working on the translation.", "Translating it now - a moment."],
		ru: ["\u041f\u0435\u0440\u0435\u0432\u043e\u0436\u0443.", "\u0421\u0435\u0439\u0447\u0430\u0441 \u043f\u0435\u0440\u0435\u0432\u0435\u0434\u0443.", "\u041f\u0435\u0440\u0435\u0432\u043e\u0436\u0443, \u043c\u0438\u043d\u0443\u0442\u0443."],
	},
	analysis: {
		en: ["Let me think this through.", "Thinking it over - I'll come back with a view.", "Working through it now.",
			"Looking into it - back shortly with a view.", "Weighing it up now."],
		ru: ["\u0421\u0435\u0439\u0447\u0430\u0441 \u043f\u043e\u0434\u0443\u043c\u0430\u044e.", "\u041e\u0431\u0434\u0443\u043c\u044b\u0432\u0430\u044e, \u0441\u043a\u043e\u0440\u043e \u0432\u0435\u0440\u043d\u0443\u0441\u044c \u0441 \u043f\u043e\u0437\u0438\u0446\u0438\u0435\u0439.", "\u0420\u0430\u0437\u0431\u0438\u0440\u0430\u044e\u0441\u044c.", "\u0421\u0435\u0439\u0447\u0430\u0441 \u0438\u0437\u0443\u0447\u0443 \u0432\u043e\u043f\u0440\u043e\u0441.", "\u0412\u0437\u0432\u0435\u0448\u0438\u0432\u0430\u044e, \u043c\u0438\u043d\u0443\u0442\u0443."],
	},
	quick: {
		en: ["One second.", "Just a moment.", "Give me a second.", "One moment - I'll tell you.", "Coming right up."],
		ru: ["\u0421\u0435\u043a\u0443\u043d\u0434\u0443.", "\u041e\u0434\u043d\u0443 \u043c\u0438\u043d\u0443\u0442\u0443.", "\u0421\u0435\u043a\u0443\u043d\u0434\u0443, \u0441\u0435\u0439\u0447\u0430\u0441 \u0440\u0430\u0441\u0441\u043a\u0430\u0436\u0443.", "\u0421\u0435\u0439\u0447\u0430\u0441 \u043e\u0442\u0432\u0435\u0447\u0443.", "\u041c\u0438\u043d\u0443\u0442\u043a\u0443."],
	},
};

// What a request asks for, by its words, first match wins: the most specific
// work first, a short word last.
const INTENTS = [
	["translate", /\btranslat\w*|\u043f\u0435\u0440\u0435\u0432\u0435\u0434|\u043f\u0435\u0440\u0435\u0432\u0435\u0434\u0438|\u043f\u0435\u0440\u0435\u0432\u0435\u0441\u0442\u0438/i],
	["drafting", /\b(draft|write|prepare|compose|redline|mark\s?up)\w*|\u0441\u043e\u0441\u0442\u0430\u0432\u044c|\u043d\u0430\u043f\u0438\u0448\u0438|\u043f\u043e\u0434\u0433\u043e\u0442\u043e\u0432\u044c|\u043d\u0430\u0431\u0440\u043e\u0441\u0430\u0439|\u0441\u0444\u043e\u0440\u043c\u0443\u043b\u0438\u0440\u0443\u0439/i],
	// Asked to change something, not a "change of control" or an "amended" plan.
	["changes", /(^|[.!?]\s*|please\s+|can you\s+|could you\s+)(fix|change|update|edit|amend|revise|correct)\b|\u0438\u0441\u043f\u0440\u0430\u0432\u044c|\u043f\u043e\u043f\u0440\u0430\u0432\u044c|\u0438\u0437\u043c\u0435\u043d\u0438(\u0442\u0435)?\b|\u043e\u0431\u043d\u043e\u0432\u0438|\u0434\u043e\u0440\u0430\u0431\u043e\u0442\u0430\u0439/i],
	["compare", /\b(compare|comparison|difference|differ|versus|vs\.?)\b|\u0441\u0440\u0430\u0432\u043d|\u0440\u0430\u0437\u043d\u0438\u0446|\u043e\u0442\u043b\u0438\u0447\u0430/i],
	["summary", /\b(summar\w*|tl;?dr|key points|overview|brief)\b|\u043a\u0440\u0430\u0442\u043a\u043e|\u0440\u0435\u0437\u044e\u043c|\u0441\u0443\u0442\u044c|\u0441\u0432\u043e\u0434\u043a|\u0432\u044b\u0436\u0438\u043c\u043a/i],
	["numbers", /\b(calculat\w*|how much|how many|total|sum|percent\w*|interest|days)\b|\u043f\u043e\u0441\u0447\u0438\u0442\u0430|\u0441\u043a\u043e\u043b\u044c\u043a\u043e|\u0441\u0443\u043c\u043c|\u043f\u0440\u043e\u0446\u0435\u043d\u0442/i],
	["find", /\b(find|where|locate|look up|search)\b|\u043d\u0430\u0439\u0434\u0438|\u043d\u0430\u0439\u0442\u0438|\u0433\u0434\u0435\b|\u043f\u043e\u0438\u0449\u0438|\u0438\u0449\u0438/i],
	["documents", /\b(document|contract|agreement|clause|section|file|attachment|nda|spa|review|check|read)\w*|\u0434\u043e\u043a\u0443\u043c\u0435\u043d\u0442|\u0434\u043e\u0433\u043e\u0432\u043e\u0440|\u0441\u043e\u0433\u043b\u0430\u0448\u0435\u043d\u0438|\u043f\u0443\u043d\u043a\u0442|\u0444\u0430\u0439\u043b|\u043f\u0440\u043e\u0432\u0435\u0440|\u043f\u043e\u0441\u043c\u043e\u0442\u0440\u0438|\u043f\u0440\u043e\u0447\u0438\u0442\u0430\u0439|\u0438\u0437\u0443\u0447\u0438/i],
	["analysis", /\b(risk\w*|why|should|can we|could we|analy\w*|assess\w*|opinion|advise|position|exposure)\b|\u0440\u0438\u0441\u043a|\u043f\u043e\u0447\u0435\u043c\u0443|\u043c\u043e\u0436\u043d\u043e \u043b\u0438|\u0441\u0442\u043e\u0438\u0442 \u043b\u0438|\u0430\u043d\u0430\u043b\u0438\u0437|\u043e\u0446\u0435\u043d|\u043c\u043d\u0435\u043d\u0438|\u043f\u043e\u0437\u0438\u0446\u0438/i],
];

export function intentOf(text, hasFiles = false) {
	const words = (text ?? "").trim();
	for (const [intent, pattern] of INTENTS) {
		if (intent == "documents" && hasFiles) return "documents";
		if (pattern.test(words)) return intent;
	}
	if (hasFiles) return "documents";
	return words.length < 60 ? "quick" : "analysis";
}

export function languageOf(text) {
	return /[\u0430-\u044f\u0451]/i.test(text ?? "") ? "ru" : "en";
}

// A small, steady number for a text: the same request picks the same line.
function seedOf(text) {
	let hash = 2166136261;
	for (const character of text ?? "") {
		hash ^= character.codePointAt(0);
		hash = Math.imul(hash, 16777619) >>> 0;
	}
	return hash;
}

// The line a colleague says while it works on `request` ({ id, text,
// hasFiles }).
export function holdingLine(request, colleagueId = "") {
	const text = request?.text ?? "";
	const lines = LINES[intentOf(text, request?.hasFiles == true)][languageOf(text)];
	return lines[seedOf(`${request?.id ?? text}\n${colleagueId}`) % lines.length];
}
