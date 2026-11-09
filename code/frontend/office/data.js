import { useCallback, useEffect, useRef, useState } from "react";

import { isUnreadableNow } from "../data/unreadable.js";

// What the pane shows of Singularity: the Matter a file belongs to, its
// threads, its colleagues and their reviews - read from Singularity on this
// computer through the same Native API the window uses
// (code/backend/Office/OfficeConnection), never kept here as a second truth.
// It is read again whenever Singularity says the Matter or a colleague changed.

const native = () => window.nativeApp;

export function isConnected() {
	return window.__lmOffice?.isPaired?.() == true;
}

// Which Matter a file Office has open belongs to.
export function describeFile(location) {
	return native().describeOfficeFile(location ?? "");
}

function textOf(turn) {
	if (turn.isMemberMessage == true) return turn.prompt ?? "";
	if (Array.isArray(turn.responseSegments) && turn.responseSegments.length > 0) {
		return turn.responseSegments.map(segment => segment.text).join("");
	}
	return turn.prompt ?? "";
}

// A thread's messages as the pane shows them: who, when, what, which files,
// and the place in a document each is about - its own, or, for an answer, the
// place of the message it answers.
export function messagesOf(conversation, agents) {
	const messages = [];
	const byId = new Map();
	for (const turn of conversation?.turns ?? []) {
		if ((turn.threadEvent ?? "") != "") continue;
		if ((turn.deletedAt ?? "") != "") continue;
		// A colleague's run through a model answers a message already here: only
		// its answer is the colleague's message, and only once it has one.
		const isColleagueRun = turn.isMemberMessage != true && turn.isAgentMessage != true && (turn.agentId ?? "") != "";
		if (isColleagueRun && (turn.status != "completed" || (turn.responseSegments ?? []).length == 0)) continue;
		const agent = turn.isAgentMessage == true || isColleagueRun ? agents.find(entry => entry.id == turn.agentId) ?? null : null;
		const isPerson = turn.isMemberMessage == true || (turn.isAgentMessage != true && !isColleagueRun);
		const answers = (turn.quoteMessageId || turn.answersMessageId) ?? "";
		const message = {
			id: turn.taskId,
			at: turn.createdAt,
			authorName: agent?.name ?? (isColleagueRun ? "A colleague" : turn.authorName ?? ""),
			agent: agent ?? null,
			isViewer: isPerson && ((turn.authorActorId ?? "") == "" || turn.authorActorId == conversation.viewer?.memberId),
			text: textOf(turn),
			reviewId: turn.documentReviewId ?? null,
			documentIds: turn.attachmentDocumentIds ?? [],
			place: turn.place ?? byId.get(answers)?.place ?? null,
		};
		messages.push(message);
		byId.set(message.id, message);
	}
	return messages;
}

// A place in this document someone asked to see from Singularity's window:
// the one waiting when the pane opened, then each as it is asked. Each is gone
// to once, also when the pane is opened again.
export function useReveal(documentId, waiting, reveal) {
	const act = useRef(reveal);
	act.current = reveal;
	useEffect(() => {
		const done = key => {
			try {
				const seen = JSON.parse(sessionStorage.getItem("singularity.revealed") ?? "[]");
				if (seen.includes(key)) return true;
				sessionStorage.setItem("singularity.revealed", JSON.stringify([...seen.slice(-20), key]));
			} catch {
				// Without storage a pane opened again may go to the place again.
			}
			return false;
		};
		const apply = request => {
			if (request == null || request.documentId != documentId || done(request.revealId)) return;
			act.current(request);
		};
		apply(waiting);
		const off = native()?.on?.("office.reveal", apply);
		return () => {
			if (typeof off == "function") off();
		};
	}, [documentId, waiting?.revealId]);
}

// The documents one piece of work is about: the agreement a colleague reviews
// and each version it handed in, so a place in one is the same clause in all.
export function familyOf(documentId, work) {
	return new Set([documentId, work.review?.sourceDocumentId, ...(work.review?.versions ?? []).map(version => version.resultDocumentId)]
		.filter(Boolean));
}

