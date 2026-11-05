import { hasNative, realThreadsLatest, shownMatters, shownMembers, shownThreads } from "../../data/native.js";
import { conversationsFor } from "../../pages/Messages/conversations.js";
import { lastEvent } from "../../pages/threadState.js";

// Recent holds conversations only - a direct message or a thread of any
// Matter - never a page that was looked at, such as a Matter's (the owner,
// 2026-11-02: "only direct messages and messages in threads"). Each stands by
// whichever came last: the member opening it in this window, or something
// written in it. The latest three, so the Matters keep the room. Each by a
// face: a direct message's member, and in a thread whoever wrote in it last,
// the member here included.
export const RECENT_MOST = 3;

// Recent cleared, or one conversation taken out of it, without touching the
// conversations themselves (the owner, 2026-11-04: "a way to remove the
// Recent list without deleting the chats"): what stood in it then leaves it,
// and a conversation comes back only once something new happens in it. Kept on
// this computer, as the Matters' order is (Matters.jsx).
const CLEARED_KEY = "singularity.recent.cleared";
const CHANGED = "singularity:recent-changed";

// What this window cleared, when the computer cannot keep it.
let clearedHere = null;

function readable(kept) {
	if (kept == null || typeof kept != "object") return { clearedAt: "", removed: {} };
	return { clearedAt: typeof kept.clearedAt == "string" ? kept.clearedAt : "", removed: kept.removed != null && typeof kept.removed == "object" ? kept.removed : {} };
}

export function recentCleared() {
	try {
		const text = window.localStorage.getItem(CLEARED_KEY);
		return readable(text == null ? null : JSON.parse(text));
	} catch {
		// The computer keeps nothing for the window: what it cleared stands.
		return readable(clearedHere);
	}
}

function keepCleared(cleared) {
	clearedHere = cleared;
	try {
		window.localStorage.setItem(CLEARED_KEY, JSON.stringify(cleared));
	} catch {
		// Not kept: Recent still clears in this window.
	}
	window.dispatchEvent(new Event(CHANGED));
}

// Clears Recent as it stands now; returns how to put it back.
export function clearRecent(now = new Date().toISOString()) {
	const before = recentCleared();
	keepCleared({ clearedAt: now, removed: {} });
	return () => keepCleared(before);
}

// Takes one conversation out of Recent; returns how to put it back.
export function removeFromRecent(key, now = new Date().toISOString()) {
	const before = recentCleared();
	keepCleared({ ...before, removed: { ...before.removed, [key]: now } });
	return () => keepCleared(before);
}

export function onRecentCleared(listener) {
	window.addEventListener(CHANGED, listener);
	return () => window.removeEventListener(CHANGED, listener);
}

// Whether an entry still stands in Recent: something happened in it since
// Recent was cleared, or since it was taken out.
export function standsAfterClearing(entry, cleared) {
	if (cleared.clearedAt != "" && entry.standsAt <= cleared.clearedAt) return false;
	const removedAt = cleared.removed[entry.key];
	return removedAt == null || entry.standsAt > removedAt;
}

// `opened`: when the member opened each conversation in this window, by its
// address ("#messages/<member>", "#thread/<thread>").
export function recentConversations(state, opened = {}, cleared = recentCleared()) {
	const found = new Map();
	for (const conversation of conversationsFor(state)) {
		if (conversation.kind != "member") continue;
		const entry = directMessage(conversation.memberId, conversation.latestAt ?? conversation.items.at(-1)?.at ?? "");
		if (entry != null && (entry.at != "" || opened[entry.key] != null)) found.set(entry.key, entry);
	}
	for (const thread of threadsSaid(state)) found.set(thread.key, thread);
	for (const [key, at] of Object.entries(opened)) {
		if (found.has(key)) continue;
		// A conversation opened before the Matters' summaries are read, or one
		// they do not know yet, stands by what the window holds of it.
		const entry = key.startsWith("#messages/") ? directMessage(key.slice("#messages/".length), "")
			: key.startsWith("#thread/") ? threadHeld(key.slice("#thread/".length), state) : null;
		if (entry != null) found.set(key, { ...entry, at: entry.at || at });
	}
	return [...found.values()]
		.map(entry => ({ ...entry, standsAt: [entry.at ?? "", opened[entry.key] ?? ""].sort().at(-1) }))
		.filter(entry => entry.standsAt != "" && standsAfterClearing(entry, cleared))
		.sort((first, second) => second.standsAt.localeCompare(first.standsAt))
		.slice(0, RECENT_MOST);
}

// A thread as the window holds it: the open Matter's, with its posts.
function threadHeld(threadId, state) {
	const thread = shownThreads().find(item => item.id == threadId);
	if (thread == null || Array.isArray(thread.posts) == false) return null;
	const event = lastEvent(thread, state);
	const hasWords = event != null && event.kind != "begun";
	return {
		key: `#thread/${thread.id}`,
		at: hasWords ? event.at ?? "" : "",
		name: thread.title || "A thread",
		detail: shownMatters().find(matter => matter.id == thread.matterId)?.name ?? null,
		faceId: hasWords ? event.authorId ?? null : null,
		matterId: thread.matterId ?? null,
		place: { kind: "thread", threadId: thread.id, from: "inbox" },
	};
}

function directMessage(memberId, at) {
	const member = shownMembers().find(item => item.id == memberId);
	if (member == null) return null;
	return { key: `#messages/${member.id}`, at, name: member.name, faceId: member.id, matterId: null, place: { kind: "messages", memberId: member.id } };
}

// The threads of the firm's Matters with what each said last: from the
// Matters' summaries in Singularity, whichever Matter is open, and from the
// prototype's threads in a plain browser.
function threadsSaid(state) {
	const matterName = id => shownMatters().find(matter => matter.id == id)?.name ?? null;
	if (hasNative()) {
		return realThreadsLatest().map(thread => ({
			key: `#thread/${thread.threadId}`,
			at: thread.at,
			name: thread.title || "A thread",
			detail: matterName(thread.matterId),
			faceId: thread.authorId,
			matterId: thread.matterId,
			place: { kind: "thread", threadId: thread.threadId, from: "inbox" },
		}));
	}
	const found = [];
	for (const thread of shownThreads()) {
		if (Array.isArray(thread.posts) == false) continue;
		const event = lastEvent(thread, state);
		if (event == null || event.kind == "begun") continue;
		found.push({
			key: `#thread/${thread.id}`,
			at: event.at ?? "",
			name: thread.title,
			detail: matterName(thread.matterId),
			faceId: event.authorId ?? null,
			matterId: thread.matterId ?? null,
			place: { kind: "thread", threadId: thread.id, from: "inbox" },
		});
	}
	return found;
}
