import { useSyncExternalStore } from "react";

import { namesAll } from "../components/Composer/mentions.js";
import { plainOf } from "../components/MessageText/markdown.js";
import { pigmentName, pigments } from "../components/ProjectMark/pigments.js";

import { isEmulation } from "../emulation/emulation.js";

import { aiUsage, aiWork, expenses, timeEntries } from "./billing.js";
import { calendarEvents } from "./calendar.js";
import { ensureDefaultModels } from "./models.js";
import { projectMail } from "./mail.js";
import { becomeOrganisation, becomeViewer, becomeViewerRights, chats, general, inbox, matters, members, organisation, threads, viewerId } from "./workspace.js";

// The window's native core (the Native API), when Singularity runs this
// page, and nothing in a plain browser, where the prototype's mock data is all
// there is (the owner's overnight mandate, 2026-10-24: "replace the critical
// mock path with the real backend").
//
// In the app every place is the firm's own (2026-10-29: a new firm was shown
// Marlowe Finch's Amsterdam): its name, its members, its Matters and, of the
// Matter the member has open, its threads, what its AI colleagues did in them,
// its documents and the document reviews. All are read from the backend -
// never kept here as a second truth (ARCH-2) - and read again whenever the
// backend says they changed (matter.changed, agent.changed).

// The app's emulation inside onboarding runs on the mock data alone (D-134).
export function hasNative() {
	return isEmulation == false && typeof window != "undefined" && window.nativeApp != null
		&& typeof window.nativeApp.listDocumentReviews == "function";
}

export function nativeApp() {
	return window.nativeApp;
}

// A Matter of the firm as the window names it: "rm_" and its id.
const REAL_MATTER_PREFIX = "rm_";
// The Matter the member last opened on this computer, which the window opens on.
const OPEN_MATTER_KEY = "singularity.openMatter";

function uiMatterIdOf(realMatterId) {
	return `${REAL_MATTER_PREFIX}${realMatterId}`;
}

let snapshot = { status: hasNative() ? "connecting" : "off", version: 0 };
const listeners = new Set();
let reloadTimer = null;
let isSubscribed = false;
// Messages deleted here whose Undo still stands: message id to its timer. The
// backend deletes one only once its Undo has gone (components/Toast).
const pendingDeletions = new Map();
export const UNDO_MILLISECONDS = 5200;

function publish(next) {
	snapshot = { ...snapshot, ...next, version: snapshot.version + 1 };
	for (const listener of listeners) listener();
}

function subscribe(listener) {
	listeners.add(listener);
	return () => listeners.delete(listener);
}

function getSnapshot() {
	return snapshot;
}

// The real work as the pages read it; they render again when it changes.
export function useRealWork() {
	return useSyncExternalStore(subscribe, getSnapshot);
}

export function realWork() {
	return snapshot;
}

// ---- Who is who ----------------------------------------------------------------

// The Agent Family's species an AI colleague's face is drawn as (D-063).
const SPECIES = ["kitsune", "unicorn", "phoenix", "griffin", "pegasus", "dragon"];

// The species of a firm's colleague: the one it was hired as, else the one its
// name is, else one of its own by its name, so two colleagues rarely look alike.
function speciesOf(name, character) {
	if (SPECIES.includes(character)) return character;
	const words = (name ?? "").trim().toLowerCase();
	if (SPECIES.includes(words)) return words;
	let hash = 0;
	for (const letter of words) hash = (hash * 31 + letter.charCodeAt(0)) >>> 0;
	return SPECIES[hash % SPECIES.length];
}

// A member for someone the backend names: the person at this computer is the
// viewer; anyone else is the firm's member of that name. In the app the firm's
// members are its own (2026-10-29): a name is never taken for the prototype's
// member of the same name, as a plain browser, where the prototype is all
// there is, still does.
function memberIdFor(name, isColleague, isViewer) {
	if (isViewer == true) return viewerId;
	const words = (name ?? "").trim() || (isColleague == true ? "AI colleague" : "");
	if (hasNative() == false) {
		const known = members.find(member => member.name.toLowerCase() == words.toLowerCase()
			&& (member.kind == "ai_colleague") == isColleague);
		if (known != null) return known.id;
		if (words == "" || words == "AI colleague") return isColleague == true ? "m_juno" : viewerId;
	}
	if (words == "") return viewerId;
	const named = idsByName.get(nameKey(words, isColleague));
	if (named != null && members.some(member => member.id == named)) return named;
	const id = `real_${isColleague == true ? "ai" : "person"}_${slugOf(words)}`;
	placeMember(id, words, isColleague);
	return id;
}

function slugOf(words) {
	return words.toLowerCase().replace(/[^a-z0-9]+/g, "_");
}

function initialsOf(words) {
	return words.split(/\s+/).map(word => word[0]).join("").slice(0, 2).toUpperCase();
}

// A member's row, made once.
function placeMember(id, words, isColleague) {
	if (members.some(member => member.id == id) == true) return;
	members.push({
		id,
		kind: isColleague == true ? "ai_colleague" : "person",
		name: words,
		initials: initialsOf(words),
		color: "#5b6170",
		role: isColleague == true ? "AI colleague" : "Member",
		practice: null,
		isReal: true,
		...(isColleague == true ? { species: speciesOf(words), record: "No results yet. Each piece of work it delivers counts here once a lawyer accepts it or sends it back.", area: "" } : {}),
	});
}

// Who is who, by what does not change (the owner, 2026-11-02: "Oleg P and
// Magomed both appeared, though I only renamed Oleg"; an archived Pasha took
// the place of the one at work): a person by their seat, an AI colleague by
// its own id. A name finds only the row it was last given to, and a renamed
// member keeps their row, which their old name still finds.
const idsByName = new Map();
const personIdsByActor = new Map();
const colleagueIdsByAgent = new Map();

function nameKey(words, isColleague) {
	return `${isColleague == true ? "ai" : "person"}:${(words ?? "").trim().toLowerCase()}`;
}

// A row given the name its record states now; its earlier name still finds it.
function nameMember(member, name, isColleague) {
	const words = (name ?? "").trim();
	if (words == "") return;
	if ((member.name ?? "") != "") idsByName.set(nameKey(member.name, isColleague), member.id);
	idsByName.set(nameKey(words, isColleague), member.id);
	if (member.name == words) return;
	member.name = words;
	member.initials = initialsOf(words);
}

// A person by their seat, where it is known; by their name otherwise.
function personIdForActor(actorId, name) {
	const actor = actorId ?? "";
	const known = actor != "" ? personIdsByActor.get(actor) : null;
	if (known != null && members.some(member => member.id == known)) return known;
	const id = memberIdFor(name, false, false);
	if (actor != "" && id != viewerId) personIdsByActor.set(actor, id);
	return id;
}

// A member's name as the window shows it now, by their seat or an AI
// colleague's id; the name a record kept where neither is known.
export function nameBySeat(actorId, keptName) {
	const actor = (actorId ?? "").trim();
	if (actor != "") {
		const personRow = personIdsByActor.get(actor);
		const row = personRow ?? colleagueIdsByAgent.get(actor);
		const member = row != null ? members.find(item => item.id == row) : null;
		if (member != null && (member.name ?? "") != "") return member.name;
	}
	return (keptName ?? "").trim();
}

// The seat of a person of the firm the window shows, by their row; none
// for a row no seat was read for.
export function actorOfPerson(memberId) {
	for (const [actor, id] of personIdsByActor) if (id == memberId) return actor;
	return null;
}

// A review is a colleague's by its id; by its name only where the review
// names no colleague's id.
function isReviewOf(review, agent) {
	return (review.agentId ?? "") != "" ? review.agentId == agent.id : review.agentName == agent.name;
}

// An AI colleague by its own id, by its name only where none is known: a
// deleted colleague's words keep their own author, never another colleague's
// of the same name (the owner, 2026-11-02).
function colleagueIdFor(agentId, name) {
	const agent = (agentId ?? "").trim();
	const known = agent != "" ? colleagueIdsByAgent.get(agent) : null;
	if (known != null && members.some(member => member.id == known)) return known;
	if (agent != "") return colleagueMember({ id: agent, name: (name ?? "").trim() || "AI colleague", status: "stopped", isHere: false });
	return memberIdFor(name, true, false);
}

// A person of the firm's record: their row by their seat, under the name
// the record gives them now.
function firmPersonMember(person) {
	const id = personIdForActor(person.actorId, person.name);
	if (id == viewerId) return id;
	const member = members.find(item => item.id == id);
	if (member != null && member.isReal == true) {
		nameMember(member, person.name, false);
		member.isFirmPerson = true;
	}
	return id;
}

// A person of the firm by the name their record gives, as the window shows
// them: the member here when it is them.
export function personOf(name, isYou) {
	const id = memberIdFor(name, false, isYou == true);
	return members.find(member => member.id == id) ?? null;
}

// A firm's AI colleague as the backend holds it: its member, in the office it
// was given (agent.setOffice) and with the face it was hired with.
function colleagueMember(agent) {
	const words = (agent.name ?? "").trim() || "AI colleague";
	let id = (agent.id ?? "") != "" ? colleagueIdsByAgent.get(agent.id) : null;
	if (id == null || members.some(member => member.id == id) == false) {
		// The row its name found before it was read, unless another colleague
		// holds that row; then a row of its own.
		const fromName = `real_ai_${slugOf(words)}`;
		const holder = members.find(member => member.id == fromName);
		id = holder == null || (holder.agentId ?? "") == "" || holder.agentId == agent.id ? fromName : `real_ai_${slugOf(agent.id ?? words)}`;
		placeMember(id, words, true);
		if ((agent.id ?? "") != "") colleagueIdsByAgent.set(agent.id, id);
	}
	const member = members.find(item => item.id == id);
	if (member != null && member.isReal == true) {
		// Its name finds it, unless a colleague at work answers to the same name.
		const held = idsByName.get(nameKey(words, true));
		const holder = held != null && held != id ? members.find(item => item.id == held) : null;
		if (agent.status != "stopped" || holder == null || holder.isArchived == true) nameMember(member, words, true);
		else if (member.name != words) {
			member.name = words;
			member.initials = initialsOf(words);
		}
		const office = agent.office ?? {};
		member.species = speciesOf(agent.name, agent.character);
		if (office.title) member.role = office.title;
		if (office.ranksWith) member.ranksWith = office.ranksWith;
		if (office.practice) member.practice = office.practice;
		// Its area alone: what it does is said under its own name (the owner,
		// 2026-11-02).
		member.area = office.area || member.area || "";
		member.agentId = agent.id;
		// Where it runs, and the model chosen for it there (D-185): a colleague
		// another computer runs is known here by its card, which names none.
		member.runsHere = agent.isHere != false;
		// The seat of the lawyer whose computer runs it, so it is there while
		// that computer is (components/Avatar/online.js).
		member.hostActorId = agent.responsibleActorId || member.hostActorId || "";
		// Held here from the firm's record (D-213): changed where it runs.
		member.isFirmCopy = agent.firmCopy == true;
		// Archived (D-214): apart from the team, until restored.
		member.isArchived = agent.status == "stopped";
		member.goal = agent.goal ?? member.goal ?? "";
		member.manner = agent.manner ?? member.manner ?? "";
		member.character = agent.character ?? member.character ?? "";
		member.office = agent.office ?? member.office ?? {};
		if (agent.model !== undefined) member.ownModel = agent.model;
	}
	return id;
}

// The models a colleague may think with (D-185): those the firm's connected
// accounts give for its reasoning, and the firm's default among them.
export async function colleagueModelChoices() {
	const api = nativeApp();
	const [capabilities, status] = await Promise.all([api.getAiSetupCapabilities(), api.getOnboardingStatus()]);
	const reasoning = (capabilities?.roles ?? []).find(role => role.role == "reasoning");
	const models = (reasoning?.eligibleModels ?? []).map(model => ({ providerId: model.providerId ?? "local", modelId: model.modelId,
		name: model.displayName || model.modelId }));
	const assigned = (status?.localSetup?.roleAssignments ?? []).find(entry => entry.role == "reasoning");
	const firmDefault = assigned?.providerId && assigned?.modelId ? { providerId: assigned.providerId, modelId: assigned.modelId } : null;
	return { models, firmDefault };
}

// Chooses the model a colleague thinks with, or the firm's default with none
// named; its page shows the choice once the firm is read again.
export async function chooseColleagueModel(agentId, model) {
	await nativeApp().setAgentModel(agentId, model?.providerId ?? "", model?.modelId ?? "");
	await readFirm();
}

// ---- The firm's colleagues ------------------------------------------------------

// Every AI colleague is the firm's (D-157): read whether a Matter is open or
// none is, each with its personal chat with the member here, kept on the
// member's desk - never a Matter. Its faces, offices and chats come from here.
let isReadingFirm = false;
let isFirmStale = false;

// The firm read again: its people, as its record says who sits in each seat
// (D-209), and its colleagues; after the member changed who sits in a seat.
export function refreshFirm() {
	return readFirm();
}

function takeOwnName(person) {
	const name = (person.name ?? "").trim();
	const current = members.find(item => item.id == viewerId);
	if (current?.name == name) return;
	// The member here, renamed: the same row under the new name, never a
	// second member beside them (the owner, 2026-11-02).
	if (current != null && current.isReal == true && current.isSeatStandIn != true) {
		nameMember(current, name, false);
		placeOffice(current, person);
		current.actorId = person.actorId ?? null;
		return;
	}
	const id = memberIdFor(name, false, false);
	// The seat's login stood for them until now: it is no one else.
	if (current != null && current.isSeatStandIn == true && current.id != id) {
		const at = members.indexOf(current);
		if (at >= 0) members.splice(at, 1);
	}
	becomeViewer(id);
	const member = members.find(item => item.id == id);
	if (member != null && member.isReal == true) {
		placeOffice(member, person);
		member.actorId = person.actorId ?? null;
	}
}

async function readFirm() {
	if (hasNative() == false) return;
	if (isReadingFirm == true) {
		isFirmStale = true;
		return;
	}
	isReadingFirm = true;
	try {
		do {
			isFirmStale = false;
			const api = nativeApp();
			// The firm's people: everyone whose Singularity opened one of its
			// Matters known here, in the office their record states (D-168) -
			// the team, whichever Matter is open.
			const team = typeof api.getFirmTeam == "function" ? await api.getFirmTeam().catch(() => null) : null;
			for (const person of team?.people ?? []) {
				if ((person.name ?? "").trim() == "") continue;
				// The member here, as the firm's record names their seat once this
				// computer is checked (D-205, D-209): their name stands for the seat's
				// login the window showed until it came.
				if (person.isYou == true) {
					takeOwnName(person);
					continue;
				}
				// A record of the member here from before they signed in is still
				// them: their own office stands as they state it now.
				const id = firmPersonMember(person);
				if (id == viewerId) continue;
				const member = members.find(item => item.id == id);
				if (member != null && member.isReal == true) {
					placeOffice(member, person);
					// Their seat, where the firm's record says who sits in it (D-209).
					member.actorId = person.actorId ?? null;
					// Where they stand with the firm (D-214).
					member.isArchived = person.archived == true;
					member.isRemoved = person.removed == true;
				}
			}
			const listed = await api.listAgents();
			const agents = (listed?.agents ?? []).filter(agent => agent.status != "stopped" && agent.status != "waiting_for_approval"
				&& agent.status != "draft");
			const conversations = {};
			await Promise.all(agents.map(async agent => {
				if ((agent.chatId ?? "") == "" || (agent.chatProjectId ?? "") == "") return;
				conversations[agent.chatId] = await api.listChatConversation(agent.chatProjectId, agent.chatId).catch(() => null);
			}));
			// The files the member and the colleagues sent each other, kept on the
			// member's desk (D-157, D-212).
			const desks = [...new Set(agents.map(agent => agent.chatProjectId ?? "").filter(id => id != ""))];
			const documents = Object.fromEntries(await Promise.all(desks.map(async id => [id, await personalDocuments(id)])));
			const firmIds = new Set(agents.map(colleagueMember));
			// The firm's archived colleagues stand apart on the team (D-214).
			for (const agent of (listed?.agents ?? []).filter(entry => entry.status == "stopped" && (entry.matterId ?? "") == "")) {
				firmIds.add(colleagueMember(agent));
			}
			// The firm's colleagues its other computers run, as the firm's record
			// holds them (D-203): every computer lists the whole firm's.
			const elsewhere = typeof api.listFirmColleagues == "function" ? await api.listFirmColleagues().catch(() => null) : null;
			const remote = (elsewhere?.colleagues ?? [])
				.filter(colleague => agents.some(agent => agent.id == colleague.agentId) == false)
				.map(colleague => ({ id: colleague.agentId, name: colleague.name, status: colleague.status || "active", isHere: false,
					responsibleName: colleague.responsibleName ?? "", responsibleActorId: colleague.responsibleActorId ?? "",
					office: colleague.office ?? {}, character: colleague.character ?? "",
					goal: colleague.goal ?? "", manner: colleague.manner ?? "", state: "" }));
			for (const colleague of remote) firmIds.add(colleagueMember(colleague));
			// The firm's colleagues are those it has, here or on its other
			// computers: one deleted is known only by the record a Matter keeps
			// of its work, and is no one on the team (the owner, 2026-11-02).
			for (const member of members) {
				if (member.isReal == true && member.kind == "ai_colleague") member.isFirmColleague = firmIds.has(member.id);
			}
			// The firm's people, for a personal chat with anyone of them (D-042).
			publish({ firm: { agents, remote, conversations, documents, people: team?.people ?? snapshot.firm?.people ?? [] } });
		} while (isFirmStale == true);
	} catch {
		// The colleagues stay as they were read last.
	} finally {
		isReadingFirm = false;
	}
	readColleagueRecords();
}