// Whether a colleague takes work now, as the app's team counts them
// (data/native.js): a stopped colleague never runs again, and a draft or one
// waiting for approval has not begun, so a word to one would wait for no one.
export const takesWork = agent => !["stopped", "waiting_for_approval", "draft", "archived"].includes(agent?.status);

// The Matter, read whole: its documents, threads, colleagues and reviews.
async function readMatter(matterId) {
	const api = native();
	const [summaries, documents, chats, agents, reviews, organisation] = await Promise.all([
		api.listMatterSummaries(),
		api.listMatterDocuments(matterId),
		api.listProjectChats(matterId),
		api.listAgents(matterId),
		api.listDocumentReviews(matterId),
		api.getOrganisation().catch(() => null),
	]);
	const matter = (summaries?.matters ?? []).find(entry => entry.id == matterId) ?? null;
	const colleagues = (agents?.agents ?? []).filter(agent => agent.status != "archived");
	const agentChats = new Set(colleagues.map(agent => agent.chatId).filter(Boolean));
	const threads = (chats?.chats ?? [])
		.filter(chat => (chat.deletedAt ?? "") == "" && chat.archived != true && agentChats.has(chat.id) == false && (chat.agentId ?? "") == "")
		.sort((first, second) => (second.updatedAt ?? "").localeCompare(first.updatedAt ?? ""));
	return {
		matter,
		documents: (documents?.documents ?? []).filter(document => (document.deletedAt ?? "") == ""),
		threads,
		agents: colleagues,
		// Those a pane may give work to; every colleague still names its own messages.
		takers: colleagues.filter(takesWork),
		reviews: reviews?.reviews ?? [],
		viewer: organisation?.viewer ?? null,
		members: organisation?.members ?? [],
	};
}

// The Matter as a hook: read, and read again when Singularity says it changed.
export function useMatter(matterId) {
	const [data, setData] = useState(null);
	const [problem, setProblem] = useState(null);
	const timer = useRef(null);

	const reload = useCallback(async () => {
		if (matterId == null) return;
		try {
			setData(await readMatter(matterId));
			setProblem(null);
		} catch (error) {
			setProblem(error?.userMessage || error?.message || "Singularity did not answer.");
		}
	}, [matterId]);

	useEffect(() => {
		reload();
		const later = () => {
			clearTimeout(timer.current);
			timer.current = setTimeout(reload, 250);
		};
		const offMatter = native()?.on?.("matter.changed", payload => {
			if (payload?.projectId == null || payload.projectId == matterId) later();
		});
		const offAgent = native()?.on?.("agent.changed", payload => {
			if (payload?.matterId == null || payload.matterId == matterId) later();
		});
		return () => {
			clearTimeout(timer.current);
			if (typeof offMatter == "function") offMatter();
			if (typeof offAgent == "function") offAgent();
		};
	}, [matterId, reload]);

	return { data, problem, reload };
}

// A thread's conversation as a hook, read again with its Matter.
export function useConversation(matterId, chatId, version) {
	const [conversation, setConversation] = useState(null);
	useEffect(() => {
		let isCurrent = true;
		if (matterId == null || chatId == null) {
			setConversation(null);
			return undefined;
		}
		native().listChatConversation(matterId, chatId).then(
			read => { if (isCurrent) setConversation(read); },
			// A Matter whose data can't be read just now keeps the conversation
			// shown; a thread gone is shown gone.
			error => { if (isCurrent && isUnreadableNow(error) == false) setConversation(null); },
		);
		return () => { isCurrent = false; };
	}, [matterId, chatId, version]);
	return conversation;
}

// Where the work on a document stands: as a source a colleague reviews, or as
// the draft one of its versions handed in.
export function workOn(documentId, reviews) {
	for (const review of reviews) {
		const version = (review.versions ?? []).find(entry => entry.resultDocumentId == documentId);
		if (version != null) return { kind: "draft", review, version };
	}
	const reviewed = reviews
		.filter(review => review.sourceDocumentId == documentId)
		.sort((first, second) => (second.updatedAt ?? "").localeCompare(first.updatedAt ?? ""))[0];
	if (reviewed != null) return { kind: "source", review: reviewed, version: reviewed.versions?.at(-1) ?? null };
	return { kind: "none", review: null, version: null };
}

