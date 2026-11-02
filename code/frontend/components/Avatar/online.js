import { useSyncExternalStore } from "react";

import { viewerId } from "../../data/workspace.js";

// Who of the firm is there now (the owner, 2026-11-02: "if a colleague - a
// person or an AI - is online, available, always show a green dot beside
// them, everywhere in the system"): a person while a computer of theirs is
// connected to this one (data/coordination.js says which, as the firm's
// exchange reports it); an AI colleague while the computer that runs it is
// here or connected; and the member at this computer, who is here (the owner,
// 2026-11-02: "why am I not shown as online?").
let state = { actors: new Set(), version: 0 };
const listeners = new Set();

function subscribe(listener) {
	listeners.add(listener);
	return () => listeners.delete(listener);
}

// The seats connected now, as the firm's exchange last reported them.
export function noteOnline(actorIds) {
	const next = new Set((actorIds ?? []).filter(Boolean));
	if (next.size == state.actors.size && [...next].every(actor => state.actors.has(actor))) return;
	state = { actors: next, version: state.version + 1 };
	for (const listener of listeners) listener();
}

// Read again by a face whenever who is there changes.
export function useOnline() {
	useSyncExternalStore(subscribe, () => state.version);
}

export function isOnline(member) {
	if (member == null) return false;
	if (member.id == viewerId) return true;
	if (member.kind == "ai_colleague") {
		if (member.isArchived == true) return false;
		// One this computer runs is here while Singularity is open.
		if (member.runsHere != false) return true;
		return (member.hostActorId ?? "") != "" && state.actors.has(member.hostActorId);
	}
	return (member.actorId ?? "") != "" && state.actors.has(member.actorId);
}