// An AI colleague's record (ORG-8, PHIL-13; the owner, 2026-11-02: "what is
// the record and how does it work?"): what it delivered across the Matters
// open here and what the lawyers made of it. A result is a review it wrote -
// each version accepted or returned by a lawyer - or a written step of its
// work with no review of its own; each decision is one lawyer action, and the
// attention a result cost is the lawyer's actions until it was accepted.
let isReadingRecords = false;

async function readColleagueRecords() {
	const api = nativeApp();
	if (isReadingRecords || typeof api?.listDocumentReviews != "function") return;
	isReadingRecords = true;
	try {
		const ids = [...new Set(matters.filter(matter => matter.isReal == true && (matter.realMatterId ?? "") != "").map(matter => matter.realMatterId))];
		const reviews = [];
		const assignments = [];
		for (const id of ids) {
			const [reviewList, assignmentList] = await Promise.all([
				api.listDocumentReviews(id).catch(() => null),
				typeof api.listAssignments == "function" ? api.listAssignments(id).catch(() => null) : null,
			]);
			reviews.push(...(reviewList?.reviews ?? []));
			assignments.push(...(assignmentList?.assignments ?? []));
		}
		let isChanged = false;
		for (const agent of colleagues()) {
			const member = members.find(item => item.id == colleagueIdsByAgent.get(agent.id));
			if (member == null) continue;
			const words = recordWords(resultsOf(agent, reviews, assignments));
			if (member.record != words) {
				member.record = words;
				isChanged = true;
			}
		}
		if (isChanged) publish({});
	} finally {
		isReadingRecords = false;
	}
}

// Each result of the colleague: whether a lawyer accepted it, how many times
// it came back, and the lawyer's actions it took.
export function resultsOf(agent, reviews, assignments) {
	const results = [];
	for (const review of reviews.filter(item => isReviewOf(item, agent))) {
		const states = (review.versions ?? []).map(version => version.state);
		const returned = states.filter(state => state == "returned").length;
		const accepted = states.includes("accepted");
		results.push({ accepted, returned, actions: returned + (accepted ? 1 : 0), delivered: states.length > 0 });
	}
	for (const assignment of assignments.filter(item => (item.agentId ?? "") != "" ? item.agentId == agent.id : item.agentName == agent.name)) {
		for (const step of (assignment.steps ?? []).filter(item => (item.reviewId ?? "") == "")) {
			if (step.state != "accepted" && step.state != "returned") continue;
			results.push({ accepted: step.state == "accepted", returned: step.state == "returned" ? 1 : 0, actions: 1, delivered: true });
		}
	}
	return results;
}

// The record in one or two sentences: "3 results accepted and 1 returned. 1
// lawyer action per accepted result, at the median."
export function recordWords(results) {
	const delivered = results.filter(result => result.delivered).length;
	const accepted = results.filter(result => result.accepted);
	const returned = results.reduce((sum, result) => sum + result.returned, 0);
	const count = (number, one, many) => `${number} ${number == 1 ? one : many}`;
	if (accepted.length == 0 && returned == 0) {
		// Empty, it says what it will count (the owner, 2026-11-02: "so what is
		// Record for the colleagues?").
		return delivered == 0 ? "No results yet. Each piece of work it delivers counts here once a lawyer accepts it or sends it back."
			: `${count(delivered, "result", "results")} delivered, none decided yet.`;
	}
	if (accepted.length == 0) return `${count(returned, "result", "results")} returned, none accepted yet.`;
	const actions = accepted.map(result => result.actions).sort((one, other) => one - other);
	const middle = actions.length % 2 == 1 ? actions[(actions.length - 1) / 2] : (actions[actions.length / 2 - 1] + actions[actions.length / 2]) / 2;
	const median = Number.isInteger(middle) ? middle : middle.toFixed(1);
	return `${count(accepted.length, "result", "results")} accepted and ${returned} returned. ${median} lawyer ${median == 1 ? "action" : "actions"} per accepted result, at the median.`;
}

// The firm's colleagues as this window knows them: the firm's own, fresh, and
// any the open Matter names besides.
function colleagues() {
	const firm = snapshot.firm?.agents ?? [];
	const seen = new Set(firm.map(agent => agent.id));
	const inMatter = (snapshot.agents ?? []).filter(agent => seen.has(agent.id) == false);
	for (const agent of inMatter) seen.add(agent.id);
	// Then those the firm's record names that no Matter here names (D-203).
	const remote = (snapshot.firm?.remote ?? []).filter(agent => seen.has(agent.id) == false);
	return [...firm.map(agent => ({ ...agent, isHere: true })), ...inMatter, ...remote];
}

// The project a chat is kept in: a colleague's personal chat on the member's
// desk, or a thread of the open Matter.
function projectOfChat(chatId) {
	const agent = (snapshot.firm?.agents ?? []).find(entry => entry.chatId == chatId && (entry.chatProjectId ?? "") != "");
	return agent?.chatProjectId ?? snapshot.matterId;
}

// The project whose Papinian Guard masks what a chat sends (data/masking.js).
export function protectionProjectOfChat(chatId) {
	return projectOfChat(chatId) ?? null;
}

// The open Matter's own id, or null.
export function openMatterRealId() {
	return snapshot.matterId ?? null;
}

function paragraphsOf(text) {
	return (text ?? "").split(/\n\s*\n/).map(paragraph => paragraph.trim()).filter(paragraph => paragraph != "");
}

function textOf(turn) {
	if (turn.isMemberMessage == true) return turn.prompt ?? "";
	if (Array.isArray(turn.responseSegments) && turn.responseSegments.length > 0) {
		return turn.responseSegments.map(segment => segment.text).join("");
	}
	return turn.prompt ?? "";
}

// What a message carries, the same for a person's and a colleague's: the
// message it quotes, the Matter's documents it hands over with what was not
// checked, the answers it offers, how the members reacted to it, and its edit
// or deletion - as the backend keeps them.
function carriedBy(turn, byId, viewerMemberId, documents, memberOfActor) {
	const quoted = turn.quoteMessageId ? byId.get(turn.quoteMessageId) : null;
	const reactions = {};
	for (const reaction of turn.reactions ?? []) {
		reactions[reaction.emoji] = (reaction.actorIds ?? []).map(actor => actor == viewerMemberId ? viewerId : memberOfActor(actor));
	}
	const files = (turn.attachmentDocumentIds ?? []).map(documentId => {
		const document = documents.find(entry => entry.id == documentId);
		return { documentId, name: document?.displayName ?? document?.name ?? "A document", sizeBytes: document?.sizeBytes ?? null };
	});
	const placed = turn.place?.documentId ? documents.find(entry => entry.id == turn.place.documentId) : null;
	// When it reached the other (D-212): for the member's own message, the first
	// time another member's computer had it, and whose; for anyone else's, when
	// this member's computer had it.
	const isOwnTurn = turn.isMemberMessage == true && ((turn.authorActorId ?? "") == "" || turn.authorActorId == viewerMemberId);
	const reached = Object.entries(turn.received ?? {})
		.filter(([actor, at]) => actor != viewerMemberId && (at ?? "") != "")
		.sort((first, second) => first[1].localeCompare(second[1]))[0] ?? null;
	const receivedAt = isOwnTurn ? reached?.[1] ?? null : turn.received?.[viewerMemberId] ?? null;
	return {
		receivedAt,
		receivedBy: isOwnTurn ? (reached != null ? memberOfActor(reached[0]) : null) : viewerId,
		// The place in a document the message is about, with the document's name.
		place: turn.place?.documentId ? { ...turn.place, documentName: placed?.displayName ?? placed?.name ?? "" } : null,
		quote: turn.quoteMessageId ? {
			postId: turn.quoteMessageId,
			authorId: quoted?.authorId ?? viewerId,
			text: quoted ? (quoted.isDeleted ? "A deleted message" : quoted.paragraphs.join(" ")) : "A message no longer in this thread",
		} : null,
		realFiles: files.length > 0 ? files : null,
		notChecked: (turn.notChecked ?? "") != "" ? turn.notChecked : null,
		ask: (turn.askOptions ?? []).length > 0 ? { options: turn.askOptions } : null,
		answerOption: (turn.answerOption ?? "") != "" ? turn.answerOption : null,
		realReactions: reactions,
		isEdited: (turn.editedAt ?? "") != "",
		isDeleted: (turn.deletedAt ?? "") != "" || pendingDeletions.has(turn.taskId),
		canChange: turn.canChange == true,
	};
}

// A thread's closing or reopening, as the prototype draws one.
function threadEventOf(turn, authorId) {
	if (turn.threadEvent == "closed") return { id: turn.taskId, kind: "closed", byId: authorId, at: turn.createdAt, conclusion: textOf(turn), isReal: true };
	return { id: turn.taskId, kind: "reopened", byId: authorId, at: turn.createdAt, isReal: true };
}

// A thread's turns as the prototype draws posts: the person's words, and each
// colleague's message - every one keeps its turn and its review.
function postsOf(conversation, agents, documents) {
	const viewerMemberId = conversation.viewer?.memberId ?? "";
	const posts = [];
	const byId = new Map();
	// Who reacted, by the members the thread knows: someone who wrote in it is
	// that member; anyone else is still shown, as another member.
	const actors = new Map();
	for (const turn of conversation.turns) {
		if (turn.isMemberMessage == true && (turn.authorActorId ?? "") != "" && turn.authorActorId != viewerMemberId) {
			actors.set(turn.authorActorId, personIdForActor(turn.authorActorId, turn.authorName));
		}
	}
	// A colleague's reaction is put under its own id (the owner, 2026-10-26:
	// "teach the AI agents to put reactions on messages").
	const memberOfActor = actor => {
		if (actors.has(actor)) return actors.get(actor);
		const colleague = actor.startsWith("agent_") ? agents.find(entry => entry.id == actor) : null;
		if (colleague != null) return colleagueIdFor(colleague.id, colleague.name);
		return memberIdFor("Another member", false, false);
	};
	for (const turn of conversation.turns) {
		if (turn.isMemberMessage == true) {
			const isViewer = turn.authorActorId == viewerMemberId || turn.authorActorId == "";
			if ((turn.threadEvent ?? "") != "") {
				posts.push(threadEventOf(turn, isViewer ? viewerId : personIdForActor(turn.authorActorId, turn.authorName)));
				continue;
			}
			const post = {
				id: turn.taskId,
				authorId: isViewer ? viewerId : personIdForActor(turn.authorActorId, turn.authorName),
				at: turn.createdAt,
				paragraphs: paragraphsOf(textOf(turn)),
				reviewId: turn.documentReviewId ?? null,
				isReal: true,
				// The colleagues it names.
				mentionIds: (turn.mentions ?? []).filter(id => id.startsWith("agent_")),
				// It names the member at this computer with @.
				addressesViewer: isViewer == false && viewerMemberId != "" && (turn.mentions ?? []).includes(viewerMemberId),
				authorActorId: turn.authorActorId ?? "",
				heldFor: heldColleagues(turn, agents),
			};
			Object.assign(post, carriedBy(turn, byId, viewerMemberId, documents, memberOfActor));
			posts.push(post);
			byId.set(post.id, post);
			continue;
		}
		if (turn.isAgentMessage == true) {
			const agent = agents.find(entry => entry.id == turn.agentId);
			if ((turn.threadEvent ?? "") != "") {
				posts.push(threadEventOf(turn, colleagueIdFor(turn.agentId, agent?.name ?? turn.authorName)));
				continue;
			}
			const post = {
				id: turn.taskId,
				authorId: colleagueIdFor(turn.agentId, agent?.name ?? turn.authorName),
				at: turn.createdAt,
				paragraphs: paragraphsOf(textOf(turn)),
				reviewId: turn.documentReviewId ?? null,
				isReal: true,
				// The colleagues and people it names (D-168): a colleague hands a part
				// of its work to a member by name, and the member it names finds it
				// among what asks them.
				mentionIds: (turn.mentions ?? []).filter(id => id.startsWith("agent_")),
				addressesViewer: viewerMemberId != "" && (turn.mentions ?? []).includes(viewerMemberId),
			};
			Object.assign(post, carriedBy(turn, byId, viewerMemberId, documents, memberOfActor));
			posts.push(post);
			byId.set(post.id, post);
			continue;
		}
		// A request run through a model. A colleague's run answers a member's
		// message already in the thread, or does the colleague's own standing
		// work: only its answer is shown, as the colleague's own message - its
		// request is never a second copy of that message, and never anyone's
		// words but their author's. A person's own request of a model shows as
		// its author's, then its answer.
		const agent = agents.find(entry => entry.id == turn.agentId) ?? null;
		const isColleagueRun = (turn.agentId ?? "") != "";
		if (!isColleagueRun) {
			const isViewer = (turn.authorActorId ?? "") == "" || turn.authorActorId == viewerMemberId;
			posts.push({ id: `${turn.taskId}_q`, authorId: isViewer ? viewerId : personIdForActor(turn.authorActorId, turn.authorName || "Another member"), at: turn.createdAt, paragraphs: paragraphsOf(turn.prompt), isReal: true });
		}
		if (turn.status == "completed" && (turn.responseSegments ?? []).length > 0) {
			const post = {
				id: turn.taskId,
				authorId: isColleagueRun ? colleagueIdFor(turn.agentId, agent?.name ?? "A colleague") : memberIdFor("", true, false),
				at: turn.createdAt,
				paragraphs: paragraphsOf(textOf(turn)),
				isReal: true,
			};
			Object.assign(post, carriedBy(turn, byId, viewerMemberId, documents, memberOfActor));
			posts.push(post);
			byId.set(post.id, post);
		}
	}
	// A question the viewer answered offers its answers no more.
	for (const post of posts) {
		if (post.answerOption == null || post.authorId != viewerId || post.quote == null || post.isDeleted) continue;
		const question = byId.get(post.quote.postId);
		if (question != null) question.realAnswer = post.answerOption;
	}
	return posts;
}

// ---- Reading it from the backend ---------------------------------------------------

// A Matter's client, from its description as the firm writes it: "For
// <client>: <what it is>" (New Matter, as the prototype's Matters read).
function clientOf(description) {
	const found = /^For ([^:]{1,120}): /.exec(description ?? "");
	return found != null ? found[1].trim() : "";
}

// The firm's Matters known here, as the window shows a Matter: its name, its
// client, what it is, and who is on it once it has been read.
function placeMatters(list) {
	const kept = new Map(matters.filter(matter => matter.isReal == true).map(matter => [matter.id, matter]));
	for (let index = matters.length - 1; index >= 0; --index) {
		if (matters[index].isReal == true) matters.splice(index, 1);
	}
	for (const matter of list) matters.push(shownMatterOf(matter, kept.get(uiMatterIdOf(matter.id))));
}

// One Matter of the firm as the window shows it, from what the backend says
// of it; `kept` is the one shown before, whose members it keeps.
function shownMatterOf(matter, kept) {
	const id = uiMatterIdOf(matter.id);
	const description = (matter.description ?? "").trim();
	// Its mark as a member set it (D-200): a colour and an icon, or its
	// initials, kept with the Matter (matter.update's appearance).
	const appearance = matter.appearance ?? {};
	const icon = appearance.markGlyph || appearance.icon || "";
	return {
		id,
		realMatterId: matter.id,
		name: matter.name,
		// A Matter of the firm's own, with no client named, is the firm's.
		client: clientOf(description) || organisation.name,
		color: pigments[appearance.markGround] ?? appearance.color ?? pigments.blue,
		...(icon != "" ? { icon } : {}),
		appearance,
		description,
		directoryPath: matter.directoryPath ?? null,
		memberIds: kept?.memberIds ?? [viewerId],
		updatedAt: matter.updatedAt ?? "",
		// Archived for every computer of it (D-214): out of the open Matters,
		// as the prototype's closed one stands.
		archivedAt: matter.archivedAt ?? "",
		// The rules of its correspondence intake (D-228): a letter to `mailbox`
		// from `from` is filed here as Outlook shows it.
		intake: Array.isArray(matter.correspondenceIntake) ? matter.correspondenceIntake : kept?.intake ?? [],
		isClosed: (matter.archivedAt ?? "") != "",
		isReal: true,
	};
}

// A Matter changed on another computer - renamed, archived, given another
// mark or client - shows here as it is now, from the summaries every change
// reads anyway (the owner, 2026-11-02: "I rename the project on macOS, but
// nothing changes on Windows"). True when one changed.
function takeSummaries(summaries) {
	const byId = new Map(summaries.map(summary => [uiMatterIdOf(summary.id), summary]));
	let isChanged = false;
	for (let index = 0; index < matters.length; ++index) {
		const kept = matters[index];
		const summary = kept.isReal == true ? byId.get(kept.id) : null;
		if (summary == null) continue;
		const next = shownMatterOf({ ...summary, description: summary.description ?? kept.description }, kept);
		const isSame = next.name == kept.name && next.description == kept.description && next.archivedAt == kept.archivedAt
			&& JSON.stringify(next.appearance) == JSON.stringify(kept.appearance);
		if (isSame) continue;
		matters[index] = next;
		isChanged = true;
	}
	return isChanged;
}