export function isUnderway(version) {
	return ["queued", "preparing", "running", "producing", "returned"].includes(version?.state);
}

// The thread the document's work is discussed in: its review's, or else the
// Matter's latest.
// The thread a document's work is discussed in: its review's; else the thread
// that bears the document's name; else none yet - the first word about it
// begins one, as one topic is one thread.
export function threadFor(work, threads, documentTitle) {
	if (work.review?.chatId != null && threads.some(thread => thread.id == work.review.chatId)) return work.review.chatId;
	const named = threads.find(thread => (thread.title ?? "").trim().toLowerCase() == (documentTitle ?? "").trim().toLowerCase());
	return named?.id ?? null;
}

// A document's name as a thread's title: "Harborline Mutual NDA".
export function titleOf(document) {
	return (document?.displayName ?? document?.name ?? "").replace(/\.[^.]+$/, "").trim();
}

// The colleague whose desk a document is: the one whose brief names what the
// document is ("NDA", "supply"), else the first.
export function deskFor(document, agents) {
	const words = (document?.displayName ?? "").toLowerCase().split(/[^a-z0-9]+/).filter(word => word.length >= 3);
	const scored = agents.map(agent => {
		const brief = `${agent.goal ?? ""} ${agent.name ?? ""}`.toLowerCase();
		return { agent, score: words.filter(word => brief.includes(word)).length };
	});
	scored.sort((first, second) => second.score - first.score);
	return scored[0]?.agent ?? null;
}

// The rows and columns an Excel address covers, from 0: "G2", "A2:M2", "G:G",
// "2:4"; null for what is no address.
export function boundsOf(address) {
	const [from, to] = String(address ?? "").replaceAll("$", "").split(":");
	const corner = text => {
		const found = /^([A-Za-z]{0,3})(\d{0,7})$/.exec(text ?? "");
		if (found == null || (found[1] == "" && found[2] == "")) return null;
		const letters = found[1].toUpperCase();
		return {
			column: letters == "" ? null : [...letters].reduce((total, letter) => total * 26 + letter.charCodeAt(0) - 64, 0) - 1,
			row: found[2] == "" ? null : Number(found[2]) - 1,
		};
	};
	const start = corner(from);
	const end = to == null ? start : corner(to);
	if (start == null || end == null) return null;
	return {
		top: start.row ?? 0,
		bottom: end.row ?? Number.POSITIVE_INFINITY,
		left: start.column ?? 0,
		right: end.column ?? Number.POSITIVE_INFINITY,
	};
}

// Whether two addresses on one sheet share a cell.
export function rangesMeet(first, second) {
	const a = boundsOf(first);
	const b = boundsOf(second);
	if (a == null || b == null) return false;
	return a.top <= b.bottom && b.top <= a.bottom && a.left <= b.right && b.left <= a.right;
}

// Gives work or says something in the Matter's thread, as the window does: a
// colleague the words name takes it up. A thread is begun, with the given
// title, when there is none.
export async function postToMatter(matterId, chatId, text, mentionedAgentIds, documentIds, title, place) {
	const api = native();
	let thread = chatId;
	if (thread == null) {
		const made = await api.createProjectChat(matterId);
		try {
			await api.renameProjectChat(matterId, made.id, made.revision, title || "New thread");
		} catch {
			// It keeps the backend's name until its first message names it.
		}
		thread = made.id;
	}
	await api.postChatMessage(matterId, thread, text, mentionedAgentIds, {
		attachmentDocumentIds: documentIds,
		place: place ?? null,
	});
	return thread;
}

