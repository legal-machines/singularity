// What an AI colleague says while it works on a reply that takes a while, in
// place of "is typing" (the owner, 2026-11-02: "no 'the agent is typing' in
// threads; if it thinks more than ten seconds, Singularity writes one of
// prepared messages - I'll look now, I'm studying it, one second, I'll tell
// you - with logic for which goes in answer to which request"; D-236). The
// line fits what was asked, in the language it was asked in, and is the same
// on every computer for the same request.

// After how long a colleague's reply in work is said in words.
export const HOLD_AFTER_MS = 10_000;

const LINES = {
	documents: {
		en: ["Going through the documents now.", "Reading it now - I'll come back with what matters.", "Looking at the documents; back shortly.",
			"Let me read it properly first.", "Checking it against the documents now.", "Reading the relevant sections now."],
		ru: ["Сейчас посмотрю документы.", "Изучаю документы, скоро вернусь.", "Читаю, сейчас расскажу, что важно.",
			"Сверяю с документами.", "Сейчас внимательно прочитаю.", "Смотрю нужные разделы."],
	},
	drafting: {
		en: ["Drafting it now.", "Working on the draft - give me a moment.", "Putting the draft together now.", "Writing it up now.",
			"On the draft now; I'll share it shortly."],
		ru: ["Готовлю проект.", "Пишу, дайте минуту.", "Собираю проект, скоро пришлю.", "Сейчас набросаю.", "Работаю над текстом."],
	},
	changes: {
		en: ["Making the changes now.", "Updating it now.", "Revising it now - back in a moment.", "On it - making those edits.", "Reworking it now."],
		ru: ["Вношу изменения.", "Сейчас поправлю.", "Дорабатываю, минуту.", "Обновляю.", "Переделываю, скоро будет."],
	},
	compare: {
		en: ["Comparing them now.", "Putting them side by side now.", "Checking where they differ.", "Lining them up now - back shortly."],
		ru: ["Сравниваю.", "Сейчас сопоставлю.", "Смотрю, в чём разница.", "Кладу рядом и сверяю."],
	},
	summary: {
		en: ["Pulling the key points together.", "Summarising it now.", "Reading it through for the main points.", "Condensing it now - a moment."],
		ru: ["Собираю главное.", "Сейчас коротко изложу.", "Выделяю суть.", "Сокращаю до главного, минуту."],
	},
	numbers: {
		en: ["Running the numbers now.", "Working through the figures.", "Calculating it now.", "Checking the figures - back in a moment."],
		ru: ["Считаю.", "Сейчас посчитаю.", "Проверяю цифры.", "Пересчитываю, минуту."],
	},
	find: {
		en: ["Looking for it now.", "Searching the Matter now.", "Finding it - one moment.", "Tracking it down now."],
		ru: ["Ищу.", "Сейчас найду.", "Ищу в материалах дела.", "Ищу, минуту."],
	},
	translate: {
		en: ["Translating it now.", "Working on the translation.", "Translating it now - a moment."],
		ru: ["Перевожу.", "Сейчас переведу.", "Перевожу, минуту."],
	},
	analysis: {
		en: ["Let me think this through.", "Thinking it over - I'll come back with a view.", "Working through it now.",
			"Looking into it - back shortly with a view.", "Weighing it up now."],
		ru: ["Сейчас подумаю.", "Обдумываю, скоро вернусь с позицией.", "Разбираюсь.", "Сейчас изучу вопрос.", "Взвешиваю, минуту."],
	},
	quick: {
		en: ["One second.", "Just a moment.", "Give me a second.", "One moment - I'll tell you.", "Coming right up."],
		ru: ["Секунду.", "Одну минуту.", "Секунду, сейчас расскажу.", "Сейчас отвечу.", "Минутку."],
	},
};

// What a request asks for, by its words, first match wins: the most specific
// work first, a short word last.
const INTENTS = [
	["translate", /\btranslat\w*|перевед|переведи|перевести/i],
	["drafting", /\b(draft|write|prepare|compose|redline|mark\s?up)\w*|составь|напиши|подготовь|набросай|сформулируй/i],
	// Asked to change something, not a "change of control" or an "amended" plan.
	["changes", /(^|[.!?]\s*|please\s+|can you\s+|could you\s+)(fix|change|update|edit|amend|revise|correct)\b|исправь|поправь|измени(те)?\b|обнови|доработай/i],
	["compare", /\b(compare|comparison|difference|differ|versus|vs\.?)\b|сравн|разниц|отлича/i],
	["summary", /\b(summar\w*|tl;?dr|key points|overview|brief)\b|кратко|резюм|суть|сводк|выжимк/i],
	["numbers", /\b(calculat\w*|how much|how many|total|sum|percent\w*|interest|days)\b|посчита|сколько|сумм|процент/i],
	["find", /\b(find|where|locate|look up|search)\b|найди|найти|где\b|поищи|ищи/i],
	["documents", /\b(document|contract|agreement|clause|section|file|attachment|nda|spa|review|check|read)\w*|документ|договор|соглашени|пункт|файл|провер|посмотри|прочитай|изучи/i],
	["analysis", /\b(risk\w*|why|should|can we|could we|analy\w*|assess\w*|opinion|advise|position|exposure)\b|риск|почему|можно ли|стоит ли|анализ|оцен|мнени|позици/i],
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
	return /[а-яё]/i.test(text ?? "") ? "ru" : "en";
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