// The member's personal conversations with people (D-042), each with what
// was written in it - in every chat of it, as two computers that began it at
// once began one each. Read once at a time; asked again while it reads, it
// reads once more after.
let isReadingPersonal = false;
let isPersonalStale = false;

export async function readPersonal() {
	const api = nativeApp();
	if (typeof api?.listPersonalChats != "function") return;
	if (isReadingPersonal == true) {
		isPersonalStale = true;
		return;
	}
	isReadingPersonal = true;
	try {
		do {
			isPersonalStale = false;
			const listed = await api.listPersonalChats().catch(() => null);
			// The conversations the member cleared here (chat.clearConversation):
			// what was said up to then is no longer shown.
			const cleared = typeof api.listClearedConversations == "function" ? await api.listClearedConversations().catch(() => null) : null;
			const clearedOf = new Map((cleared?.conversations ?? []).map(item => [item.chatId, item.clearedBefore ?? ""]));
			if (cleared != null) publish({ cleared: Object.fromEntries(clearedOf) });
			const personal = {};
			for (const entry of listed?.conversations ?? []) {
				const chatIds = (entry.chatIds ?? []).length > 0 ? entry.chatIds : [entry.chatId];
				const read = await Promise.all(chatIds.map(chatId => api.listChatConversation(entry.projectId, chatId).catch(() => null)));
				const clearedBefore = chatIds.map(chatId => clearedOf.get(chatId) ?? "").filter(Boolean).sort().at(-1) ?? "";
				const turns = read.flatMap(conversation => conversation?.turns ?? [])
					.filter(turn => clearedBefore == "" || (turn.createdAt ?? "") > clearedBefore)
					.sort((first, second) => (first.createdAt ?? "").localeCompare(second.createdAt ?? ""));
				const viewer = read.find(conversation => conversation?.viewer != null)?.viewer;
				// What the other wrote is here now: said once, so they see when it
				// reached this member (D-212). Its record changed: read again, as a
				// change that came before the window listened is never heard.
				const me = viewer?.memberId ?? "";
				if (me != "" && typeof api.markChatReceived == "function") {
					read.forEach((conversation, index) => {
						const isNew = (conversation?.turns ?? []).some(turn => turn.isMemberMessage == true && (turn.authorActorId ?? "") != ""
							&& turn.authorActorId != me && (turn.deletedAt ?? "") == "" && (turn.received?.[me] ?? "") == "");
						if (isNew == false) return;
						api.markChatReceived(entry.projectId, chatIds[index])
							.then(answer => {
								if ((answer?.marked ?? 0) > 0) readPersonal();
							})
							.catch(() => {});
					});
				}
				// The files the two sent each other, kept in the conversation's own
				// folder (D-212): each attachment by its name, kind and size.
				const documents = await personalDocuments(entry.projectId);
				personal[entry.otherActorId] = { ...entry, documents, conversation: { ...(viewer != null ? { viewer } : {}), turns } };
			}
			publish({ personal });
		} while (isPersonalStale == true);
	} finally {
		isReadingPersonal = false;
	}
}

// Opens the member's personal conversation with a person of the firm, made
// when it is not yet: its project and chat, to write the first message in.
// A conversation cleared by the member here (the owner, 2026-11-04: "a way to
// clear the messages of a direct conversation"): its messages up to now leave
// it on this computer, and an AI colleague answering from here begins afresh;
// the other member's are theirs (Chat/ConversationClearing). `entry` is the
// conversation as Messages holds it. Returns its Undo.
export async function clearConversation(entry) {
	const api = nativeApp();
	if (typeof api?.clearConversation != "function") throw new Error("This conversation can't be cleared here.");
	const personal = entry?.actorId != null ? (snapshot.personal ?? {})[entry.actorId] : null;
	const chatIds = personal != null ? ((personal.chatIds ?? []).length > 0 ? personal.chatIds : [personal.chatId])
		: entry?.send?.kind == "chat" && (entry.send.chatId ?? "") != "" ? [entry.send.chatId] : [];
	if (chatIds.length == 0) throw new Error("This conversation has nothing to clear yet.");
	const answers = [];
	for (const chatId of chatIds) answers.push(await api.clearConversation(chatId));
	publish({ cleared: { ...(snapshot.cleared ?? {}), ...Object.fromEntries(answers.map(answer => [answer.chatId, answer.clearedBefore])) } });
	await readPersonal();
	return async () => {
		for (const [index, chatId] of chatIds.entries()) await api.restoreConversation(chatId, answers[index]?.previouslyClearedBefore ?? null);
		const restored = { ...(snapshot.cleared ?? {}) };
		for (const [index, chatId] of chatIds.entries()) {
			const before = answers[index]?.previouslyClearedBefore ?? null;
			if (before == null) delete restored[chatId];
			else restored[chatId] = before;
		}
		publish({ cleared: restored });
		await readPersonal();
	};
}

export async function openPersonalChat(actorId) {
	const opened = await nativeApp().openPersonalChat(actorId);
	await readPersonal();
	return opened;
}

// The member at this computer as the backend names them (OrganisationViewer's
// memberId): whose words are "yours" in what the Matters say last.
let viewerActorId = "";

// A Matter's name as the window shows it now, after a rename on any of the
// firm's computers, or null for one not listed here.
export function matterNameOf(realMatterId) {
	return matters.find(matter => matter.isReal == true && matter.realMatterId == realMatterId)?.name ?? null;
}

// The seat the member here sits in, as the firm's record names them (D-209),
// or null: what their own profile is edited by, whatever the sign-in's
// answer says (the owner, 2026-11-02: "as the administrator I must be able
// to edit my own profile too").
export function viewerSeatId() {
	return viewerActorId.startsWith("seat:") ? viewerActorId.slice(5) : null;
}

// What each Matter says last, as the sidebar shows it under its name
// (D-200): the latest message of its threads - never a colleague's own chat -
// from the Matters' summaries. Read once at a time; asked again while it
// reads, it reads once more after.
let isReadingSummaries = false;
let isSummariesStale = false;
let summariesTimer = null;

export async function readMatterSummaries() {
	const api = nativeApp();
	if (typeof api?.listMatterSummaries != "function") return;
	if (isReadingSummaries == true) {
		isSummariesStale = true;
		return;
	}
	isReadingSummaries = true;
	try {
		do {
			isSummariesStale = false;
			const listed = await api.listMatterSummaries().catch(() => null);
			if (listed == null) break;
			takeSummaries(listed.matters ?? []);
			const latest = {};
			const threadsLatest = [];
			// How many files and threads each Matter holds, for the Matters page.
			const counts = {};
			for (const summary of listed.matters ?? []) {
				const threads = (summary.chats ?? []).filter(chat => chat.archived != true && (chat.deletedAt ?? "") == "" && (chat.agentId ?? "") == "");
				counts[uiMatterIdOf(summary.id)] = { files: summary.documentCount ?? 0, threads: threads.length };
				let found = null;
				for (const chat of summary.chats ?? []) {
					const message = chat.lastMessage;
					if (message == null || chat.archived == true || (chat.deletedAt ?? "") != "" || (chat.agentId ?? "") != "") continue;
					const said = { ...message, at: message.at ?? "", chatId: chat.id, title: chat.title ?? "", matterId: uiMatterIdOf(summary.id) };
					threadsLatest.push(said);
					if (found == null || said.at > found.at) found = said;
				}
				if (found != null) latest[uiMatterIdOf(summary.id)] = found;
			}
			publish({ latest, threadsLatest, counts });
		} while (isSummariesStale == true);
	} finally {
		isReadingSummaries = false;
	}
}

// Changes come in bursts - a message, its answer, a thread's title - so they
// are read once they settle.
function scheduleSummaries() {
	if (summariesTimer != null) clearTimeout(summariesTimer);
	summariesTimer = setTimeout(() => {
		summariesTimer = null;
		readMatterSummaries();
	}, 250);
}

// What a Matter of the firm said last (D-200): who wrote it - "You" for the
// member here, a person or an AI colleague by name, no one for a model's
// answer to a person's own request - its words in one line, and how many
// files it handed over. Null for a Matter with no message yet.
export function realLatestOf(matterId) {
	const message = snapshot.latest?.[matterId];
	if (message == null) return null;
	const isViewer = viewerActorId != "" && message.authorActorId == viewerActorId;
	return {
		at: message.at,
		authorName: isViewer ? "" : (message.authorName ?? "").trim(),
		isViewer,
		text: message.text ?? "",
		files: message.attachmentCount ?? 0,
		mentions: message.mentions ?? [],
		addressesViewer: viewerActorId != "" && (message.mentions ?? []).includes(viewerActorId),
	};
}

// Every thread of the firm's Matters with what it said last, the latest
// first: who wrote it - a member of the window, the member here as theirs,
// or null where none is known - its title, its Matter and when (Recent).
export function realThreadsLatest() {
	return (snapshot.threadsLatest ?? [])
		.filter(message => isListedMatter(message.matterId))
		.map(message => {
			const isViewer = viewerActorId != "" && message.authorActorId == viewerActorId;
			const name = (message.authorName ?? "").trim();
			// Who wrote it, by their seat or the colleague's id; a name only where
			// an older record names no one.
			const byAgent = (message.agentId ?? "") != "" ? colleagueIdsByAgent.get(message.agentId) ?? null : null;
			const byActor = (message.agentId ?? "") == "" && (message.authorActorId ?? "") != "" ? personIdsByActor.get(message.authorActorId) ?? null : null;
			const author = isViewer ? viewerId : byAgent ?? byActor
				?? members.find(member => member.name == name && (message.agentId != "") == (member.kind == "ai_colleague"))?.id ?? null;
			return { chatId: message.chatId, threadId: `real:${message.chatId}`, title: message.title, matterId: message.matterId, at: message.at, authorId: author };
		})
		.sort((first, second) => second.at.localeCompare(first.at));
}

// ---- What the member has read (D-202) -------------------------------------------

// The member's read marks: by chat id, the time of the latest message they
// have seen there. Read once as the window connects; kept by the backend.
export async function readReadMarks() {
	const api = nativeApp();
	if (typeof api?.readMarks != "function") return;
	const answer = await api.readMarks().catch(() => null);
	if (answer != null) publish({ marks: laterOf(snapshot.marks ?? {}, answer.marks ?? {}) });
}

// Each conversation's later mark of two sets.
function laterOf(first, second) {
	const marks = { ...first };
	for (const [chatId, at] of Object.entries(second)) if ((marks[chatId] ?? "") < at) marks[chatId] = at;
	return marks;
}

// The member has seen a conversation up to `upTo`: shown at once, and kept
// for them unless a later time is kept already.
export function markChatRead(chatId, upTo) {
	if (hasNative() == false || (chatId ?? "") == "" || (upTo ?? "") == "") return;
	const marks = snapshot.marks ?? {};
	if ((marks[chatId] ?? "") >= upTo) return;
	publish({ marks: { ...marks, [chatId]: upTo } });
	if (typeof nativeApp().markRead != "function") return;
	nativeApp().markRead(chatId, upTo).then(answer => {
		if (answer?.marks != null) publish({ marks: laterOf(snapshot.marks ?? {}, answer.marks) });
	}, () => {});
}

function isOthersUnread(chatId, at, authorIsViewer) {
	return authorIsViewer == false && (at ?? "") > ((snapshot.marks ?? {})[chatId] ?? "");
}

// The firm's threads with something the member has not read: another's
// message after the member last saw the thread, whichever Matter it is in.
// Whether that message names the member (@) is said with it.
export function realUnreadThreads() {
	return (snapshot.threadsLatest ?? [])
		.filter(message => isListedMatter(message.matterId))
		.filter(message => isOthersUnread(message.chatId, message.at, viewerActorId != "" && message.authorActorId == viewerActorId))
		.map(message => ({ chatId: message.chatId, threadId: `real:${message.chatId}`, matterId: message.matterId, at: message.at,
			namesViewer: viewerActorId != "" && (message.mentions ?? []).includes(viewerActorId) }));
}

// Whether a real thread open in the window holds another's message the
// member has not seen.
export function isRealThreadUnread(thread) {
	const chatId = thread?.real?.chatId;
	if (chatId == null) return false;
	const last = [...(thread.posts ?? [])].reverse().find(post => post.isDeleted != true && (post.kind ?? "post") == "post");
	if (last == null) return realUnreadThreads().some(entry => entry.chatId == chatId);
	return isOthersUnread(chatId, last.at, last.authorId == viewerId);
}

// How many of another's messages in a direct conversation the member has not
// seen: its items after the member last saw it.
export function unreadInConversation(conversation) {
	const chatId = conversation?.send?.chatId;
	if (chatId == null) return 0;
	return (conversation.items ?? []).filter(item => isOthersUnread(chatId, item.at, item.isOwn == true)).length;
}

// A Matter's mark as the member sets it (D-200): its colour, and an icon or
// its initials, kept with the Matter so every computer it is on shows it.
// The rest of its appearance stays as it was.
export async function keepMatterMark(matterId, look) {
	const matter = matters.find(item => item.id == matterId);
	if (hasNative() == false || matter == null || matter.isReal != true) return;
	const appearance = {
		shortName: matter.appearance?.shortName ?? "",
		markText: matter.appearance?.markText ?? "",
		markShape: "circle",
		markGround: pigmentName(look.color ?? matter.color),
		// A change of colour keeps the icon; null is the initials.
		markGlyph: (Object.hasOwn(look, "icon") ? look.icon : matter.icon) ?? "",
	};
	matter.appearance = appearance;
	matter.color = appearance.markGround || matter.color;
	if (appearance.markGlyph != "") matter.icon = appearance.markGlyph;
	else delete matter.icon;
	await nativeApp().updateMatter(matter.realMatterId, { appearance });
}

// The firm's Matters known here, and those the firm's computers brought that
// are not open here yet (D-123): the window lists both. Those known here are
// listed at once. Looking for arrivals walks the folders the member opened to
// Singularity, which can take minutes, so it never holds the list back
// (2026-10-31: a home folder's walk kept the window without a Matter).
export async function readMatters() {
	const api = nativeApp();
	// This computer's Trash first (project.listTrash, D-247): it deletes what
	// has stood there 30 days, and what is in it leaves every list here.
	const trash = typeof api.listMatterTrash == "function" ? await api.listMatterTrash().catch(() => null) : null;
	const trashed = new Map((trash?.matters ?? []).map(entry => [entry.id, entry]));
	const listed = await api.listMatters();
	const every = (listed?.matters ?? listed ?? []).filter(matter => (matter.deletedAt ?? "") == "");
	const list = every.filter(matter => trashed.has(matter.id) == false);
	placeMatters(list);
	publish({
		trash: every.filter(matter => trashed.has(matter.id)).map(matter => ({
			id: matter.id, name: matter.name ?? "A Matter", client: matter.client ?? matter.clientName ?? "",
			trashedAt: trashed.get(matter.id).trashedAt, deletesAt: trashed.get(matter.id).deletesAt,
			// Its mark as it stood among the Matters.
			mark: shownMatterOf(matter),
		})).sort((first, second) => second.trashedAt.localeCompare(first.trashedAt)),
	});
	// One opened here is no longer an arrival, before the search says so again.
	publish({ arrivals: (snapshot.arrivals ?? []).filter(arrival => list.some(matter => matter.id == arrival.id) == false) });
	lookForArrivedMatters({ isRoutine: true });
	return list;
}

// The firm's Matters in the member's folders that are not open here yet, one
// search at a time: asked for again while one runs, it searches once more
// after it. Said as it ends, against the Matters known here then.
let arrivalsSearch = null;
let isArrivalsStale = false;
let arrivalsSearchedAt = 0;
// A re-read of the Matters looks for arrivals at most this often; news of a
// Matter that came looks at once.
const ARRIVALS_EVERY_MS = 30 * 1000;

export function lookForArrivedMatters({ isRoutine = false } = {}) {
	if (isRoutine == true && arrivalsSearch == null && Date.now() - arrivalsSearchedAt < ARRIVALS_EVERY_MS) return Promise.resolve();
	if (arrivalsSearch != null) {
		isArrivalsStale = true;
		return arrivalsSearch;
	}
	arrivalsSearchedAt = Date.now();
	arrivalsSearch = (async () => {
		try {
			do {
				isArrivalsStale = false;
				let found = null;
				try {
					found = await nativeApp().discoverProjects();
				} catch {
					// Nothing found is said as nothing arrived.
				}
				const known = new Set(matters.filter(matter => matter.isReal == true).map(matter => matter.realMatterId));
				const arrivals = (found?.projects ?? [])
					.filter(project => (project.id ?? "") != "" && project.hasDuplicateLocation != true && known.has(project.id) == false)
					.map(project => ({ id: project.id, name: project.name ?? "A Matter", path: project.path }));
				// With no Matter open here, one that arrived can be opened.
				const status = snapshot.status == "absent" && arrivals.length > 0 ? "discoverable"
					: snapshot.status == "discoverable" && arrivals.length == 0 ? "absent" : snapshot.status;
				publish({ arrivals, status });
			} while (isArrivalsStale == true);
		} finally {
			arrivalsSearch = null;
		}
	})();
	return arrivalsSearch;
}

