import { isRealThreadUnread, realColleagueState, realThreadStatus, shownInbox } from "../data/native.js";
import { nowLocal } from "../data/clock.js";
import { colleagueStates, decisions, inbox, members, reactions, threads, viewerId } from "../data/workspace.js";
import { plainWords } from "./words.js";

// The workspace's state on top of the mock data: what the viewer decided,
// replied, marked done, read and saved. It is kept in App and never written
// anywhere (PROD-2).
export const initialState = {
	doneIds: [],
	readIds: [],
	savedIds: ["t_kestrel"],
	choices: {},
	extraEvents: {},
	extraMessages: {},
	readChatIds: [],
	// How many AI colleagues the member hired in this session (D-071): a hire
	// joins the mock data, and the count tells the pages to read it again.
	hires: 0,
	// Direct messages the member filed from the Inbox's Unsorted: message id
	// to the project's id.
	filed: {},
	// What the members did with the messages of threads: reactions by post
	// (emoji to who), the viewer's edits (post to its paragraphs), the posts the
	// viewer deleted, and the threads the viewer marked unread again.
	reactions: Object.fromEntries(Object.entries(reactions).map(([postId, byEmoji]) => [postId, Object.fromEntries(Object.entries(byEmoji).map(([emoji, who]) => [emoji, [...who]]))])),
	editedPosts: {},
	deletedPostIds: [],
	unreadIds: [],
	// The answers the viewer gave to the questions members asked with their
	// messages: post or message id to the answer.
	answers: {},
	// Who writes an answer now, in a mock thread (by its id) or a personal chat
	// (by the member's id): what the app's emulation in onboarding shows
	// (D-134). A real thread's and chat's come from the backend.
	typing: {},
};

// Whether a thread is unread for the viewer: as the inbox has it until read,
// or because the viewer marked it unread again.
export function isThreadUnread(threadId, state) {
	if (state.unreadIds.includes(threadId)) return true;
	// A real thread is unread while another's message in it is newer than what
	// the member has seen (D-202).
	const thread = threads.find(item => item.id == threadId);
	if (thread?.isReal == true) return state.readIds.includes(threadId) == false && isRealThreadUnread(thread);
	const entry = inbox.find(item => item.threadId == threadId);
	return entry?.isUnread == true && state.readIds.includes(threadId) == false;
}

// The project a chat message is filed to: as its writer filed it, or as the
// member filed it from the Inbox's Unsorted.
export function filingOf(message, state) {
	return state.filed[message.id] ?? message.matterId ?? null;
}

export function findThread(threadId) {
	return threads.find(thread => thread.id == threadId);
}

// Everything that happened in a thread, in order: its posts, its closing and
// reopening, and what the viewer added. A real thread's closing and reopening
// come among its posts, each with its kind (data/native.js). A deleted message
// keeps its place without its words, in a real thread as in the demo (D-092).
export function threadEvents(thread, state) {
	const events = thread.posts.map(post => ({ kind: "post", ...post }));

	if (thread.closed != null) {
		events.push({ id: `${thread.id}_closed`, kind: "closed", ...thread.closed });
	}

	return [...events, ...(state.extraEvents[thread.id] ?? [])]
		.map(event => state.deletedPostIds.includes(event.id) ? { ...event, isDeleted: true, paragraphs: [], files: null, ask: null } : event)
		.map(event => state.editedPosts[event.id] != null && event.isDeleted != true ? { ...event, paragraphs: state.editedPosts[event.id], isEdited: true } : event);
}

// The words a list shows for an event: a closing's conclusion, a message's
// last paragraph, or the files it hands over when it has no words.
export function eventWords(event) {
	if (event.kind == "closed") return event.conclusion;
	if (event.kind == "begun") return "No messages yet";
	const words = event.paragraphs?.[event.paragraphs.length - 1];
	if (words != null && words != "") return plainWords(words);
	return (event.realFiles ?? event.files ?? []).map(file => file.name).join(", ");
}

// Whether an event is one a list names a thread by: a message still there, or
// a closing.
export function isListedEvent(event) {
	return (event.kind == "post" && event.isDeleted != true) || event.kind == "closed";
}

export function isThreadClosed(thread, state) {
	const turns = threadEvents(thread, state).filter(event => event.kind == "closed" || event.kind == "reopened");
	if (turns.length == 0) return false;
	return turns[turns.length - 1].kind == "closed";
}

export function hasPendingDecision(thread, state) {
	// A real thread waits for the lawyer when its latest version does.
	if (thread.isReal == true) return realThreadStatus(thread).kind == "yours";
	if (thread.decisionId == null) return false;
	return state.choices[thread.decisionId] == null;
}

function hasViewerAnswered(thread, state) {
	return (state.extraEvents[thread.id] ?? []).some(event => event.kind == "post" && event.authorId == viewerId);
}

