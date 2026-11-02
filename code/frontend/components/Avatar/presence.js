// What an AI colleague's character acts out (D-013). The backend derives it in
// one place (Agent/AgentActions agent_presence) and carries it on every AI
// colleague as `presence`, in `agent.*` and `organisation.*`: greet, think,
// work, wait, explain, listen or rest. A species without a pass for one shows
// the nearest honest one: working, it thinks; greeting, it explains; and
// otherwise it rests. `presences` maps each presence a species has to how
// many milliseconds its pass plays.
const nearer = { work: "think", greet: "explain" };

export function shownPresence(presence, presences) {
	const has = name => (Array.isArray(presences) ? presences.includes(name) : presences?.[name] != null);
	for (let wanted = presence; wanted != null; wanted = nearer[wanted]) {
		if (has(wanted)) return wanted;
	}
	return "rest";
}

// What the small dot at a face's corner says (D-067): the colleague's ground
// is who it is, the dot what it is doing now. Its colours are tokens in
// styles/base.css; a colleague at rest wears none.
const states = {
	think: { colour: "var(--state-working)", words: "Working" },
	work: { colour: "var(--state-working)", words: "Working" },
	wait: { colour: "var(--state-waiting)", words: "Waiting its turn" },
	explain: { colour: "var(--state-ready)", words: "Result ready" },
	greet: { colour: "var(--state-decision)", words: "Needs your decision" },
	listen: { colour: "var(--state-decision)", words: "Needs your decision" },
};

export function stateOf(presence) {
	return states[presence] ?? null;
}