// The Matter the window opens on: the one the member last opened here, else
// the one most lately changed.
function preferredMatterId(list) {
	let kept = null;
	try {
		kept = window.localStorage.getItem(OPEN_MATTER_KEY);
	} catch {
		// Not kept: the latest is opened.
	}
	if (kept != null && list.some(matter => matter.id == kept)) return kept;
	return [...list].sort((one, other) => (other.updatedAt ?? "").localeCompare(one.updatedAt ?? ""))[0]?.id ?? null;
}

// The Matter's threads among its chats, the latest first: none deleted or
// archived, and none that is a colleague's own chat.
function threadChatsOf(chatList, agents) {
	const agentChats = new Set((agents ?? []).filter(agent => agent.isHere == true).map(agent => agent.chatId).filter(Boolean));
	// An archived thread stays readable among the Matter's archived ones (D-214).
	return (chatList?.chats ?? [])
		.filter(chat => (chat.deletedAt ?? "") == "" && agentChats.has(chat.id) == false
			&& (chat.agentId ?? "") == "")
		.sort((first, second) => (second.updatedAt ?? "").localeCompare(first.updatedAt ?? ""));
}

async function readAll(matterId) {
	const api = nativeApp();
	const [chatList, agentList, documentList, reviewList, shared, letterList, assignmentList] = await Promise.all([
		api.listProjectChats(matterId),
		api.listAgents(matterId),
		api.listMatterDocuments(matterId),
		api.listDocumentReviews(matterId),
		// Who works on the Matter on other computers, from its shared folder.
		api.getSharedActivity(matterId).catch(() => null),
		// The letters filed to it (D-106).
		api.listLetters != null ? api.listLetters(matterId).catch(() => null) : Promise.resolve(null),
		// The assignments colleagues carried out, step by step (D-119).
		api.listAssignments != null ? api.listAssignments(matterId).catch(() => null) : Promise.resolve(null),
	]);
	// The Matter's AI colleagues: those this computer runs, and those another
	// lawyer's computer runs, known from the shared folder. A message names
	// either; the computer that runs a colleague takes the work up there.
	// At work only: an archived or unapproved colleague takes no part (D-214).
	const here = (agentList?.agents ?? []).filter(agent => agent.status != "stopped" && agent.status != "draft"
		&& agent.status != "waiting_for_approval").map(agent => ({ ...agent, isHere: true }));
	const elsewhere = (shared?.agents ?? [])
		.filter(card => ["archived", "stopped", "deleted"].includes(card.status ?? "active") == false
			&& here.some(agent => agent.id == card.agentId) == false)
		.map(card => ({ id: card.agentId, name: card.name, status: card.status ?? "active", isHere: false, responsibleName: card.responsibleName ?? "",
			// Its office and face as the computer that runs it holds them.
			office: card.office ?? {}, character: card.character ?? "",
			// At work on the computer that runs it, as its card says on every other.
			state: card.state ?? "" }));
	const agents = [...here, ...elsewhere];
	const chats = threadChatsOf(chatList, agents);
	const conversations = {};
	const asked = {};
	for (const chat of chats) {
		asked[chat.id] = performance.now();
		conversations[chat.id] = await api.listChatConversation(matterId, chat.id);
	}
	// Each colleague's own chat, run here: the member's personal chat with it
	// in Messages (D-042). It is no thread of the Matter.
	for (const agent of here) {
		if ((agent.chatId ?? "") == "") continue;
		asked[agent.chatId] = performance.now();
		conversations[agent.chatId] = await api.listChatConversation(matterId, agent.chatId).catch(() => null);
	}
	return {
		chats,
		agents,
		documents: documentList?.documents ?? [],
		reviews: reviewList?.reviews ?? [],
		letters: letterList?.letters ?? [],
		assignments: assignmentList?.assignments ?? [],
		// Each document the Matter took in, lost, or saw change, with when and
		// whose it was: the overview's history (the owner, 2026-11-02).
		documentChanges: shared?.changes ?? [],
		// The Matter's people, by their actor ids: whom "@Name" in a message
		// names (ORG-14).
		people: (shared?.members ?? [])
			.map(member => ({ actorId: member.actorId ?? "", name: (member.name ?? "").trim(), isYou: member.isYou == true,
				position: (member.position ?? "").trim(), practice: (member.practice ?? "").trim(),
				// Their computers, by device: where a colleague they answer for runs.
				computers: (member.computers ?? []).map(computer => computer.deviceId ?? "").filter(device => device != ""),
				// Given the Matter from this computer, and not on it yet (D-205).
				invited: member.invited == true,
				// When their computer first joined the Matter.
				firstSeenAt: member.firstSeenAt ?? "" }))
			.filter(person => person.actorId != "" && person.name != ""),
		conversations,
		asked,
	};
}

// Who began a thread, as the Matter's records name its people: the member
// here when it was them or no one is named (2026-10-29: Dana's empty thread
// was listed on Priya's computer as begun by Priya).
export function beganByOf(actorId, people) {
	if ((actorId ?? "") == "") return viewerId;
	const person = (people ?? []).find(entry => entry.actorId == actorId);
	if (person?.isYou == true) return viewerId;
	return personIdForActor(actorId, person?.name ?? "Another member");
}

// A person in the office their record states: their position, by which the
// team orders them (D-059), and their practice; their face as they show it to
// the firm (D-163). Never a bare "Member" where their record says more.
function placeOffice(member, person) {
	if (person.position) {
		member.position = person.position;
		member.role = person.position;
	}
	if (person.practice) member.practice = person.practice;
	// Their own words for their position, from the firm's record (D-209).
	if (person.title !== undefined) member.title = person.title || null;
	// What they would have the firm know of them, from the same record.
	if (person.about !== undefined) member.about = person.about || null;
	// Their own photo, never a prototype portrait's sizes over it.
	if (person.photo !== undefined) {
		member.photo = person.photo || null;
		member.photoSet = null;
	}
}

// The open Matter's threads in the window's lists, each at its own address,
// and its people and AI colleagues as members of the firm.
function placeThreads(matterId, data) {
	const uiMatterId = uiMatterIdOf(matterId);
	for (let index = threads.length - 1; index >= 0; --index) {
		if (threads[index].isReal == true) threads.splice(index, 1);
	}
	for (let index = inbox.length - 1; index >= 0; --index) {
		if (inbox[index].isReal == true) inbox.splice(index, 1);
	}
	const colleagueIds = (data.agents ?? []).map(colleagueMember);
	// Each person in the office they state on the Matter's record: "Senior
	// Associate", never a bare "Member" where their record says more.
	const peopleIds = (data.people ?? []).filter(person => person.isYou != true).map(person => {
		const id = firmPersonMember(person);
		const member = members.find(item => item.id == id);
		if (member != null && member.isReal == true && id != viewerId) placeOffice(member, person);
		return id;
	});
	data.chats.forEach(chat => {
		const conversation = data.conversations[chat.id];
		if (conversation == null) return;
		const posts = withSending(chat.id, postsOf(conversation, data.agents, data.documents));
		// Who takes part: the lawyer, whoever wrote in it, and the colleagues it named.
		const named = posts.flatMap(post => post.mentionIds ?? [])
			.map(id => data.agents.find(agent => agent.id == id))
			.filter(Boolean)
			.map(agent => memberIdFor(agent.name, true, false));
		const participantIds = [...new Set([viewerId, ...posts.map(post => post.authorId ?? post.byId).filter(Boolean), ...named])];
		const thread = {
			id: `real:${chat.id}`,
			matterId: uiMatterId,
			title: conversation.chatTitle || chat.title,
			createdAt: chat.createdAt,
			beganBy: beganByOf(chat.createdByActorId, data.people),
			participantIds,
			posts,
			isArchived: chat.archived == true,
			isReal: true,
			real: { matterId, chatId: chat.id },
		};
		threads.unshift(thread);
		inbox.unshift({ threadId: thread.id, isUnread: false, isReal: true });
	});
	const matter = matters.find(item => item.id == uiMatterId);
	if (matter != null) {
		matter.memberIds = [...new Set([viewerId, ...peopleIds, ...colleagueIds])];
		// Those it was given to whose computer has not opened it yet.
		matter.invitedIds = (data.people ?? []).filter(person => person.invited == true).map(person => personIdForActor(person.actorId, person.name));
	}
}

// The chat of the thread the member has open. When the Matter changes it is
// read first and shown, and the rest of the Matter follows: a new message in
// the open thread shows in a fraction of a second (2026-10-26: reading
// Amsterdam's 21 threads took 2.6 seconds, one at a time in the backend, and a
// message from the Mac stood 6.5 seconds on this computer's disk before the
// window showed it).
let openChatId = null;
let isReading = false;
let isStale = false;
let isReadingOpen = false;
let isOpenStale = false;
// When each thread's words now shown were asked for: the open thread is read
// beside the whole Matter, and a read asked for earlier never replaces one
// asked for later.
const askedAt = new Map();

export function watchRealChat(chatId) {
	openChatId = chatId ?? null;
}

function newestConversations(conversations, asked) {
	const kept = {};
	for (const [chatId, conversation] of Object.entries(conversations)) {
		const shown = snapshot.conversations?.[chatId];
		if (shown != null && (asked[chatId] ?? 0) < (askedAt.get(chatId) ?? 0)) {
			kept[chatId] = shown;
			continue;
		}
		kept[chatId] = conversation;
		if (asked[chatId] != null) askedAt.set(chatId, asked[chatId]);
	}
	return kept;
}

async function readOpenThread() {
	if (isReadingOpen == true) {
		isOpenStale = true;
		return;
	}
	isReadingOpen = true;
	try {
		do {
			isOpenStale = false;
			const matterId = snapshot.matterId;
			const chatId = openChatId;
			if (matterId == null || chatId == null || snapshot.conversations == null) break;
			const api = nativeApp();
			const asked = performance.now();
			const [chatList, conversation] = await Promise.all([api.listProjectChats(matterId), api.listChatConversation(matterId, chatId)]);
			if (snapshot.matterId != matterId) break;
			const data = {
				chats: threadChatsOf(chatList, snapshot.agents),
				agents: snapshot.agents ?? [],
				documents: snapshot.documents ?? [],
				conversations: newestConversations({ ...snapshot.conversations, [chatId]: conversation }, { [chatId]: asked }),
			};
			placeThreads(matterId, data);
			publish({ status: "connected", ...data, error: null });
		} while (isOpenStale == true);
	} catch {
		// The whole read that follows says what failed.
	}
	isReadingOpen = false;
}

// The whole read under way: one asking for a read while it runs waits for it
// to end, the Matter now open read last - so a Matter just opened is read
// before its thread is looked for (2026-11-02: Recent's thread was "no longer
// here" while a read begun earlier was still running).
let reading = null;

function reload() {
	if (snapshot.matterId == null) return Promise.resolve();
	// One whole read at a time; a change during it reads again once it ends.
	if (isReading == true) {
		isStale = true;
		return reading ?? Promise.resolve();
	}
	reading = readWhole().finally(() => {
		reading = null;
	});
	return reading;
}

async function readWhole() {
	isReading = true;
	try {
		do {
			isStale = false;
			const matterId = snapshot.matterId;
			const { asked, ...data } = await readAll(matterId);
			// The member opened another Matter meanwhile: that one is read instead.
			if (snapshot.matterId != matterId) {
				isStale = true;
				continue;
			}
			data.conversations = newestConversations(data.conversations, asked);
			placeThreads(matterId, data);
			publish({ status: "connected", ...data, error: null });
		} while (isStale == true);
	} catch (error) {
		publish({ status: "connected", error: error?.message ?? String(error) });
	}
	isReading = false;
}

function scheduleReload() {
	if (reloadTimer != null) clearTimeout(reloadTimer);
	reloadTimer = setTimeout(() => {
		reloadTimer = null;
		readOpenThread();
		reload();
		readFirm();
	}, 16);
}

// What the backend says changed, heard once for as long as the window runs.
function subscribeToChanges() {
	if (isSubscribed == true) return;
	isSubscribed = true;
	// Any Matter's change may change what it says last.
	nativeApp().on("matter.changed", scheduleSummaries);
	// The firm's record changed - a colleague's settings, its memory, what the
	// member read on another computer (D-203), who sits in a seat (D-209).
	nativeApp().on("matter.changed", payload => {
		if ((payload?.projectId ?? "").startsWith("pc_firm_") == false) return;
		readFirm();
		readReadMarks();
	});
	window.addEventListener("focus", scheduleSummaries);
	nativeApp().on("matter.changed", payload => {
		if (payload?.projectId == null || payload.projectId == snapshot.matterId) scheduleReload();
		// A colleague's personal chat changed on the member's desk.
		if (payload?.projectId != null && (snapshot.firm?.agents ?? []).some(agent => agent.chatProjectId == payload.projectId)) readFirm();
	});
	nativeApp().on("agent.changed", payload => {
		if (payload?.matterId == null || payload.matterId == snapshot.matterId) scheduleReload();
		readFirm();
	});
	// A personal conversation changed: the other member's words came over the
	// firm's exchange ("pc_"), or this member wrote in it.
	nativeApp().on("matter.changed", payload => {
		const id = payload?.projectId ?? "";
		if (id.startsWith("pc_") || Object.values(snapshot.personal ?? {}).some(entry => entry.projectId == id)) readPersonal();
	});
	// Not every change of a thread is announced yet (a thread made elsewhere is
	// not): the window coming back reads it again.
	window.addEventListener("focus", scheduleReload);
}

// Reads who is at this computer, the firm, its Matters and the one the member
// has open; listens for what changes them. Safe to call more than once.
export async function connectRealWork() {
	if (hasNative() == false) {
		publish({ status: "off" });
		return;
	}
	// Who is at this computer, as the backend knows them: the firm's member of
	// that name, or a new one - never the prototype's Evelyn on someone else's
	// computer, and that before any Matter of the firm is open here (the Mac's
	// first window greeted Gabriel as Evelyn, 2026-10-25). The firm is theirs:
	// its own name, never Marlowe Finch's.
	try {
		const [organisationView, account] = await Promise.all([
			nativeApp().getOrganisation(),
			typeof nativeApp().getAccountState == "function" ? nativeApp().getAccountState().catch(() => null) : Promise.resolve(null),
		]);
		const viewer = organisationView?.viewer;
		// Whose words are "yours" in what the Matters say last (D-200).
		viewerActorId = viewer?.memberId ?? "";
		becomeOrganisation((viewer?.organisationName || account?.organisationName || "").trim() || "Your firm");
		// Whether this member hires the firm's AI colleagues and connects its
		// model accounts: its administrative account does, a member does not.
		becomeViewerRights(viewer?.may);
		// A member signed in to a seat whose name has not come yet is that seat
		// until the firm's record names who sits in it (D-209): never the
		// prototype's Evelyn (the owner, 2026-11-02: a colleague's new computer
		// showed Evelyn Hart's profile before its first check).
		const name = (viewer?.name ?? "").trim() || (account?.signedIn == true ? (account?.seatId ?? "").trim() : "");
		if (name != "") {
			const id = memberIdFor(name, false, false);
			becomeViewer(id);
			if ((viewer?.name ?? "").trim() == "") {
				const standIn = members.find(item => item.id == id);
				if (standIn != null) standIn.isSeatStandIn = true;
			}
			// The member stands in the office they gave (organisation.setOwnOffice,
			// onboarding's first page), never as a bare "Member" (2026-10-29: a new
			// firm's partner showed so).
			const member = members.find(item => item.id == id);
			if (member != null && member.isReal == true) {
				if (viewer.position) member.position = viewer.position;
				if (viewer.practice) member.practice = viewer.practice;
				// And with the photo they chose (D-163).
				member.photo = viewer.photo || null;
				member.photoSet = null;
			}
		}
	} catch {
		// The firm keeps the name it had.
	}
	// The firm's colleagues think with the connected accounts' default models
	// for any role not set yet (D-138), also for accounts connected before.
	ensureDefaultModels(nativeApp()).catch(() => {});
	// Its AI colleagues, with or without a Matter open (D-157).
	await readFirm();
	// The member's personal conversations with people (D-042).
	readPersonal();
	// What the backend says changed is heard before any Matter is read: a
	// colleague's new computer has no Matter, and maybe no workspace, when the
	// firm's record comes to it (the PC's walk, 2026-11-02: the administrator's
	// profile came a moment after the app opened, and Team never showed him).
	subscribeToChanges();
	try {
		const list = await readMatters();
		// What each Matter says last, under its name in the sidebar (D-200), and
		// what the member has read (D-202).
		readMatterSummaries();
		readReadMarks();
		const wanted = snapshot.matterId != null && list.some(matter => matter.id == snapshot.matterId) ? snapshot.matterId : preferredMatterId(list);
		if (wanted == null) {
			// No Matter here yet: the firm's computers may bring one (D-123), or
			// the member opens the first.
			publish({ status: (snapshot.arrivals ?? []).length > 0 ? "discoverable" : "absent" });
			return;
		}
		const matter = matters.find(item => item.realMatterId == wanted);
		snapshot = { ...snapshot, matterId: wanted, matterName: matter?.name ?? "", uiMatterId: uiMatterIdOf(wanted) };
		await reload();
	} catch (error) {
		publish({ status: "failed", error: error?.message ?? String(error) });
	}
}