// What a row of the inbox waits for: a decision the viewer holds, a question
// addressed to the viewer, or nothing.
export function waitingKind(entry, state) {
	const thread = findThread(entry.threadId);
	if (hasPendingDecision(thread, state)) return "decision";
	if (entry.waits == "question" && hasViewerAnswered(thread, state) == false) return "question";
	return null;
}

export function lastEvent(thread, state) {
	const events = threadEvents(thread, state).filter(isListedEvent);
	return events[events.length - 1] ?? beginningOf(thread);
}

// A thread with nothing in it yet - a real one just begun (data/native.js) -
// is listed by its beginning: who began it and when, with nothing said.
export function beginningOf(thread) {
	return { id: `${thread.id}_begun`, kind: "begun", authorId: thread.beganBy ?? viewerId, at: thread.createdAt ?? nowLocal(), paragraphs: [] };
}

export function activeInbox(state) {
	return shownInbox().filter(entry => state.doneIds.includes(entry.threadId) == false);
}

export function doneInbox(state) {
	return shownInbox().filter(entry => state.doneIds.includes(entry.threadId) == true);
}

export function waitingCounts(state) {
	const counts = { decision: 0, question: 0 };

	for (const entry of activeInbox(state)) {
		const kind = waitingKind(entry, state);
		if (kind != null) counts[kind] += 1;
	}

	return counts;
}

// What an AI colleague is doing: { text, kind, presence }, where kind is
// "yours" (it waits for the viewer), "working" (work for the viewer under way)
// or "idle", and presence is what its character acts out (D-013).
export function colleagueState(memberId, state) {
	// A colleague of the real Matter is doing what the backend says it is.
	const real = realColleagueState(memberId);
	if (real !== undefined) return real;

	for (const [decisionId, decision] of Object.entries(decisions)) {
		if (decision.colleagueId != memberId) continue;

		const choice = state.choices[decisionId];
		if (choice == null) continue;

		const option = decision.options.find(item => item.key == choice);
		return option.state;
	}

	return colleagueStates[memberId] ?? null;
}

// The waiting items of the viewer's inbox that belong to one Matter.
export function waitingInMatter(matterId, state) {
	return activeInbox(state)
		.filter(entry => waitingKind(entry, state) != null)
		.filter(entry => findThread(entry.threadId).matterId == matterId)
		.length;
}

// Unread threads of one Matter that are still in the viewer's inbox.
export function unreadInMatter(matterId, state) {
	return activeInbox(state)
		.filter(entry => entry.isUnread == true && state.readIds.includes(entry.threadId) == false)
		.filter(entry => findThread(entry.threadId).matterId == matterId)
		.length;
}

export function isChatUnread(chat, state) {
	return chat.isUnread == true && state.readChatIds.includes(chat.memberId) == false;
}

export function lastChatMoment(chat, state) {
	const messages = [...chat.messages, ...(state.extraMessages[chat.memberId] ?? [])];
	return messages[messages.length - 1].at;
}

export function now() {
	return nowLocal();
}

// A member's time off, as their data says it. The member at this computer
// sets none here: the account menu no longer offers it (D-201).
export function memberAway(member) {
	if (member.id == viewerId) return null;
	return member.away ?? null;
}

// The one status a thread shows (PRODUCT.md, "One status per row"): what it
// waits for, and from whom, in the order a member reads them. `words` says
// it; `kind` picks its mark (components/Status).
export function threadStatus(thread, state) {
	// A real thread says what its latest review waits for (data/native.js).
	if (thread.isReal == true) return realThreadStatus(thread);
	const entry = inbox.find(item => item.threadId == thread.id);
	const decision = thread.decisionId != null ? decisions[thread.decisionId] : null;
	const choice = decision != null ? state.choices[thread.decisionId] : null;

	if (isThreadClosed(thread, state)) return { kind: "settled", words: "Closed with a conclusion" };
	if (decision != null && choice == null) {
		if (decision.kind == "ask") return { kind: "yours", words: "Asks you" };
		return { kind: "yours", words: "Waits for your acceptance" };
	}
	if (decision != null && choice == "B") return { kind: "underway", words: `Returned: ${findMemberName(decision.colleagueId)} is working on it` };
	if (entry?.waits == "question" && hasViewerAnswered(thread, state) == false) {
		return { kind: "yours", words: entry.due != null && entry.due == nowLocal().slice(0, 10) ? "Waits for your answer today" : "Waits for your answer" };
	}
	if (entry?.waits == "deadline") return { kind: "due", words: `Due ${dueWords(entry.due)}` };
	const failure = thread.posts.find(post => post.failure != null);
	if (failure != null) return { kind: "failed", words: "Stopped: waits for a readable copy" };
	return { kind: "waiting", words: "Open" };
}

function findMemberName(memberId) {
	return members.find(member => member.id == memberId)?.name ?? "the colleague";
}

function dueWords(day) {
	const date = new Date(`${day}T12:00:00`);
	return new Intl.DateTimeFormat("en-US", { weekday: "short", month: "short", day: "numeric" }).format(date);
}
