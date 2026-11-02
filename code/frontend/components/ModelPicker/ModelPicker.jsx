import { useMemo, useState } from "react";

import { Icon } from "../Icon/Icon.jsx";
import { Input } from "../Field/Field.jsx";

import styles from "./ModelPicker.module.css";

// Who made a model, from what its id and name say (the owner, 2026-11-02:
// "a search, and categories - gather them by the keywords that are there,
// OpenAI, Anthropic and so on"): an account's own id's prefix first
// ("openai/gpt-6", "anthropic/claude-…"), else the words of its name; a model
// on this computer is this computer's.
const MAKERS = {
	openai: "OpenAI", anthropic: "Anthropic", google: "Google", "meta-llama": "Meta", meta: "Meta", mistralai: "Mistral",
	mistral: "Mistral", qwen: "Qwen", deepseek: "DeepSeek", "x-ai": "xAI", xai: "xAI", amazon: "Amazon", cohere: "Cohere",
	microsoft: "Microsoft", nvidia: "NVIDIA", perplexity: "Perplexity", moonshotai: "Moonshot", "z-ai": "Zhipu", thudm: "Zhipu",
	ai21: "AI21", "aion-labs": "Aion Labs", inflection: "Inflection", nousresearch: "Nous Research", minimax: "MiniMax",
	baidu: "Baidu", tencent: "Tencent", bytedance: "ByteDance", ibm: "IBM", "ibm-granite": "IBM", liquid: "Liquid",
};
const BY_WORDS = [
	[/\b(gpt|o\d|openai|chatgpt)/, "OpenAI"], [/claude|anthropic/, "Anthropic"], [/gemini|gemma|google/, "Google"],
	[/llama/, "Meta"], [/qwen/, "Qwen"], [/mistral|mixtral|codestral|ministral/, "Mistral"], [/deepseek/, "DeepSeek"],
	[/grok/, "xAI"], [/nova|titan/, "Amazon"], [/command/, "Cohere"], [/phi-?\d/, "Microsoft"],
];

export function makerOf(model) {
	if (model.providerId == "local") return "On this computer";
	const id = model.modelId ?? "";
	const slash = id.indexOf("/");
	if (slash > 0) {
		const prefix = id.slice(0, slash).toLowerCase();
		return MAKERS[prefix] ?? prefix.split(/[-_]/).map(word => word.charAt(0).toUpperCase() + word.slice(1)).join(" ");
	}
	const words = `${model.name ?? ""} ${id}`.toLowerCase();
	return BY_WORDS.find(([pattern]) => pattern.test(words))?.[1] ?? "Other";
}

// A model's own name in its maker's list: an account's id without its maker.
function shownName(model) {
	const name = model.name ?? model.modelId ?? "";
	const slash = name.indexOf("/");
	return name == model.modelId && slash > 0 ? name.slice(slash + 1) : name;
}

const MOST_MAKERS = 12;

// Words as the search compares them: letters and digits only, so "gpt4"
// finds "gpt-4o" and "4.1" finds "gpt-4.1" (the owner, 2026-11-02: "make the
// search smart").
function plain(words) {
	return (words ?? "").toLowerCase().replace(/[^a-z0-9]+/g, "");
}

function isInOrder(term, text) {
	let at = 0;
	for (const letter of term) {
		at = text.indexOf(letter, at);
		if (at < 0) return false;
		at += 1;
	}
	return true;
}

// How well a model answers the words searched for, or 0 where one of them is
// not in it: in its own name best, at a word's start better still; then in
// its id or its maker; last, a word's letters in their order ("cld" for
// claude). Each word must be found.
export function scoreOf(item, terms) {
	const name = plain(item.name);
	const whole = plain(`${item.model.modelId} ${item.maker} ${item.name}`);
	const starts = (item.name ?? "").toLowerCase().split(/[^a-z0-9]+/).map(plain);
	let score = 0;
	for (const raw of terms) {
		const term = plain(raw);
		if (term == "") continue;
		if (starts.some(word => word.startsWith(term))) score += 4;
		else if (name.includes(term)) score += 3;
		else if (whole.includes(term)) score += 2;
		else if (term.length >= 3 && isInOrder(term, whole)) score += 1;
		else return 0;
	}
	return score;
}