// Reads the firm's Matter the member opens, in place of the one read before.
export async function bindRealMatter(realMatterId) {
	if (hasNative() == false || realMatterId == null) return;
	try {
		window.localStorage.setItem(OPEN_MATTER_KEY, realMatterId);
	} catch {
		// The window opens on the latest Matter next time.
	}
	if (snapshot.matterId == realMatterId) return;
	const matter = matters.find(item => item.realMatterId == realMatterId);
	for (let index = threads.length - 1; index >= 0; --index) {
		if (threads[index].isReal == true) threads.splice(index, 1);
	}
	snapshot = {
		...snapshot,
		matterId: realMatterId,
		matterName: matter?.name ?? "",
		uiMatterId: uiMatterIdOf(realMatterId),
		chats: [],
		agents: [],
		documents: [],
		reviews: [],
		letters: [],
		assignments: [],
		people: [],
		conversations: {},
	};
	publish({ status: "connecting" });
	await reload();
}

// A new Matter of the firm, as a lawyer opens one: its name, its client and
// what it is. It is open here at once, and the window reads it.
// A new Matter of the firm (D-211): in the folder the lawyer chose for it (its
// token from chooseNewMatterFolder), else a new folder named after it; then
// given to each lawyer chosen (D-205). Answers the window's id for it, and
// whether each chosen lawyer's computer is reached now.
export async function createFirmMatter({ name, client, about, folderToken = null, shareWith = [], look = null }) {
	const api = nativeApp();
	const made = await api.createNamedProject(folderToken != null ? { name: name.trim(), folderToken } : { name: name.trim() });
	const what = (about ?? "").trim().replace(/[.\s]+$/, "");
	const forClient = (client ?? "").trim();
	const description = forClient != "" ? `For ${forClient}: ${what || "its work"}.` : what != "" ? `${what}.` : "";
	if (description != "") await api.updateMatter(made.id, { description });
	const given = [];
	for (const actorId of shareWith) {
		try {
			given.push({ actorId, ...(await api.giveMatterTo(made.id, actorId)) });
			tellMatterGiven(made.id);
		} catch (error) {
			given.push({ actorId, given: false, problem: error?.message ?? "" });
		}
	}
	await readMatters();
	await bindRealMatter(made.id);
	// The mark chosen as it was opened, kept with it (D-200).
	if (look != null) await keepMatterMark(uiMatterIdOf(made.id), look).catch(() => {});
	return { uiId: uiMatterIdOf(made.id), realId: made.id, given };
}

// ---- Archived, deleted, changed (D-214) -------------------------------------------

function realMatterOf(uiMatterId) {
	const matter = matters.find(item => item.id == uiMatterId && item.isReal == true);
	if (matter == null) throw new Error("This Matter is not on this computer.");
	return matter;
}

// What a Matter is called and is for, as New Matter writes it: "For <client>:
// <what it is>.".
export function matterWordsOf(matter) {
	const description = (matter?.description ?? "").trim();
	const found = /^For ([^:]{1,120}): (.*)$/s.exec(description);
	const about = (found != null ? found[2] : description).trim().replace(/\.$/, "");
	return { name: matter?.name ?? "", client: found != null ? found[1].trim() : "", about: about == "its work" ? "" : about };
}

export async function editMatter(uiMatterId, { name, client, about }) {
	const matter = realMatterOf(uiMatterId);
	const what = (about ?? "").trim().replace(/[.\s]+$/, "");
	const forClient = (client ?? "").trim();
	const description = forClient != "" ? `For ${forClient}: ${what || "its work"}.` : what != "" ? `${what}.` : null;
	await nativeApp().updateMatter(matter.realMatterId, { name: name.trim(), description });
	await readMatters();
}

export async function setMatterArchived(uiMatterId, archived) {
	const matter = realMatterOf(uiMatterId);
	await nativeApp().updateMatter(matter.realMatterId, { archived });
	await readMatters();
}

// A letter the member takes out of the Matter (the owner, 2026-11-02: "delete
// these mock letters"): for everyone on it; one filed from the Matter's
// folder takes its email file to this computer's bin too, or the folder would
// file it again (D-180).
export async function removeLetter(uiMatterId, letter) {
	const matter = realMatterOf(uiMatterId);
	if (letter.filing?.from == "folder") {
		const place = (letter.filing?.evidence ?? []).map(line => /“(.+)”/.exec(line)?.[1]).find(Boolean) ?? null;
		const document = place == null ? null : realDocuments().find(item => (item.relativePath ?? "").replaceAll("\\", "/") == place.replaceAll("\\", "/"));
		if (document != null) await removeDocumentFromMatter(document.id);
	}
	await nativeApp().unfileLetter(matter.realMatterId, letter.id);
	await reload();
}

// The rules of a Matter's correspondence intake, kept as one list for every
// computer of it (D-228).
export async function setMatterIntake(uiMatterId, rules) {
	const matter = realMatterOf(uiMatterId);
	await nativeApp().updateMatter(matter.realMatterId, { correspondenceIntake: rules.map(rule => ({ mailbox: rule.mailbox, from: rule.from })) });
	await readMatters();
}

// The Matter's folder, shown in Finder, File Explorer or the files of Linux.
export function openMatterFolder(uiMatterId) {
	return nativeApp().openMatterFolder(realMatterOf(uiMatterId).realMatterId);
}

// The model usage of a Matter's whole life, as Usage counts it from the
// Matter's own record (usage.getSummary), whichever computer ran it.
export function readMatterLifeUsage(uiMatterId) {
	const matter = matters.find(item => item.id == uiMatterId);
	if (hasNative() == false || matter?.realMatterId == null || typeof nativeApp().getUsageSummary != "function") return Promise.resolve(null);
	const end = new Date(Date.now() + 24 * 60 * 60 * 1000).toISOString().replace(/\.\d{3}Z$/, "Z");
	return nativeApp().getUsageSummary("2000-01-01T00:00:00Z", end, matter.realMatterId);
}

// This computer's Trash of Matters (D-247; the owner, 2026-11-04: "Matters
// can be cleared away, they go to the Trash and are kept there for 30
// days"): one in it leaves every list here and comes back whole with Restore;
// after 30 days, or with Delete now, it is deleted from this computer as
// deleteMatterHere deletes it. Moving it returns its Undo.
export function matterTrash() {
	return snapshot.trash ?? [];
}

export async function moveMatterToTrash(uiMatterId) {
	const matter = realMatterOf(uiMatterId);
	await nativeApp().moveMatterToTrash(matter.realMatterId);
	await readMatters();
	return () => restoreMatterFromTrash(matter.realMatterId);
}

export async function restoreMatterFromTrash(realMatterId) {
	await nativeApp().restoreMatterFromTrash(realMatterId);
	await readMatters();
}

export async function deleteMatterFromTrash(realMatterId) {
	await nativeApp().deleteMatterFromTrash(realMatterId);
	await readMatters();
}

// Deleted from this computer: it stops travelling here and leaves the list;
// its folder and documents stay where they are, and the others on it keep it.
export async function deleteMatterHere(uiMatterId) {
	const matter = realMatterOf(uiMatterId);
	await nativeApp().deleteMatter(matter.realMatterId);
	await readMatters();
}

async function chatOf(thread) {
	const real = thread?.real;
	if (real == null) throw new Error("This thread is not on this computer.");
	const listed = await nativeApp().listProjectChats(real.matterId);
	const chat = (listed?.chats ?? []).find(item => item.id == real.chatId);
	if (chat == null) throw new Error("This thread is no longer here.");
	return { ...real, revision: chat.revision ?? 1 };
}

function threadOf(threadId) {
	const thread = threads.find(item => item.id == threadId && item.isReal == true);
	if (thread == null) throw new Error("This thread is not on this computer.");
	return thread;
}

export async function renameThread(threadId, title) {
	const chat = await chatOf(threadOf(threadId));
	await nativeApp().renameProjectChat(chat.matterId, chat.chatId, chat.revision, title.trim());
	await reload();
}

export async function setThreadArchived(threadId, archived) {
	const chat = await chatOf(threadOf(threadId));
	await nativeApp().setChatArchived(chat.matterId, chat.chatId, archived);
	await reload();
}

// Deleted, and brought back while its notice stands: its words are never erased.
export async function deleteThread(threadId) {
	const chat = await chatOf(threadOf(threadId));
	await nativeApp().deleteProjectChat(chat.matterId, chat.chatId, chat.revision);
	await reload();
	return async () => {
		await nativeApp().restoreChat(chat.matterId, chat.chatId);
		await reload();
	};
}

function colleagueOf(memberId) {
	const member = members.find(item => item.id == memberId);
	if (member?.agentId == null) throw new Error("This colleague is not on this computer.");
	return member;
}

// The administrator's change of a colleague (D-214), from any of their
// computers (D-219): one this computer runs changes at once; one another
// computer runs is asked of it, and changes once that computer takes it.
// Whether it was only asked.
export async function editColleague(memberId, words) {
	const member = colleagueOf(memberId);
	const isAsked = member.isFirmCopy == true && typeof nativeApp().requestColleagueEdit == "function";
	if (isAsked) await nativeApp().requestColleagueEdit(member.agentId, words);
	else await nativeApp().editColleague(member.agentId, words);
	await readFirm();
	return { isAsked };
}

export async function setColleagueArchived(memberId, archived) {
	const member = colleagueOf(memberId);
	await nativeApp().archiveColleague(member.agentId, archived);
	await readFirm();
}

export async function deleteColleague(memberId) {
	const member = colleagueOf(memberId);
	await nativeApp().deleteColleague(member.agentId);
	const at = members.indexOf(member);
	if (at >= 0) members.splice(at, 1);
	await readFirm();
}

// A colleague's seat archived, restored with a new password, or removed.
export async function setPersonState(memberId, state, password = "") {
	const member = members.find(item => item.id == memberId);
	const seatId = (member?.actorId ?? "").startsWith("seat:") ? member.actorId.slice(5) : null;
	if (seatId == null) throw new Error("This colleague has no seat of the firm.");
	await nativeApp().setSeatState(seatId, state, password);
	await readFirm();
	return seatId;
}

// A Matter this computer took from the firm's computers - accepted when it was
// given, or asked for by an invitation (D-205) -, listed among the member's
// Matters once it has come whole: taking it was the member's choice, so it is
// never left to be found and opened a second time (the owner, 2026-11-02:
// "Matters doesn't show what came"). With no Matter open here yet, the window
// opens on it. Answers the window's id for it.
export async function listArrivedMatter(realMatterId, folder) {
	const known = matters.find(matter => matter.isReal == true && matter.realMatterId == realMatterId);
	if (known != null) return known.id;
	const opened = await nativeApp().openDiscoveredMatter(folder);
	const id = opened?.id ?? realMatterId;
	// Read once, its folder is watched from now as any Matter open here: its
	// documents stay current, and the firm's computers hear that this member
	// has it (its member record, SharedMatterHooks.opened).
	await nativeApp().listMatterDocuments(id).catch(() => null);
	await readMatters();
	if (["absent", "discoverable", "failed"].includes(snapshot.status)) await connectRealWork();
	return uiMatterIdOf(id);
}

// The Matters given from this window to a lawyer of the firm, told to whoever
// listens: the window's coordination says each one's first transfer (D-207).
const givenListeners = new Set();

export function onMatterGiven(listener) {
	givenListeners.add(listener);
	return () => givenListeners.delete(listener);
}

export function tellMatterGiven(realMatterId) {
	for (const listener of givenListeners) listener(realMatterId);
}

// Opens here a Matter the firm's computers brought (D-123), and reads it.
export async function openArrivedMatter(arrival) {
	const opened = await nativeApp().openDiscoveredMatter(arrival.path);
	await readMatters();
	const id = opened?.id ?? arrival.id;
	await bindRealMatter(id);
	return uiMatterIdOf(id);
}

// A new AI colleague of the firm (D-157): always the whole firm's, never hired
// into a Matter. It holds the office and the face it was hired with, is
// named in any Matter's thread to take work there, and its personal chat with
// the member opens on their desk at once.
export async function hireColleague({ name, species, title, ranksWith, practice, area, answersTo, goal, manner }) {
	const made = await nativeApp().hireColleague({
		name: (name ?? "").trim(),
		...(SPECIES.includes(species) ? { character: species } : {}),
		title: title ?? "",
		ranksWith: ranksWith ?? "",
		practice: practice ?? "",
		area: area ?? "",
		answersTo: answersTo ?? "",
		goal: goal ?? "",
		manner: (manner ?? "").slice(0, 400),
	});
	await readFirm();
	return colleagueMember(made);
}

// What a colleague remembers, as its page shows it, and a note a member lets
// go of (D-159).
export async function colleagueMemory(agentId) {
	const api = nativeApp();
	if (typeof api?.getColleagueMemory != "function") return [];
	return (await api.getColleagueMemory(agentId))?.notes ?? [];
}

export async function forgetColleagueNote(agentId, noteId) {
	return (await nativeApp().forgetColleagueMemory(agentId, noteId))?.notes ?? [];
}

// ---- What the lists show ----------------------------------------------------------

// In Singularity the lists hold the firm's own work only: its real Matter,
// that Matter's threads and the chats begun here (the owner, 2026-10-24:
// "start Singularity from zero, with one project and one thread"). The
// prototype's sample Matters, threads, chats and mail are for a plain
// browser, where they are all there is. They stay in the data, so a link
// that names one still opens it.
export function shownMatters() {
	return hasNative() ? matters.filter(matter => matter.isReal == true) : matters;
}

// The firm's members: in the app its own - the member at this computer, the
// people of its Matters and its AI colleagues - and the prototype's in a
// plain browser.
export function shownMembers() {
	return hasNative() ? members.filter(member => member.isReal == true || member.id == viewerId) : members;
}

// The firm's team: the member here, the people the firm's record names and
// its AI colleagues - never a name met only in a message (the owner,
// 2026-11-02: a renamed colleague stood twice).
export function teamMembers() {
	if (hasNative() == false) return members;
	return shownMembers().filter(member => member.id == viewerId
		|| (member.kind == "ai_colleague" ? member.isFirmColleague == true : member.isFirmPerson == true));
}

// A thread begun here with nothing written in it yet - only the name the
// window gave it - is listed nowhere but where it is open: left empty, it
// holds no conversation (the walkthrough of 2026-10-29: an empty "New thread"
// stayed in every list). It is kept, and its first message names and lists it.
function isUnbegun(thread) {
	return thread.isReal == true && (thread.posts ?? []).length == 0 && ["New thread", "New chat"].includes(thread.title)
		&& thread.real?.chatId != openChatId;
}

// A thread stands in the lists while its Matter is listed here: the firm's
// own General, or a Matter neither in this computer's Trash nor deleted from
// it. What is in the Trash leaves every list here (D-247), its threads with
// it, and they come back with it (2026-11-05: the Inbox could not show a row
// of a Matter deleted a moment before).
function isListedMatter(uiMatterId) {
	return uiMatterId == general.id || matters.some(matter => matter.id == uiMatterId && matter.isReal == true);
}

function isListed(thread) {
	return thread.isReal == true && isUnbegun(thread) == false && thread.isArchived != true && isListedMatter(thread.matterId);
}

export function shownThreads() {
	return hasNative() ? threads.filter(isListed) : threads;
}

// A Matter's archived threads (D-214), the latest first: readable, and
// restored from its list.
export function archivedThreads(uiMatterId) {
	return threads.filter(thread => thread.isReal == true && thread.isArchived == true && thread.matterId == uiMatterId);
}

export function shownInbox() {
	if (hasNative() == false) return inbox;
	return inbox.filter(entry => threads.some(thread => thread.id == entry.threadId && isListed(thread)));
}

// The member's personal chats. The firm's own are not kept yet, so here they
// are the ones begun in this session.
export function shownChats(state) {
	if (hasNative() == false) return chats;
	return Object.entries(state?.extraMessages ?? {})
		.filter(([, messages]) => messages.length > 0)
		.map(([memberId]) => ({ memberId, isUnread: false, messages: [] }));
}

export function shownMail() {
	return hasNative() ? [] : projectMail;
}

// The time, AI work and disbursements billing counts. The prototype's are its
// sample Matters' work, so here only the time recorded in this session counts.
export function shownBillables() {
	if (hasNative() == false) return { timeEntries, aiWork, aiUsage, expenses };
	return { timeEntries: timeEntries.filter(entry => entry.isRecorded == true), aiWork: [], aiUsage: [], expenses: [] };
}

// The calendar's own events, before any the member adds in it.
export function shownCalendarEvents() {
	// In the app none of the prototype's: its member's own meetings were Marlowe
	// Finch's too (2026-10-29), and the colleagues' working time comes from the
	// backend (realColleagueEvents).
	return hasNative() ? [] : calendarEvents;
}

// Whether a Matter of the prototype stands for the real one here.
export function isRealMatter(matterId) {
	return hasNative() && typeof matterId == "string" && matterId.startsWith(REAL_MATTER_PREFIX);
}

// The real Matter's documents, as its page lists them: each file, its size
// and when it came, and whether it can be read. A colleague's draft says
// whose it is and which version.
// A version its colleague replaced in the same work, before anyone decided on
// it (D-168): an earlier pass of the work, never handed in - its document and
// its work product are not listed, the version handed in is.
function isReplacedInWork(review, version) {
	return version?.state == "superseded" && decisionOf(review, version) == null;
}