// The marks the agreement already carried before the colleague's draft - the
// counterparty's comments and tracked changes - read from the source document
// as Singularity read it, so a return carries only the lawyer's own.
export async function marksOfSource(matterId, sourceDocumentId) {
	const marks = { comments: new Set(), authors: new Set() };
	if (matterId == null || sourceDocumentId == null) return marks;
	try {
		const view = await native().readDocumentView(matterId, sourceDocumentId);
		const textOf = comment => (comment.paragraphs ?? []).map(paragraph => (paragraph.runs ?? []).map(run => run.text ?? "").join("")).join(" ");
		for (const comment of view.comments ?? []) {
			marks.comments.add(markKey(comment.author, textOf(comment)));
			if (comment.author) marks.authors.add(comment.author.trim().toLowerCase());
		}
		for (const block of view.blocks ?? []) {
			const paragraph = block.paragraph;
			if (paragraph == null) continue;
			for (const revision of [paragraph.revision, ...(paragraph.runs ?? []).map(run => run.revision)]) {
				if (revision?.author) marks.authors.add(revision.author.trim().toLowerCase());
			}
		}
	} catch {
		// Without the source's marks nothing is told apart: the lawyer sees each
		// mark's author before the draft goes back.
	}
	return marks;
}

export function markKey(author, text) {
	return `${(author ?? "").trim().toLowerCase()}|${(text ?? "").replace(/\s+/g, " ").trim().toLowerCase().slice(0, 60)}`;
}

export function decide(matterId, reviewId, version, decision, comment) {
	return native().decideDocumentReview(matterId, reviewId, version, decision, comment ?? "");
}

export function openInOffice(matterId, documentId) {
	return native().openOfficeDocument(matterId, documentId);
}

export function fileIntoMatter(matterId, name, base64) {
	return native().fileIntoMatter(matterId, name, base64);
}

// ---- A Matter's correspondence ------------------------------------------------

// Where a letter belongs among the Matters open on this computer, with the
// evidence for each; nothing is filed by asking.
export function suggestMatter(message) {
	return native().suggestLetterMatter(message);
}

// Files the letter to the Matter, with the evidence the person saw. Filed
// `byRule`, a rule of the Matter's intake files it, which the Matter
// checks.
export function fileLetter(matterId, message, evidence, byRule = false) {
	return native().fileLetter(matterId, message, evidence ?? [], "outlook", byRule ? "rule" : "person");
}

export function moveLetter(matterId, letterId, toMatterId) {
	return native().moveLetter(matterId, letterId, toMatterId);
}

export async function lettersOf(matterId) {
	const answer = await native().listLetters(matterId);
	return answer?.letters ?? [];
}

export function saveAttachment(matterId, letterId, providerId, base64) {
	return native().saveLetterAttachment(matterId, letterId, providerId, base64);
}

export async function matterSummaries() {
	const answer = await native().listMatterSummaries();
	return answer?.matters ?? answer ?? [];
}

// The email drafts colleagues prepared in the Matters open here, the newest
// first, each with whether it is ready to send.
export async function colleagueEmailDrafts() {
	const drafts = [];
	for (const matter of await matterSummaries()) {
		const matterId = matter.matterId ?? matter.id;
		const listed = await native().listAssignments(matterId).catch(() => null);
		for (const assignment of listed?.assignments ?? []) {
			const email = assignment.email ?? {};
			if (email.state != "drafted" || !email.documentId) continue;
			drafts.push({
				matterId,
				matterName: matter.name ?? matter.matterName ?? "",
				agentName: assignment.agentName ?? "",
				subject: email.subject ?? "",
				documentId: email.documentId,
				isReady: email.isReady == true,
				readiness: email.readiness ?? "",
				at: assignment.finishedAt || assignment.startedAt || "",
			});
		}
	}
	return drafts.sort((first, second) => second.at.localeCompare(first.at)).slice(0, 6);
}

export function readEmailDraft(matterId, documentId) {
	return native().readEmailDraft(matterId, documentId);
}

// The Matter's document a file in its folder became, once Singularity has read
// it; null when it has not within the wait. A record the folder no longer
// answers to is never the one.
export async function documentAt(matterId, relativePath, seconds = 20) {
	const wanted = (relativePath ?? "").replaceAll("\\", "/").toLowerCase();
	const end = Date.now() + seconds * 1000;
	while (Date.now() < end) {
		const listed = await native().listMatterDocuments(matterId).catch(() => null);
		const found = (listed?.documents ?? []).find(document => (document.deletedAt ?? "") == ""
			&& document.processingState != "unavailable"
			&& (document.relativePath ?? "").replaceAll("\\", "/").toLowerCase() == wanted);
		if (found != null) return found;
		await new Promise(resolve => setTimeout(resolve, 700));
	}
	return null;
}