// A model chosen by its name or its maker: words to search for, the makers to
// choose among, and the models under each maker's name, the chosen one ticked.
// `leading` stands over them all - the firm's default - and `trailing` under
// them - off, where there is such a choice.
export function ModelPicker({ models, value, onChange, keyOf, leading = [], trailing = [], label = "Model" }) {
	const [words, setWords] = useState("");
	const [maker, setMaker] = useState("all");
	const all = useMemo(() => models.map(model => ({ model, key: keyOf(model), maker: makerOf(model), name: shownName(model) })), [models, keyOf]);
	const makers = useMemo(() => {
		const counts = new Map();
		for (const item of all) counts.set(item.maker, (counts.get(item.maker) ?? 0) + 1);
		const sorted = [...counts.entries()].sort((one, other) => other[1] - one[1] || one[0].localeCompare(other[0]));
		const shown = sorted.slice(0, MOST_MAKERS).filter(([name]) => name != "Other");
		const restCount = sorted.slice(MOST_MAKERS).reduce((sum, [, count]) => sum + count, 0) + (counts.get("Other") ?? 0);
		return { shown, named: new Set(shown.map(([name]) => name)), restCount };
	}, [all]);
	const terms = words.trim().toLowerCase().split(/\s+/).filter(Boolean);
	const isSearching = terms.length > 0;
	const found = all
		.filter(item => (maker == "other" ? !makers.named.has(item.maker) : maker == "all" || item.maker == maker))
		.map(item => ({ ...item, score: isSearching ? scoreOf(item, terms) : 1 }))
		.filter(item => item.score > 0);
	// Searched for, the models stand in one list, the best answer first and a
	// shorter name before a longer one, each with its maker beside it.
	const ranked = isSearching ? [...found].sort((one, other) => other.score - one.score || one.name.length - other.name.length || one.name.localeCompare(other.name)) : [];
	const groups = [];
	for (const item of found) {
		const group = groups.find(entry => entry.maker == item.maker);
		if (group == null) groups.push({ maker: item.maker, items: [item] });
		else group.items.push(item);
	}
	groups.sort((one, other) => (one.maker == "On this computer" ? -1 : other.maker == "On this computer" ? 1 : one.maker.localeCompare(other.maker)));
	const isPlain = terms.length == 0 && maker == "all";

	return (
		<div className={styles.picker}>
			<label className={styles.search}>
				<Icon name="search" size={16} />
				<Input value={words} onChange={event => setWords(event.target.value)} placeholder="Search models" aria-label={`Search ${label.toLowerCase()}s`} autoFocus />
			</label>
			<div className={styles.makers} role="group" aria-label="Makers">
				<button type="button" aria-pressed={maker == "all"} onClick={() => setMaker("all")}>All<span>{all.length}</span></button>
				{makers.shown.map(([name, count]) => (
					<button key={name} type="button" aria-pressed={maker == name} onClick={() => setMaker(name)}>{name}<span>{count}</span></button>
				))}
				{makers.restCount > 0 ? <button type="button" aria-pressed={maker == "other"} onClick={() => setMaker("other")}>Other<span>{makers.restCount}</span></button> : null}
			</div>
			<div className={styles.list} role="listbox" aria-label={label}>
				{isPlain ? leading.map(option => <Choice key={option.key} label={option.label} isChosen={value == option.key} onChoose={() => onChange(option.key)} />) : null}
				{isSearching ? ranked.map(item => <Choice key={item.key} label={item.name} maker={item.maker} isChosen={value == item.key} onChoose={() => onChange(item.key)} />)
					: groups.map(group => (
						<section key={group.maker} className={styles.group} aria-label={group.maker}>
							<h3 className={styles.group_title}>{group.maker}</h3>
							{group.items.map(item => <Choice key={item.key} label={item.name} isChosen={value == item.key} onChoose={() => onChange(item.key)} />)}
						</section>
					))}
				{isPlain ? trailing.map(option => <Choice key={option.key} label={option.label} isChosen={value == option.key} onChoose={() => onChange(option.key)} />) : null}
				{found.length == 0 && !isPlain ? <p className={styles.none}>No model by that name.</p> : null}
			</div>
		</div>
	);
}

function Choice({ label, maker, isChosen, onChoose }) {
	return (
		<button type="button" role="option" aria-selected={isChosen} className={styles.choice} onClick={onChoose}>
			<span className={styles.choice_name}>{label}</span>
			<span className={styles.choice_end}>
				{maker != null ? <span className={styles.choice_maker}>{maker}</span> : null}
				{isChosen ? <Icon name="check" size={16} /> : null}
			</span>
		</button>
	);
}