export function realMatterDocuments() {
	return realDocuments()
		.filter(document => {
			const draft = reviewOfDocument(document.id);
			return draft == null || isReplacedInWork(draft.review, draft.version) == false;
		})
		.map(document => {
			const draft = reviewOfDocument(document.id);
			const state = document.processingState ?? "ready";
			return {
				name: document.displayName ?? document.name,
				// Where it stands in the Matter's folder, by its folders: "01 Corporate/01.02 Bylaws.pdf".
				path: (document.relativePath || document.displayName || document.name || "").replace(/\\/g, "/"),
				documentId: document.id,
				// The seat whose computer first recorded it in the Matter (its operations' own record).
				addedByActorId: document.addedByActorId || "",
				// A picture with no words in it is still a picture the AI model sees
				// (D-256): only a document that cannot be read says so.
				kind: ["processing", "changed"].includes(state) ? "reading"
					: ["needs_attention", "unsupported"].includes(state) ? (isPictureFile(document.displayName ?? document.name) ? "ready" : "failed")
					: state == "unavailable" ? "gone"
					: draft != null ? "draft" : "ready",
				sizeBytes: document.sizeBytes ?? null,
				added: document.addedAt || null,
				// Why a document that is not ready here is not, in the backend's words.
				detail: document.attentionDetail ?? null,
				draftOf: draft != null ? { agentName: draft.review.agentName, version: draft.version.number } : null,
			};
		})
		.sort((first, second) => (second.added ?? "").localeCompare(first.added ?? ""));
}

// The drafts the Matter's colleagues handed in, the latest first: each
// version of a review that has one, with what it waits for.
export function realWorkProducts() {
	const products = [];
	// A draft is listed only while its document is in the Matter: one the
	// lawyer deleted is not handed in any more, as a written document is not.
	const present = new Set(realDocuments().map(document => document.id));
	for (const review of snapshot.reviews ?? []) {
		for (const version of review.versions ?? []) {
			if ((version.resultDocumentId ?? "") == "" || isReplacedInWork(review, version) || present.has(version.resultDocumentId) == false) continue;
			products.push({
				id: `${review.id}:${version.number}`,
				version: version.number,
				title: (review.sourceFileName || "The agreement").replace(/\.[^.]+$/, ""),
				authorId: colleagueIdFor(review.agentId, review.agentName),
				delivered: version.execution?.finishedAt || version.createdAt,
				status: versionStatus(review, version),
				documentId: version.resultDocumentId,
			});
		}
	}
	// The documents a colleague wrote as its work (D-141): an issues list, a
	// memo, a summary - a work product of their own, with no original behind it.
	for (const assignment of snapshot.assignments ?? []) {
		for (const step of assignment.steps ?? []) {
			if ((step.reviewId ?? "") != "" || step.status != "completed" || present.has(step.documentId) == false) continue;
			products.push({
				id: `${assignment.id}:${step.number}`,
				version: 1,
				title: step.label || (step.sourceName ?? "").replace(/\.[^.]+$/, ""),
				authorId: colleagueIdFor(assignment.agentId, assignment.agentName),
				delivered: assignment.finishedAt || assignment.startedAt,
				status: writtenStatus(step),
				documentId: step.documentId,
			});
		}
	}
	return products.sort((first, second) => (second.delivered ?? "").localeCompare(first.delivered ?? ""));
}

// What a real colleague is doing, as the pages say it of any colleague: at
// work, or asking the member to accept a draft; null for nothing.
export function realColleagueState(memberId) {
	if (hasNative() == false) return undefined;
	const agent = colleagues().find(entry => memberIdFor(entry.name, true, false) == memberId);
	if (agent == null) return undefined;
	const reviews = (snapshot.reviews ?? []).filter(review => isReviewOf(review, agent))
		.sort((first, second) => (second.updatedAt ?? "").localeCompare(first.updatedAt ?? ""));
	const name = review => (review.sourceFileName || "the agreement").replace(/\.[^.]+$/, "");
	const underway = reviews.find(review => isUnderway(review.versions?.at(-1)));
	if (isAtWork(agent)) return { text: underway != null ? `Works on ${name(underway)}` : "At work", kind: "working", presence: "work" };
	const waiting = reviews.find(review => review.versions?.at(-1)?.state == "ready_for_review");
	if (waiting != null) return { text: `Asks you to accept ${name(waiting)}, version ${waiting.versions.at(-1).number}`, kind: "yours", presence: "explain" };
	return null;
}

// ---- What a page asks of it -----------------------------------------------------

export function reviewsOfChat(chatId) {
	return (snapshot.reviews ?? []).filter(review => review.chatId == chatId);
}

// The firm's member for an AI colleague the backend names.
export function colleagueMemberId(name) {
	return memberIdFor(name, true, false);
}

export function reviewById(reviewId) {
	return (snapshot.reviews ?? []).find(review => review.id == reviewId) ?? null;
}

// The review a thread is about now: its latest.
export function latestReviewOfChat(chatId) {
	const reviews = reviewsOfChat(chatId);
	return reviews.sort((first, second) => (second.updatedAt ?? "").localeCompare(first.updatedAt ?? ""))[0] ?? null;
}

// The review version whose draft a document is, or null.
export function reviewOfDocument(documentId) {
	for (const review of snapshot.reviews ?? []) {
		const version = review.versions.find(entry => entry.resultDocumentId == documentId);
		if (version != null) return { review, version };
	}
	return null;
}

// The prototype's thread a backend thread stands as.
export function threadOfChat(chatId) {
	return threads.find(thread => thread.real?.chatId == chatId) ?? null;
}

// A colleague's message and the version of a review it hands in.
export function versionOfPost(post) {
	const review = post.reviewId != null ? reviewById(post.reviewId) : null;
	if (review == null) return null;
	const version = review.versions.find(entry => entry.answerMessageId == post.id) ?? null;
	return { review, version };
}

// The assignment whose card stands under this message (D-120): under the
// colleague's reply that took the work up - each result as it is handed in -
// or, for an assignment from before there was one, under its last word.
// The work whose results stand under this message: under the one in which the
// colleague says it has finished, once it has (the owner, 2026-11-02: "she
// must attach the file to the new message"; D-239); until then under the one
// in which it took the work up.
export function assignmentOfPost(post) {
	return (snapshot.assignments ?? []).find(assignment => (assignment.summaryMessageId ?? "") != ""
		? assignment.summaryMessageId == post.id
		: (assignment.ackMessageId ?? "") != "" && assignment.ackMessageId == post.id) ?? null;
}

// The decisions that stand under this message: those the colleague put to the
// lawyer in it, each with its assignment (D-120).
export function decisionsOfPost(post) {
	return decisionsUnder(snapshot.assignments, post.id);
}

export function decisionsUnder(assignments, messageId) {
	const found = [];
	if ((messageId ?? "") == "") return found;
	for (const assignment of assignments ?? []) {
		for (const decision of assignment.decisions ?? []) {
			// A decision a later version replaced stays in the record, never under
			// the message (D-174).
			if (decision.messageId == messageId && decision.state != "withdrawn") found.push({ assignment, decision });
		}
	}
	return found;
}

// The lawyer's answer to a decision: recorded once, with who gave it, and said
// to the colleague in the thread, which takes the work up with it (D-120).
export async function answerAssignmentDecision(assignmentId, decisionId, answer) {
	const answered = await nativeApp().answerAssignmentDecision(snapshot.matterId, assignmentId, decisionId, answer);
	scheduleReload();
	return answered;
}

// Who decided a version, when, and on what: the lawyer's decision as kept.
export function decisionOf(review, version) {
	return [...(review?.decisions ?? [])].reverse().find(decision => decision.version == version?.number) ?? null;
}

// Whether a version is still being worked on.
export function isUnderway(version) {
	return ["queued", "preparing", "running", "producing"].includes(version?.state);
}

// The one status a version shows, as components/Status draws it.
export function versionStatus(review, version) {
	const agent = review.agentName || "The colleague";
	switch (version.state) {
		case "queued": return { kind: "waiting", words: `Queued: ${agent} begins when its earlier work is done` };
		case "preparing": return { kind: "underway", words: `${agent} is reading ${review.sourceFileName || "the document"}` };
		case "running": return { kind: "underway", words: review.playbookName ? `${agent} is reviewing it against the ${review.playbookName}` : `${agent} is working on it` };
		case "producing": return { kind: "underway", words: `${agent} is checking its changes and writing the draft` };
		case "ready_for_review": return { kind: "yours", words: `Version ${version.number} · Ready for review` };
		case "accepted": return { kind: "settled", words: acceptedWords(decisionOf(review, version)) };
		case "returned": return { kind: "underway", words: `Returned: ${agent} is working on it` };
		case "failed": return { kind: "failed", words: "Not completed" };
		case "waiting_for_information": return { kind: "yours", words: "Asks you" };
		case "superseded": return { kind: "settled", words: version.stateMessage || "Replaced by a later version" };
		case "unchanged": return { kind: "settled", words: "No new draft: it would read as the last version" };
		default: return { kind: "waiting", words: "Open" };
	}
}

// What a real thread waits for: closed when a member closed it (D-088);
// otherwise what its latest review waits for. An accepted version closes
// nothing (D-091): the thread is open.
export function realThreadStatus(thread) {
	const events = (thread?.posts ?? []).filter(post => post.kind == "closed" || post.kind == "reopened");
	if (events.at(-1)?.kind == "closed") return { kind: "settled", words: "Closed with a conclusion" };
	if (asksViewer(thread?.posts, viewerId) == true) return { kind: "yours", words: "Asks you" };
	const review = thread?.real != null ? latestReviewOfChat(thread.real.chatId) : null;
	const version = review?.versions.at(-1);
	if (version == null || ["accepted", "superseded"].includes(version.state)) return { kind: "waiting", words: "Open" };
	return versionStatus(review, version);
}

export function realDocument(documentId) {
	return (snapshot.documents ?? []).find(document => document.id == documentId) ?? null;
}

// The AI colleague a name in a message means: "@Unicorn" names Unicorn.
export function agentNamed(name) {
	const words = (name ?? "").trim().toLowerCase();
	return colleagues().find(agent => (agent.name ?? "").trim().toLowerCase() == words) ?? null;
}

// The actor ids of the Matter's people a message names with @ and their first
// name ("@Gabriel" names Gabriel Moore), never the member who writes it nor a
// name an AI colleague goes by. Each finds the message in their own Inbox as
// asking them, on whichever computer they work (ORG-14; the owner, 2026-10-26:
// people are tagged with @ as colleagues are).
export function peopleNamed(text, people, agents = []) {
	const found = [];
	for (const match of (text ?? "").matchAll(/@([A-Za-z][A-Za-z-]*)/g)) {
		const word = match[1].toLowerCase();
		if (agents.some(agent => (agent.name ?? "").trim().toLowerCase() == word)) continue;
		for (const person of people ?? []) {
			const first = ((person.name ?? "").trim().split(/\s+/)[0] ?? "").toLowerCase();
			if (person.isYou != true && first == word && person.actorId && found.includes(person.actorId) == false) found.push(person.actorId);
		}
	}
	return found;
}

// Whether a thread's posts still ask the member: a post named them with @, and
// they have not written in the thread since.
export function asksViewer(posts, memberId) {
	for (let index = (posts ?? []).length - 1; index >= 0; --index) {
		const post = posts[index];
		if (post.authorId == memberId && post.kind == null) return false;
		if (post.addressesViewer == true) return true;
	}
	return false;
}

// The member's own messages, shown in their thread the moment they are sent and
// kept there until a read of the Matter has the message itself (2026-10-26: on
// a Matter of 2,000 records one's own reply took four seconds to show).
const sending = new Map();
let sendingCount = 0;

function withSending(chatId, posts) {
	const shown = posts.filter(post => post.isSending != true);
	for (const [key, entry] of sending) {
		if (entry.chatId != chatId) continue;
		if (entry.messageId != null && shown.some(post => post.id == entry.messageId)) {
			sending.delete(key);
			continue;
		}
		shown.push(entry.post);
	}
	return shown;
}

function showSending(chatId) {
	// A thread's posts are kept on it; a personal chat's are read from the
	// sending ones when Messages draws it.
	const thread = threads.find(entry => entry.real?.chatId == chatId);
	if (thread != null) thread.posts = withSending(chatId, thread.posts);
	publish({});
}

// The correspondents of the letters filed to the Matter (D-106), one
// conversation each: the other party of every letter, by address, its letters
// oldest first. A letter the member sent is their own.
export function letterConversations(letters) {
	const byAddress = new Map();
	for (const letter of letters ?? []) {
		const isOwn = letter.direction == "outgoing";
		const party = isOwn ? (letter.to ?? [])[0] : letter.from;
		const address = (party?.address ?? "").trim().toLowerCase();
		if (address == "") continue;
		const name = (party?.name ?? "").trim();
		const entry = byAddress.get(address) ?? { address, name: "", letters: [] };
		if (entry.name == "" && name != "" && name.toLowerCase() != address) entry.name = name;
		entry.letters.push(letter);
		byAddress.set(address, entry);
	}
	return [...byAddress.values()].map(entry => ({
		key: `mail:${entry.address}`,
		kind: "mail",
		contact: { name: entry.name || entry.address, address: entry.address },
		items: entry.letters
			.map(letter => ({
				id: letter.id,
				at: letter.sentAt || letter.updatedAt || "",
				isOwn: letter.direction == "outgoing",
				subject: (letter.subject ?? "").trim(),
				paragraphs: paragraphsOf((letter.text ?? "").replace(/\r\n/g, "\n")),
				attachments: (letter.attachments ?? []).map(attachment => ({ name: attachment.name, size: attachment.size ?? null })),
				letterId: letter.id,
			}))
			.sort((first, second) => first.at.localeCompare(second.at)),
	}));
}

// Messages in the real app (D-042): one conversation with each AI colleague
// this computer runs - its own chat, every message addressed to it - one with
// each person of the Matter, and one with each correspondent whose letters
// were filed to it (D-106).
// The firm's people and the open Matter's, each with their personal chat with
// the member (D-042): what was written in it, or a first message that begins
// it. `personal` holds the conversations readPersonal read, by the other
// member's actor id.
export function personConversations(everyone, personal, agents) {
	const people = new Map();
	for (const person of everyone) {
		if (person.isYou == true || (person.actorId ?? "") == "" || (person.name ?? "").trim() == "") continue;
		if (people.has(person.actorId) == false) people.set(person.actorId, person);
	}
	const found = [];
	for (const person of people.values()) {
		const memberId = personIdForActor(person.actorId, person.name);
		if (found.some(entry => entry.memberId == memberId)) continue;
		const chat = personal[person.actorId];
		const posts = chat != null ? withSending(chat.chatId, postsOf(chat.conversation, agents, chat.documents ?? [])) : [];
		found.push({
			key: `member:${memberId}`,
			kind: "member",
			memberId,
			actorId: person.actorId,
			items: posts.filter(post => post.kind == null).map(post => ({ ...post, isOwn: post.authorId == viewerId })),
			send: chat != null
				? { kind: "chat", chatId: chat.chatId, projectId: chat.projectId, mentions: [], isPersonal: true, reaches: chat.reaches == true }
				: { kind: "personal", actorId: person.actorId },
		});
	}
	return found;
}

export function realConversations() {
	const found = [];
	const everyColleague = colleagues();
	for (const agent of everyColleague.filter(agent => agent.isHere == true && (agent.chatId ?? "") != "")) {
		const memberId = memberIdFor(agent.name, true, false);
		// One conversation for each colleague the window shows (2026-11-02: an
		// archived Pasha beside the one at work showed as a second Pasha).
		if (found.some(entry => entry.memberId == memberId)) continue;
		const conversation = snapshot.firm?.conversations?.[agent.chatId] ?? snapshot.conversations?.[agent.chatId];
		const documents = snapshot.firm?.documents?.[agent.chatProjectId] ?? snapshot.documents ?? [];
		const clearedBefore = snapshot.cleared?.[agent.chatId] ?? "";
		const posts = withSending(agent.chatId, conversation != null ? postsOf(conversation, everyColleague, documents) : [])
			.filter(post => clearedBefore == "" || (post.at ?? "") == "" || post.at > clearedBefore);
		found.push({
			key: `member:${memberId}`,
			kind: "member",
			memberId,
			items: posts.filter(post => post.kind == null).map(post => ({ ...post, isOwn: post.authorId == viewerId })),
			// Its personal chat is on the member's desk (D-157), or, for a colleague
			// of the zero state, in the Matter it was made for.
			send: { kind: "chat", chatId: agent.chatId, projectId: agent.chatProjectId ?? snapshot.matterId, mentions: [agent.id] },
			// It writes its answer in this conversation now, as the backend says
			// (its run's stage, D-129): Messages says "typing".
			isAnswering: (agent.answeringChatId ?? "") != "" && agent.answeringChatId == agent.chatId,
		});
	}
	for (const entry of personConversations([...(snapshot.firm?.people ?? []), ...(snapshot.people ?? [])], snapshot.personal ?? {},
		everyColleague)) {
		if (found.some(known => known.memberId == entry.memberId)) continue;
		found.push(entry);
	}
	const nameOf = memberId => members.find(member => member.id == memberId)?.name ?? "";
	for (const entry of foldMembersLetters(found, letterConversations(snapshot.letters), nameOf)) {
		const last = entry.items.at(-1);
		found.push({ ...entry, send: { kind: "mail", letterId: last?.letterId ?? null, subject: last?.subject ?? "", to: entry.contact.address,
			matterId: snapshot.matterId } });
	}
	return found;
}

// A letter from or to a person of the firm stands in that person's own
// conversation, among what the two wrote each other: one row for one person
// (2026-10-31: Dana stood twice in Marcus's Messages, as the firm's member and
// as the sender of a letter filed to Project Frost). Matched by name, as the
// letter names them. The correspondents who are no member of the firm stay
// conversations of their own: returned.
export function foldMembersLetters(conversations, letterEntries, nameOf) {
	const simple = words => (words ?? "").trim().replace(/\s+/g, " ").toLowerCase();
	const left = [];
	for (const entry of letterEntries) {
		const person = conversations.find(known => known.kind == "member" && (known.actorId ?? "") != ""
			&& simple(nameOf(known.memberId)) != "" && simple(nameOf(known.memberId)) == simple(entry.contact.name));
		if (person == null) {
			left.push(entry);
			continue;
		}
		person.items = [...person.items, ...entry.items.map(item => ({ ...item, authorId: item.isOwn == true ? viewerId : person.memberId }))]
			.sort((first, second) => (first.at ?? "").localeCompare(second.at ?? ""));
	}
	return left;
}

// A reply to a letter filed to the Matter, as a draft Outlook opens to edit and
// send (D-079, D-105): Singularity writes it and opens it, never sends it.
// Files go with it as the Matter's documents, in an unsent email the mail app
// opens (D-115).
export async function draftLetterReply(letterId, text, attachmentDocumentIds = []) {
	const api = nativeApp();
	if (typeof api.draftLetterReply != "function") throw new Error("This version of Singularity cannot draft a reply yet.");
	return api.draftLetterReply(snapshot.matterId, letterId, text, attachmentDocumentIds);
}

// Everyone a thread's message can name: the Matter's people but the member at
// this computer, by actor id, and its AI colleagues, by agent id.
function everyoneHere() {
	return [
		...(snapshot.people ?? []).filter(person => person.isYou != true && person.actorId).map(person => person.actorId),
		...(snapshot.agents ?? []).map(agent => agent.id),
	];
}

export async function postToThread(chatId, text, extras = {}) {
	const named = [...text.matchAll(/@([A-Za-z][A-Za-z-]*)/g)].map(match => agentNamed(match[1])).filter(Boolean);
	// @all, where a place offers it, names everyone: those named by name come
	// first, then "all", then the rest, so a colleague knows whether it was
	// named or reached through all (the owner, 2026-10-26; the PC's talk keys
	// on it).
	const isAll = extras.allowsAll == true && namesAll(text);
	// A personal conversation stays between its two members (D-042): its words
	// name no one into it, and wake no colleague.
	const mentions = extras.isPersonal == true ? [] : [...new Set([...named.map(agent => agent.id),
		...peopleNamed(text, snapshot.people, snapshot.agents), ...(isAll ? ["all", ...everyoneHere()] : []), ...(extras.mentions ?? [])])];
	const key = `sending_${++sendingCount}`;
	sending.set(key, {
		chatId,
		messageId: null,
		// What it carries shows with it at once, as it will once kept (D-212).
		post: { id: key, authorId: viewerId, at: new Date().toISOString(), paragraphs: paragraphsOf(text), isReal: true, isSending: true, mentionIds: [],
			realFiles: (extras.files ?? []).length > 0 ? extras.files : null, quote: extras.quote ?? null },
	});
	showSending(chatId);
	const projectId = extras.projectId ?? projectOfChat(chatId);
	let answer;
	try {
		answer = await nativeApp().postChatMessage(projectId, chatId, text, mentions, {
			quoteMessageId: extras.quoteMessageId ?? null,
			attachmentDocumentIds: extras.attachmentDocumentIds ?? [],
			notChecked: extras.notChecked ?? null,
			askOptions: extras.askOptions ?? [],
			answerOption: extras.answerOption ?? null,
			place: extras.place ?? null,
		});
	} catch (error) {
		sending.delete(key);
		showSending(chatId);
		throw error;
	}
	const sent = sending.get(key);
	if (sent != null) sent.messageId = answer?.messageId ?? null;
	// A personal conversation is read from its own record, not the open Matter:
	// read at once, so the message stands as sent the moment it is kept (the
	// owner, 2026-11-02: a reply to a colleague stayed "Sending").
	if (extras.isPersonal == true) readPersonal();
	// A thread begun from here takes its name from its first message's words,
	// its formatting marks gone (a name is never drawn as markdown).
	const chat = (snapshot.chats ?? []).find(entry => entry.id == chatId);
	if (chat != null && ["New chat", "New thread"].includes(chat.title)) {
		const named = (extras.title ?? "").trim();
		const words = named != "" ? named : plainOf(text).replace(/@\S+/g, "").replace(/[\s,]+/g, " ").trim().split(" ").slice(0, 7).join(" ");
		if (words != "") {
			try {
				await nativeApp().renameProjectChat(projectId, chatId, chat.revision, words.charAt(0).toUpperCase() + words.slice(1));
			} catch {
				// The thread keeps its name; the message is posted either way.
			}
		}
	}
	scheduleReload();
	return answer;
}

// The same tools for every message (the owner, 2026-10-24): its author edits
// or deletes it, anyone reacts to it - as the backend keeps it.
export async function editRealPost(chatId, messageId, text) {
	const answer = await nativeApp().editChatMessage(snapshot.matterId, chatId, messageId, text);
	scheduleReload();
	return answer;
}

export async function deleteRealPost(chatId, messageId) {
	const answer = await nativeApp().deleteChatMessage(snapshot.matterId, chatId, messageId);
	scheduleReload();
	return answer;
}

function markDeleted(messageId, isDeleted) {
	for (const thread of threads) {
		if (thread.isReal != true) continue;
		const post = thread.posts.find(entry => entry.id == messageId);
		if (post != null) post.isDeleted = isDeleted;
	}
	publish({});
}

// A message deleted as the demo deletes one: it goes at once, and the backend
// deletes it when its Undo has gone. Returns the undo.
export function deleteRealPostWithUndo(chatId, messageId, onFailure) {
	markDeleted(messageId, true);
	const timer = setTimeout(() => {
		pendingDeletions.delete(messageId);
		deleteRealPost(chatId, messageId).catch(error => {
			markDeleted(messageId, false);
			onFailure?.(error);
		});
	}, UNDO_MILLISECONDS);
	pendingDeletions.set(messageId, timer);
	return () => {
		clearTimeout(pendingDeletions.get(messageId));
		pendingDeletions.delete(messageId);
		markDeleted(messageId, false);
	};
}

// A thread's closing with its conclusion, and its reopening (D-088).
export async function closeRealThread(chatId, conclusion) {
	const answer = await nativeApp().closeChatThread(snapshot.matterId, chatId, conclusion);
	scheduleReload();
	return answer;
}

export async function reopenRealThread(chatId) {
	const answer = await nativeApp().reopenChatThread(snapshot.matterId, chatId);
	scheduleReload();
	return answer;
}

export async function reactToRealPost(chatId, messageId, emoji) {
	const answer = await nativeApp().reactToChatMessage(snapshot.matterId, chatId, messageId, emoji);
	scheduleReload();
	return answer;
}

// ---- A personal conversation's own tools (D-212) ----------------------------
// Reactions, replies and files in a conversation of two members, as a
// messenger has them: its files are kept in the conversation's own folder, so
// they travel to the other member's computers sealed with it (D-123).

// The most one file can be: what the firm's computers carry in one envelope
// (MatterCoordination's kMostFileBytes); and the most files one message hands
// over (Chat/ChatActions).
export const MOST_ATTACHMENT_BYTES = 48 * 1024 * 1024;
export const MOST_ATTACHMENTS = 10;

async function personalDocuments(projectId) {
	if ((projectId ?? "") == "" || typeof nativeApp()?.listMatterDocuments != "function") return [];
	const listed = await nativeApp().listMatterDocuments(projectId).catch(() => null);
	const documents = Array.isArray(listed) ? listed : listed?.documents ?? [];
	return documents.filter(document => (document.deletedAt ?? "") == "");
}

// The member's reaction to a message of a conversation in Messages, put or
// taken back: one with a person, or with an AI colleague on the member's desk.
export async function reactInConversation(send, messageId, emoji) {
	const answer = await nativeApp().reactToChatMessage(send.projectId, send.chatId, messageId, emoji);
	if (send.isPersonal == true) readPersonal();
	else readFirm();
	return answer;
}

// Waits until the conversation's folder lists documents `isNew` accepts, at
// least `count` of them, as the folder's watcher finds the files; up to a few
// seconds, then whatever was found.
async function documentsFound(projectId, isNew, count) {
	const until = Date.now() + 15000;
	let found = [];
	while (Date.now() < until) {
		found = (await personalDocuments(projectId)).filter(isNew);
		if (found.length >= count) break;
		await new Promise(resolve => setTimeout(resolve, 400));
	}
	return found.map(document => ({ documentId: document.id, name: document.displayName ?? document.name ?? "", sizeBytes: document.sizeBytes ?? null }));
}

// Files the member chose with the system's own dialog, put into the
// conversation's folder; the documents they became.
export async function attachFromComputer(projectId, onChosen) {
	const before = new Set((await personalDocuments(projectId)).map(document => document.id));
	const answer = await nativeApp().addMatterDocument(projectId);
	// The dialog closed with nothing chosen: the Matter comes back as it was,
	// and nothing is waited for.
	const listed = Array.isArray(answer?.documents) ? answer.documents : null;
	const added = listed?.filter(document => before.has(document.id) == false && (document.deletedAt ?? "") == "") ?? null;
	if (added != null && added.length == 0) return [];
	onChosen?.();
	return documentsFound(projectId, document => before.has(document.id) == false, Math.max(1, added?.length ?? 1));
}

// Where a file put into a conversation stands: being read, ready, or not
// readable, in the backend's words (D-212).
export async function attachmentState(projectId, documentId) {
	const document = (await personalDocuments(projectId)).find(known => known.id == documentId);
	if (document == null) return { state: "gone", detail: null };
	const state = document.processingState ?? "ready";
	return {
		state: ["processing", "changed"].includes(state) ? "reading" : ["needs_attention", "unsupported"].includes(state) ? "failed"
			: state == "unavailable" ? "gone" : "ready",
		detail: document.attentionDetail ?? null,
	};
}

function textOfRuns(runs) {
	return runs.filter(run => run.revision?.kind != "deleted").map(run => run.text ?? "").join("").replace(/\s+/g, " ").trim();
}

// The first lines of a document as its author laid them out, for a file's
// tile to show its first page in small; none where it cannot be read.
const firstPages = new Map();
export function attachmentFirstLines(projectId, documentId) {
	const key = `${projectId}\n${documentId}`;
	if (firstPages.has(key) == false) {
		const read = Promise.resolve()
			.then(() => nativeApp().readDocumentView(projectId, documentId))
			.then(view => {
				const lines = [];
				for (const block of view?.blocks ?? []) {
					if (lines.length >= 18) break;
					if (block.kind == "paragraph") {
						const paragraph = block.paragraph ?? {};
						const runs = (paragraph.runs ?? []).filter(run => run.revision?.kind != "deleted");
						const text = textOfRuns(runs);
						if (text == "") continue;
						lines.push({
							text,
							isStrong: paragraph.isTitle == true || (paragraph.headingLevel ?? 0) > 0
								|| runs.every(run => run.bold == true || (run.text ?? "").trim() == ""),
							isCentered: paragraph.alignment == "center",
						});
					} else if (block.kind == "table") {
						for (const row of block.table?.rows ?? []) {
							if (lines.length >= 18) break;
							const text = (row.cells ?? []).map(cell => (cell.paragraphs ?? []).map(paragraph => textOfRuns(paragraph.runs ?? [])).join(" ").trim())
								.filter(Boolean).join("   ");
							if (text != "") lines.push({ text, isStrong: row.isHeader == true, isCentered: false });
						}
					}
				}
				return lines;
			})
			.catch(() => []);
		firstPages.set(key, read);
		read.then(lines => {
			if (lines.length == 0) firstPages.delete(key);
		});
	}
	return firstPages.get(key);
}

// Files dropped on the window, by where they are on this computer (the window
// takes a drop itself: Platform/FileDrop), put into the conversation's folder,
// a name already there keeping both; the documents they became.
export async function attachPaths(projectId, paths) {
	const before = new Set((await personalDocuments(projectId)).map(document => document.id));
	for (const path of paths) await nativeApp().addExternalMatterFile(projectId, "", path, false, true);
	return documentsFound(projectId, document => before.has(document.id) == false, paths.length);
}

function base64Of(file) {
	return new Promise((resolve, reject) => {
		const reader = new FileReader();
		reader.onload = () => resolve(String(reader.result).replace(/^data:[^,]*,/, ""));
		reader.onerror = () => reject(reader.error ?? new Error("The file could not be read."));
		reader.readAsDataURL(file);
	});
}

// Files dropped on the conversation, put into its folder; the documents they
// became. A file larger than the firm's computers carry is refused, in words.
export async function attachDropped(projectId, files) {
	const tooLarge = files.filter(file => file.size > MOST_ATTACHMENT_BYTES);
	if (tooLarge.length > 0) {
		throw new Error(`${tooLarge.map(file => file.name).join(", ")} ${tooLarge.length == 1 ? "is" : "are"} larger than 48 MB, more than the firm's computers carry in one message.`);
	}
	const before = new Set((await personalDocuments(projectId)).map(document => document.id));
	const placed = [];
	for (const file of files) {
		const filed = await nativeApp().fileIntoMatter(projectId, file.name, await base64Of(file));
		placed.push(filed?.relativePath ?? filed?.name ?? file.name);
	}
	return documentsFound(projectId, document => before.has(document.id) == false && placed.includes(document.relativePath), placed.length);
}

// A photo's own picture, for the conversation to show it as a messenger does;
// read once. None for a file that is not a picture the window can draw, or is
// larger than a picture shown in place should be.
const PICTURE_TYPES = { png: "image/png", jpg: "image/jpeg", jpeg: "image/jpeg", gif: "image/gif", webp: "image/webp", bmp: "image/bmp" };
const MOST_PICTURE_BYTES = 20 * 1024 * 1024;
const pictures = new Map();

export function pictureType(name) {
	const extension = (name ?? "").split(".").pop().toLowerCase();
	return PICTURE_TYPES[extension] ?? null;
}

// Whether a file goes to an AI model as a picture (D-256): every kind the
// window shows, and a phone's HEIC or HEIF photograph or a TIFF scan, which
// Singularity redraws for the model.
export function isPictureFile(name) {
	const extension = (name ?? "").split(".").pop().toLowerCase();
	return pictureType(name) != null || ["heic", "heif", "tif", "tiff"].includes(extension);
}

export function personalPicture(projectId, file) {
	const type = pictureType(file?.name);
	if (type == null || (file.sizeBytes ?? 0) > MOST_PICTURE_BYTES) return Promise.resolve(null);
	const key = `${projectId}\n${file.documentId}`;
	if (pictures.has(key) == false) {
		// A picture read is kept; one not read yet is asked again next time.
		const read = Promise.resolve()
			.then(() => nativeApp().readDocumentContent(projectId, file.documentId))
			.then(content => (content?.base64 ? `data:${type};base64,${content.base64}` : null))
			.catch(() => null);
		pictures.set(key, read);
		read.then(found => {
			if (found == null && pictures.get(key) == read) pictures.delete(key);
		});
	}
	return pictures.get(key);
}

export function openPersonalFile(projectId, documentId) {
	return nativeApp().openMatterDocument(projectId, documentId);
}

export function showPersonalFile(projectId, documentId) {
	return nativeApp().revealMatterDocument(projectId, documentId);
}

// A new thread of the real Matter, opened at once; its first message names it.
export async function createRealThread() {
	const made = await nativeApp().createProjectChat(snapshot.matterId);
	let named = made;
	try {
		named = await nativeApp().renameProjectChat(snapshot.matterId, made.id, made.revision, "New thread");
	} catch {
		// It keeps the backend's name until its first message.
	}
	await reload();
	return named;
}

// The address of the thread a real chat is shown as.
export function threadIdOfChat(chatId) {
	return threads.find(thread => thread.isReal == true && thread.real?.chatId == chatId)?.id ?? `real:${chatId}`;
}

// The Matter's documents a message can hand over, as the backend lists them.
// The letters filed to the Matter, incoming and outgoing, the latest first
// (D-106): its correspondence, apart from its threads.
// What happened on the open Matter, from its own records (the owner,
// 2026-11-02: "a feed ... and when a new person is added, that too"): each
// document taken in, taken out or changed, with whose it was where the
// record says; and each member's arrival - a person's computer joining, an AI
// colleague made on it here.
export function realMatterHistory() {
	const viewer = members.find(member => member.id == viewerId) ?? null;
	const byActor = actorId => {
		if ((actorId ?? "") == "") return null;
		if (actorId == viewerActorId || (viewer?.actorId ?? "") == actorId) return viewerId;
		return shownMembers().find(member => (member.actorId ?? "") == actorId)?.id ?? null;
	};
	const byName = name => (name ?? "") == "" ? null : shownMembers().find(member => member.name == name)?.id ?? null;
	const documentsById = new Map(realDocuments().map(document => [document.id, document]));
	const changes = (snapshot.documentChanges ?? []).filter(change => (change.at ?? "") != "").map(change => ({
		kind: change.kind,
		at: change.at,
		name: change.name || (change.relativePath ?? "").split("/").pop() || "A document",
		documentId: documentsById.has(change.documentId) ? change.documentId : null,
		byId: change.author == "you" ? viewerId
			: change.author == "member" ? byName(change.authorName)
			: change.kind == "added" ? byActor(documentsById.get(change.documentId)?.addedByActorId)
			: null,
	}));
	const arrivals = [
		...(snapshot.people ?? []).filter(person => (person.firstSeenAt ?? "") != "")
			.map(person => ({ memberId: person.isYou ? viewerId : byActor(person.actorId) ?? personIdForActor(person.actorId, person.name), at: person.firstSeenAt })),
		...(snapshot.agents ?? []).filter(agent => agent.isHere == true && (agent.createdAt ?? "") != "")
			.map(agent => ({ memberId: agent.id, at: agent.createdAt })),
	];
	// A full list may begin mid-batch: the bridge gives the latest
	// kSharedChanges (MatterBridge.cpp), so the history leaves its oldest out.
	return { changes, arrivals, isCut: (snapshot.documentChanges ?? []).length >= SHARED_CHANGES };
}

const SHARED_CHANGES = 200;

export function realLetters() {
	return snapshot.letters ?? [];
}

export function realLetter(letterId) {
	return realLetters().find(letter => letter.id == letterId) ?? null;
}

export function realDocuments() {
	const documents = (snapshot.documents ?? []).filter(document => (document.deletedAt ?? "") == "");
	// When two computers sharing the Matter's folder both recorded the same new
	// file (before D-109 gave a file the same id on both), the record the
	// folder no longer answers to is left out: the file is one document.
	const available = new Set(documents.filter(document => document.processingState != "unavailable").map(document => document.relativePath));
	return documents.filter(document => document.processingState != "unavailable" || available.has(document.relativePath) == false);
}

// A file from this computer, added to the Matter's documents through the
// system's own file dialog; the documents it added, once they are listed.
export async function addDocumentFromComputer() {
	const before = new Set(realDocuments().map(document => document.id));
	await nativeApp().addMatterDocument(snapshot.matterId);
	await reload();
	return realDocuments().filter(document => before.has(document.id) == false);
}

// Files and folders dropped on the open Matter's documents or one of its
// threads, by where they are on this computer (the window takes a drop
// itself: Platform/FileDrop): copied into its folder `folder` ("" for the
// Matter's own), a name already there keeping both, a folder whole. The
// documents they became, and what could not be added, in words.
export async function addDroppedToMatter(folder, dropped) {
	const matterId = snapshot.matterId;
	if (matterId == null) throw new Error("This Matter is not open on this computer.");
	const before = new Set(realDocuments().map(document => document.id));
	const problems = [];
	for (const item of dropped) {
		if ((item?.path ?? "") == "") continue;
		try {
			if (item.isFolder == true) await nativeApp().addExternalMatterFolder(matterId, folder, item.path);
			else await nativeApp().addExternalMatterFile(matterId, folder, item.path, false, true);
		} catch (error) {
			const name = item.path.split(/[\\/]/).filter(Boolean).at(-1) ?? "A file";
			problems.push(error?.message ? `${name}: ${error.message}` : `${name} could not be added.`);
		}
	}
	await reload();
	return { added: realDocuments().filter(document => before.has(document.id) == false), problems };
}

// A document the lawyer takes out of the Matter, one added by mistake among
// them. A file still in the Matter's folder goes to this computer's Recycle
// Bin, and its deletion goes to the firm's other computers that hold the
// Matter, sealed as every change is (D-123); the version the window read is the
// one deleted, so a file changed on another computer meanwhile is not lost
// unseen. Then the Matter forgets the document, so it is not left listed as
// missing: the lawyer took it out on purpose.
export async function removeDocumentFromMatter(documentId) {
	const document = realDocument(documentId);
	if (document == null) return;
	for (const step of removalSteps(document)) {
		if (step == "delete") await nativeApp().deleteMatterDocument(snapshot.matterId, documentId, document.version);
		else await nativeApp().removeMatterDocument(snapshot.matterId, documentId);
	}
	await reload();
}

// The colleagues running on this computer that a member's message names but
// that do not take it up: the computer that signed it is not one the person
// here confirmed as its author's, so it is shown and never acted on
// (Chat/MessageProvenance). Until then the window says so where the message
// stands (2026-10-29: Priya asked Unicorn, which runs on Marcus's computer,
// and nothing happened, with nothing said to either of them).
export function heldColleagues(turn, agents) {
	if (turn.messageProvenance != "unconfirmed") return [];
	const named = new Set(turn.mentions ?? []);
	return (agents ?? []).filter(agent => agent.isHere == true && named.has(agent.id)).map(agent => ({ id: agent.id, name: agent.name }));
}

// The computers of a member of the open Matter that the person here has not
// confirmed as theirs, each with the code its owner reads out (D-123).
export async function unconfirmedComputersOf(actorId) {
	if (snapshot.matterId == null) return [];
	const activity = await nativeApp().getSharedActivity(snapshot.matterId);
	const member = (activity?.members ?? []).find(entry => entry.actorId == actorId);
	return (member?.computers ?? []).filter(computer => computer.state == "unconfirmed");
}

// The person here compared the code with the computer's owner: it is
// confirmed as theirs, so what it signs counts as theirs here, and the
// colleagues running here take up what they ask (project.confirmComputer).
export async function confirmMemberComputer(actorId, deviceId) {
	if (snapshot.matterId == null) return;
	await nativeApp().confirmComputer(snapshot.matterId, actorId, deviceId);
	await reload();
}

// What taking a document out of the Matter does, in order: "delete" moves the
// file in the Matter's folder to the Recycle Bin (and the deletion travels);
// "forget" takes the document off the Matter's list. A document in Singularity's
// own store, or one whose file is no longer in the folder, is only
// forgotten.
export function removalSteps(document) {
	if (document.sourceMode != "managed" && document.processingState != "unavailable") return ["delete", "forget"];
	return ["forget"];
}

// Where a document a colleague wrote stands for the member (D-143): it asks
// them until one accepts or returns it, as a revised draft does.
export function writtenStatus(step) {
	if (step?.state == "accepted") return { kind: "settled", words: "Accepted" };
	if (step?.state == "returned") return { kind: "underway", words: step.stateWords || "Returned with comments" };
	return { kind: "yours", words: "Written for your review" };
}

// The member's decision on a document a colleague wrote (D-143), as on a
// revised draft: accepted as it is now, or returned with what should change,
// which the colleague takes up as its next request.
export async function decideWritten(assignmentId, step, decision, comment) {
	const assignment = await nativeApp().decideWrittenDocument(snapshot.matterId, assignmentId, step, decision, comment ?? "");
	scheduleReload();
	return assignment;
}

export async function decideReview(reviewId, version, decision, comment) {
	const review = await nativeApp().decideDocumentReview(snapshot.matterId, reviewId, version, decision, comment ?? "");
	scheduleReload();
	return review;
}

// The version waiting, made again without the changes the lawyer leaves out
// (D-119): a workbook's cells, a deck's passages, a PDF's marks. No model is
// asked; the next version is handed in under the lawyer's own word.
export async function leaveOutChanges(reviewId, version, changeIds) {
	const review = await nativeApp().leaveOutDocumentReviewChanges(snapshot.matterId, reviewId, version, changeIds);
	scheduleReload();
	return review;
}

export async function retryReview(reviewId) {
	const review = await nativeApp().retryDocumentReview(snapshot.matterId, reviewId);
	scheduleReload();
	return review;
}

export function saveReviewCopy(reviewId, version) {
	return nativeApp().saveDocumentReviewCopy(snapshot.matterId, reviewId, version);
}

// "Approved by Evelyn Hart on Oct 25 at 14:02, as it was handed in" (D-174).
// Whether a work product's status is its acceptance: "Accepted", or "Accepted
// by Marcus Bell on Oct 29, 2026 at 7:58 AM, as it was handed in" (2026-10-29:
// a version Marcus accepted was listed as Replaced).
export function isAcceptedWork(status) {
	return status?.kind == "settled" && /^(Version \d+ · )?(Accepted|Approved)/.test(status.words ?? "");
}

export function acceptedWords(decision) {
	if (decision == null) return "Accepted";
	const at = decision.decidedAt ? new Date(decision.decidedAt) : null;
	const when = at != null && Number.isNaN(at.getTime()) == false
		? ` on ${at.toLocaleDateString("en-US", { month: "short", day: "numeric", year: "numeric" })} at ${at.toLocaleTimeString("en-US", { hour: "numeric", minute: "2-digit" })}`
		: "";
	const how = decision.isAsWritten == false ? ", with the changes made to it since it was handed in" : ", as it was handed in";
	return `Approved by ${decision.actorName || "you"}${when}${how}`;
}

// Whether Word, Excel and PowerPoint are connected on this computer, so a place opens in
// them (Office.json): asked once a minute at most.
let officeAsked = { at: 0, connected: false };
export async function isOfficeConnected() {
	if (Date.now() - officeAsked.at < 60000) return officeAsked.connected;
	try {
		const status = await nativeApp().getOfficeStatus();
		officeAsked = { at: Date.now(), connected: status?.connected == true && status?.listening == true };
	} catch {
		officeAsked = { at: Date.now(), connected: false };
	}
	return officeAsked.connected;
}

// Shows a place in a document - a message's, a colleague's change: in Word,
// Excel or PowerPoint where they are connected here, the document opening
// there and its pane going to the place; otherwise in the document as this
// window shows it.
export async function showPlace(place, open) {
	const isOffice = /\.(docx|docm|doc|xlsx|xlsm|xls|pptx|ppt)$/i.test(place.documentName ?? "");
	if (isOffice && await isOfficeConnected()) {
		await nativeApp().revealInOffice(snapshot.matterId, place.documentId, place);
		return "office";
	}
	open({ kind: "document", documentId: place.documentId, at: { section: place.section ?? "", quote: place.quote ?? "" } });
	return "here";
}

export function openInWord(documentId) {
	return nativeApp().openMatterDocument(snapshot.matterId, documentId);
}

export function showInFolder(documentId) {
	return nativeApp().revealMatterDocument(snapshot.matterId, documentId);
}

export function readDocumentView(documentId) {
	return nativeApp().readDocumentView(snapshot.matterId, documentId);
}

// ---- The colleagues at work, on every computer ------------------------------

// The computers connected now to each of the firm's Matters, as its exchange
// last reported them (data/coordination.js tells this module).
let connectedByMatter = new Map();

export function noteConnectedComputers(matters) {
	connectedByMatter = new Map((matters ?? []).map(matter => [matter.matterId, new Set(matter.connected ?? [])]));
}

// Whether a colleague another computer runs is away now: it runs on its
// responsible lawyer's computer, and none of that lawyer's computers is
// connected to the Matter. What is not known - no exchange, no lawyer, no
// computer of theirs - is never away.
export function isHostAway(agent, connected, people) {
	if (agent?.isHere == true || connected == null) return false;
	// Its responsible lawyer by their seat; by name only where the card names no seat.
	const responsibleActor = (agent?.responsibleActorId ?? "").trim();
	const responsible = (agent?.responsibleName ?? "").trim();
	const owner = responsibleActor != "" ? (people ?? []).find(person => person.actorId == responsibleActor) ?? null
		: responsible == "" ? null : (people ?? []).find(person => person.name == responsible) ?? null;
	const computers = owner?.computers ?? [];
	if (computers.length == 0) return false;
	return computers.every(device => connected.has(device) == false);
}

// Whether a colleague of the real Matter is at work now: the computer that
// runs it knows; any other knows it from the colleague's card, and from the
// Matter's own record of its reviews, both in the shared folder.
function isAtWork(agent) {
	// Writing an answer is no work to act out: the conversation says "typing"
	// (the owner, 2026-10-26: "no work animation when she just answers my DM").
	if ((agent.answeringChatId ?? "") != "") return false;
	// The computer that runs it says what its face acts out (D-013): at work
	// while it works or thinks, not while its run begins, ends or waits.
	if (agent.isHere == true && (agent.isRunning == true || agent.state == "running"))
		return agent.presence == "work" || agent.presence == "think";
	// One another computer runs is at work only while that computer is there: a
	// card, or a review, left running as it went away is no work now
	// (2026-10-31: Unicorn read "Working" with Marcus's computer off).
	if (isHostAway(agent, connectedByMatter.get(snapshot.matterId) ?? null, snapshot.people)) return false;
	if (agent.isHere != true && agent.state == "running") return true;
	return (snapshot.reviews ?? []).some(review => isReviewOf(review, agent)
		&& isUnderway(review.versions?.at(-1)));
}

// The colleagues writing an answer in a chat now (their run's stage, D-129),
// as members: the thread says who is typing, as a messenger does.
export function colleaguesTypingIn(chatId) {
	if (chatId == null || chatId == "") return [];
	return colleagues()
		.filter(agent => (agent.answeringChatId ?? "") != "" && agent.answeringChatId == chatId)
		.map(agent => memberIdFor(agent.name, true, false));
}

// What each real colleague's face acts out now: at work, its work, on every
// computer; otherwise what the computer that runs it reports, or rest.
export function realColleaguePresences() {
	return colleagues().map(agent => ({
		memberId: memberIdFor(agent.name, true, false),
		presence: isAtWork(agent) ? "work" : agent.isHere == true ? (agent.presence || "rest") : "rest",
	}));
}

// A moment the backend wrote (UTC) as the calendar keeps one: local time, to
// the minute.
function calendarMoment(at) {
	const moment = new Date(at);
	if (Number.isNaN(moment.getTime())) return null;
	const two = value => String(value).padStart(2, "0");
	return `${moment.getFullYear()}-${two(moment.getMonth() + 1)}-${two(moment.getDate())}T${two(moment.getHours())}:${two(moment.getMinutes())}`;
}

function laterMoment(start, end) {
	if (end > start) return end;
	const moment = new Date(start);
	moment.setMinutes(moment.getMinutes() + 1);
	return calendarMoment(moment.toISOString());
}

function costLine(micros) {
	if (micros == null) return "";
	const amount = micros / 1000000;
	return ` Model cost ${amount < 0.01 ? `$${amount.toFixed(4)}` : `$${amount.toFixed(2)}`}.`;
}

// The colleagues' working time on the real Matter, as busy time in their
// calendar, as a person's work is: each version of a review from when it
// began to when it ended, or to now while it is under way, and each answer to
// a question about it.
export function realColleagueEvents() {
	const events = [];
	for (const review of snapshot.reviews ?? []) {
		const memberId = colleagueIdFor(review.agentId, review.agentName);
		const thread = threadOfChat(review.chatId);
		const name = (review.sourceFileName || "the agreement").replace(/\.[^.]+$/, "");
		for (const version of review.versions ?? []) {
			const execution = version.execution ?? {};
			const begun = execution.startedAt || version.createdAt;
			const isNow = isUnderway(version);
			const ended = execution.finishedAt || (isNow ? new Date().toISOString() : null);
			const start = begun ? calendarMoment(begun) : null;
			const end = ended ? calendarMoment(ended) : null;
			if (start == null || end == null) continue;
			const handIn = execution.preparation?.handIn?.costMicros ?? 0;
			events.push({
				id: `real_${review.id}_${version.number}_${execution.attempt ?? 1}`,
				calendarId: "cal_colleagues",
				isRecord: true,
				title: `${review.agentName}: ${name}, version ${version.number}`,
				start,
				end: laterMoment(start, end),
				memberIds: [memberId],
				matterId: snapshot.uiMatterId,
				threadId: thread?.id,
				description: isNow ? "At work on it now." : `Busy on the review.${costLine(execution.costMicros != null ? execution.costMicros + handIn : null)}`,
			});
			for (const answer of execution.preparation?.answers ?? []) {
				const took = (answer.attempts ?? []).reduce((sum, attempt) => sum + (attempt.latencyMilliseconds ?? 0), 0);
				const until = calendarMoment(answer.at);
				const from = calendarMoment(new Date(new Date(answer.at).getTime() - took).toISOString());
				if (until == null || from == null) continue;
				events.push({
					id: `real_${review.id}_answer_${answer.messageId}`,
					calendarId: "cal_colleagues",
				isRecord: true,
					title: `${review.agentName}: a question about ${name}`,
					start: from,
					end: laterMoment(from, until),
					memberIds: [memberId],
					matterId: snapshot.uiMatterId,
					threadId: thread?.id,
					description: `Answered in the thread.${costLine(answer.costMicros)}`,
				});
			}
		}
	}
	return events;
}

// This module keeps the window's state. While the window runs it is never
// swapped for a fresh copy, which would start empty: a change to it reloads
// the window (2026-10-26: a hot swap left the Mac's window with no Matter, so
// it read nothing and every post was refused).
if (import.meta.hot) import.meta.hot.decline();
