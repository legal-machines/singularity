import { useState } from "react";

import { Avatar } from "../../components/Avatar/Avatar.jsx";
import { Button } from "../../components/Button/Button.jsx";
import { FileKindIcon } from "../../components/FileKind/FileKind.jsx";
import { Icon } from "../../components/Icon/Icon.jsx";
import { useNavigation } from "../../components/Navigation/navigation.js";
import { Word } from "../../components/Parts/Parts.jsx";
import { Views } from "../../components/Views/Views.jsx";
import { ConfirmDialog } from "../../components/Manage/Manage.jsx";
import { toast } from "../../components/Toast/toast.js";
import { archivedThreads, createRealThread, deleteThread, hasNative, openMatterRealId, setThreadArchived, threadIdOfChat } from "../../data/native.js";
import { openDocumentLens } from "../../components/DocumentLens/lens.js";
import { decisions, members, mockNow, viewerId, workProducts } from "../../data/workspace.js";
import { inboxTime } from "../Inbox/inboxView.js";
import { beginningOf, colleagueState, eventWords, isThreadUnread, lastEvent, threadEvents, threadStatus } from "../threadState.js";
import { findMember, firstName, formatClock, formatDay, plainWords, positionOf } from "../words.js";

import styles from "./MatterDesk.module.css";

// A Matter's overview as the team's desk: who works on it, faces side by
// side, people and AI colleagues alike; what happened, day by day; and what
// the firm's books say of it. Its threads and its documents are their
// sections' own, not the overview's. No panels and no frames: room, faces and
// type carry it.
export function MatterDesk({ matter, people, state, matterThreads, matterDocuments, matterWork, history = null, isReal, invitedIds, onShare, facts, onDocuments, onThreads }) {
	const { open } = useNavigation();
	// The member here first, then the rest; whoever is there now wears the
	// green dot every face of theirs wears (components/Avatar/online.js). The
	// Matter travels sealed only to computers its people confirmed, so no sign
	// of the seal stands on a face.
	const ordered = [...people.filter(member => member?.id == viewerId), ...people.filter(member => member?.id != viewerId)];
	const tile = member => {
		const doing = member.kind == "ai_colleague" ? colleagueState(member.id, state) : null;
		const isYou = member.id == viewerId;
		const word = invitedIds.includes(member.id) ? <Word>Invited</Word>
			: doing?.kind == "working" ? <Word tone="work">Working</Word>
			: doing?.kind == "yours" ? <Word tone="yours">Asks you</Word>
			: member.away != null ? <Word tone="time">Away</Word> : null;
		return (
			<button key={member.id} type="button" className={styles.member} disabled={isYou}
				data-tooltip={isYou ? undefined : positionOf(member)}
				onClick={() => open({ kind: "profile", memberId: member.id })}>
				<Avatar member={member} size={56} presence={doing?.presence} />
				<span className={styles.member_name}>{isYou ? "You" : firstName(member)}</span>
				<span className={styles.member_state}>{word ?? positionOf(member)}</span>
			</button>
		);
	};
	return (
		<div className={styles.desk}>
			<section className={styles.team} aria-label="On the Matter">
				<div className={styles.faces}>
					{ordered.map(tile)}
					{onShare != null ? (
						<button type="button" className={styles.member} onClick={onShare}>
							<span className={styles.add}><Icon name="plus" size={22} /></span>
							<span className={styles.member_name}>Share</span>
							<span className={styles.member_state}>With a colleague</span>
						</button>
					) : null}
				</div>
			</section>

			<Unread matterThreads={matterThreads} state={state} onAll={onThreads} />

			<History matterThreads={matterThreads} matterDocuments={matterDocuments} matterWork={matterWork} history={history} state={state} isReal={isReal} onDocuments={onDocuments} />

			{facts.length > 0 ? (
				<section className={styles.section} aria-label="About">
					<h2 className={styles.title}>About</h2>
					<dl className={styles.facts}>
						{facts.map(fact => (
							<div key={fact.label} className={styles.fact}>
								<dt>{fact.label}</dt>
								<dd>{fact.value}</dd>
							</div>
						))}
					</dl>
				</section>
			) : null}
		</div>
	);
}

// The threads the member has not read yet, the latest first, as the Inbox
// lists them, on its unread ground. None, and it says nothing.
const UNREAD_SHOWN = 3;

function Unread({ matterThreads, state, onAll }) {
	const { open } = useNavigation();
	const now = hasNative() ? new Date() : new Date(mockNow);
	const unread = matterThreads
		.filter(thread => isThreadUnread(thread.id, state) && (state.doneIds ?? []).includes(thread.id) == false)
		.map(thread => ({ thread, event: lastEvent(thread, state) }))
		.sort((first, second) => (second.event.at ?? "").localeCompare(first.event.at ?? ""));
	if (unread.length == 0) return null;
	return (
		<section className={styles.section} aria-label="Unread">
			<div className={styles.title_row}>
				<h2 className={styles.title}>Unread<span className={styles.count}>{unread.length}</span></h2>
				{unread.length > UNREAD_SHOWN ? <button type="button" className={styles.all} onClick={onAll}>All threads</button> : null}
			</div>
			<ul className={`${styles.rows} ${styles.rows_unread}`}>
				{unread.slice(0, UNREAD_SHOWN).map(({ thread, event }) => {
					const face = findMember(event.byId ?? event.authorId);
					const status = threadStatus(thread, state);
					const word = status.kind == "yours" ? <Word tone="yours">Asks you</Word> : status.kind == "due" ? <Word tone="time">{status.words}</Word> : null;
					return (
						<li key={thread.id} className={`${styles.row} ${styles.row_unread}`}>
							<button type="button" className={styles.line} onClick={() => open({ kind: "thread", threadId: thread.id, from: "matter" })}>
								{face != null ? <Avatar member={face} size={40} /> : <span className={styles.sign}><Icon name="messages" size={20} /></span>}
								<span className={styles.text}>
									<span className={styles.row_head}>
										<span className={styles.row_title}>{thread.title}</span>
										<time className={styles.row_time} dateTime={event.at}>{inboxTime(event.at, now)}</time>
									</span>
									<span className={styles.row_said}>
										{face != null && event.kind != "begun" ? <span className={styles.row_author}>{firstName(face)}: </span> : null}
										{eventWords(event)}
									</span>
								</span>
								{word != null ? <span className={styles.row_word}>{word}</span> : null}
							</button>
						</li>
					);
				})}
			</ul>
		</section>
	);
}

// The Matter's Threads: its threads as the Inbox lists them - who spoke
// last, what they said, when, and what a thread waits for; a new one begins
// at the top of them, and the archived wait under a word.
export function MatterThreads(props) {
	return <div className={styles.desk}><Threads {...props} now={hasNative() ? new Date() : new Date(mockNow)} /></div>;
}

function Threads({ matter, matterThreads, state, isReal, now }) {
	const { open } = useNavigation();
	const [isMaking, setMaking] = useState(false);
	// Open or archived, as the Matters page chooses its own.
	const [view, setView] = useState("open");
	// An archived thread deleted, once confirmed, as its own page deletes one.
	const [deleting, setDeleting] = useState(null);
	const archived = isReal ? archivedThreads(matter.id) : [];
	const views = [{ id: "open", label: "Open" }, { id: "archived", label: "Archived" }];
	const openThread = threadId => open({ kind: "thread", threadId, from: "matter" });

	async function begin() {
		if (isMaking) return;
		setMaking(true);
		try {
			const made = await createRealThread();
			openThread(threadIdOfChat(made.id));
		} catch (error) {
			toast(error?.message ?? "The thread could not be made.", { tone: "seal" });
		}
		setMaking(false);
	}

	return (
		<section className={styles.section} aria-label="Threads">
			{isReal ? (
				<div className={styles.views}>
					<Views label="Which threads" views={views} view={view} onChange={setView} controls="matter-threads" />
				</div>
			) : null}
			{view == "open" ? (
			<ul className={styles.rows} id="matter-threads">
				{isReal ? (
					<li className={styles.row}>
						<button type="button" className={styles.line} disabled={isMaking} onClick={begin}>
							<span className={styles.sign}><Icon name="plus" size={20} /></span>
							<span className={styles.text}>
								<span className={styles.row_title}>Start a thread</span>
								<span className={styles.row_said}>Name a colleague in it to give them the work.</span>
							</span>
						</button>
					</li>
				) : null}
				{matterThreads.map(thread => {
					const event = lastEvent(thread, state);
					const face = findMember(event.byId ?? event.authorId);
					const status = threadStatus(thread, state);
					const word = status.kind == "yours" ? <Word tone="yours">Asks you</Word>
						: status.kind == "settled" ? <Word tone="done">Closed</Word>
						: status.kind == "due" ? <Word tone="time">{status.words}</Word>
						: status.kind == "failed" ? <Word tone="yours">Stopped</Word> : null;
					return (
						<li key={thread.id} className={styles.row}>
							<button type="button" className={styles.line} onClick={() => openThread(thread.id)}>
								{face != null ? <Avatar member={face} size={40} /> : <span className={styles.sign}><Icon name="messages" size={20} /></span>}
								<span className={styles.text}>
									<span className={styles.row_head}>
										<span className={styles.row_title}>{thread.title}</span>
										<time className={styles.row_time} dateTime={event.at}>{inboxTime(event.at, now)}</time>
									</span>
									<span className={styles.row_said}>
										{face != null && event.kind != "begun" ? <span className={styles.row_author}>{firstName(face)}: </span> : null}
										{eventWords(event)}
									</span>
								</span>
								{word != null ? <span className={styles.row_word}>{word}</span> : null}
							</button>
						</li>
					);
				})}
			</ul>
			) : archived.length == 0 ? (
				<p className={styles.none} id="matter-threads">No thread is archived.</p>
			) : (
				<ul className={styles.rows} id="matter-threads">
					{archived.map(thread => (
						<li key={thread.id} className={styles.row}>
							<button type="button" className={styles.line} onClick={() => openThread(thread.id)}>
								<span className={styles.sign}><Icon name="archive" size={19} /></span>
								<span className={styles.text}>
									<span className={styles.row_title}>{thread.title}</span>
									<span className={styles.row_said}>Archived</span>
								</span>
							</button>
							<span className={styles.row_action}>
								<Button size="small" onClick={async () => {
									try {
										await setThreadArchived(thread.id, false);
										toast(`${thread.title} is back among the threads.`);
									} catch (error) {
										toast(error?.message ?? "The thread could not be restored. Try again.", { tone: "seal" });
									}
								}}>Restore</Button>
								<Button size="small" onClick={() => setDeleting(thread)}>Delete</Button>
							</span>
						</li>
					))}
				</ul>
			)}
			{deleting != null ? (
				<ConfirmDialog
					title={`Delete ${deleting.title}?`}
					description="It leaves the Matter's threads for everyone on it. What was written in it and the work it led to stay in the Matter's history."
					confirm="Delete"
					onClose={() => setDeleting(null)}
					onConfirm={async () => {
						const undo = await deleteThread(deleting.id);
						toast(`${deleting.title} is deleted.`, { action: { label: "Undo", run: undo } });
					}} />
			) : null}
		</section>
	);
}

// What happened on the Matter, as a feed in one column and one manner: each
// line a sign, what happened in a few words, what it concerned under them,
// and when. Documents are one line only while the same person took them in,
// one after another, within a few minutes. The latest few show, so it reads
// without scrolling; the rest are a press away.
const SHOWN = 6;
// Changes further apart than this are separate lines.
const BATCH_GAP_MS = 5 * 60 * 1000;

const DOCUMENT_WORDS = {
	added: { verb: "added", alone: count => count == 1 ? "Document added" : `${count} documents added` },
	removed: { verb: "removed", alone: count => count == 1 ? "Document removed" : `${count} documents removed` },
	changed: { verb: "changed", alone: count => count == 1 ? "Document changed" : `${count} documents changed` },
	moved: { verb: "moved", alone: count => count == 1 ? "Document moved" : `${count} documents moved` },
	returned: { verb: "brought back", alone: count => count == 1 ? "Document back" : `${count} documents back` },
};

function History({ matterThreads, matterDocuments, matterWork, history, state, isReal, onDocuments }) {
	const { open } = useNavigation();
	const lines = [];
	// The documents, batch by batch: from the Matter's record of its changes,
	// or, in the prototype, from when each was added and by whom.
	const changes = history != null
		? history.changes
		: matterDocuments.filter(item => item.added != null && item.workProductId == null && item.draftOf == null)
			.map(item => ({ kind: "added", at: item.added, name: item.name, documentId: item.documentId ?? null, byId: memberNamed(item.by)?.id ?? null, item }));
	const batches = batchesOf(changes);
	// The record's oldest batch may be cut short when the list is full.
	if (history?.isCut == true) batches.shift();
	for (const batch of batches) {
		const words = DOCUMENT_WORDS[batch.kind] ?? DOCUMENT_WORDS.changed;
		const by = batch.byId != null ? findMember(batch.byId) : null;
		const count = batch.items.length;
		const names = [...new Set(batch.items.map(change => change.name))];
		const only = count == 1 ? batch.items[0] : null;
		lines.push({
			key: `documents:${batch.kind}:${batch.items[0].at}:${batch.byId ?? ""}`, at: batch.items[batch.items.length - 1].at,
			icon: batch.kind == "removed" ? "trash" : "document",
			title: by == null ? words.alone(count) : `${by.id == viewerId ? "You" : by.name} ${words.verb} ${count == 1 ? "a document" : `${count} documents`}`,
			about: names.length <= 2 ? names.join(" and ") : `${names.slice(0, 2).join(", ")} and ${names.length - 2} more`,
			onOpen: batch.kind == "removed" ? null
				: only != null && only.documentId != null ? () => openDocument(only.item ?? { documentId: only.documentId, name: only.name }, isReal, open)
				: onDocuments,
		});
	}
	// Each member's arrival, people and AI colleagues alike.
	for (const arrival of history?.arrivals ?? []) {
		const member = findMember(arrival.memberId);
		if (member == null) continue;
		lines.push({
			key: `joined:${member.id}`, at: arrival.at, member,
			title: `${member.id == viewerId ? "You" : member.name} joined`,
			about: positionOf(member) ?? "",
			onOpen: member.id == viewerId ? null : () => open({ kind: "profile", memberId: member.id }),
		});
	}
	for (const item of matterWork ?? []) {
		if (item.delivered == null) continue;
		lines.push({
			key: `work:${item.id}`, at: item.delivered, icon: "edit", title: "Work handed in",
			about: `${item.title}, version ${item.version}${findMember(item.authorId) != null ? `, by ${findMember(item.authorId).name}` : ""}`,
			onOpen: item.documentId != null ? () => open({ kind: "document", documentId: item.documentId }) : null,
		});
	}
	for (const thread of matterThreads) {
		const openThread = () => open({ kind: "thread", threadId: thread.id, from: "matter" });
		const begun = beginningOf(thread);
		const starter = findMember(thread.posts[0]?.authorId ?? begun.authorId);
		const startedAt = thread.posts[0]?.at ?? begun.at;
		if (startedAt != null) lines.push({ key: `begun:${thread.id}`, at: startedAt, icon: "messages", title: "Thread started", about: `${thread.title}${starter != null ? `, by ${starter.name}` : ""}`, onOpen: openThread });
		for (const event of threadEvents(thread, state)) {
			if (event.kind == "post" && event.workProductId != null && isReal != true) {
				const item = workProducts[event.workProductId];
				const author = findMember(event.authorId);
				lines.push({ key: `work:${thread.id}:${event.id}`, at: event.at, icon: "edit", title: "Work handed in", about: `${item.title}, version ${item.version}${author != null ? `, by ${author.name}` : ""}`,
					onOpen: item.documentId != null ? () => open({ kind: "document", documentId: item.documentId }) : openThread });
			}
			if (event.kind == "closed") {
				const by = findMember(event.byId);
				lines.push({ key: `closed:${thread.id}:${event.id}`, at: event.at, icon: "closed", title: "Thread closed", about: `${thread.title}${by != null ? `, by ${by.name}` : ""}`, onOpen: openThread });
			}
		}
		if (thread.decisionId != null && state.choices[thread.decisionId] == null) {
			const decision = decisions[thread.decisionId];
			const post = thread.posts.find(item => item.decisionId == thread.decisionId);
			lines.push({ key: `decision:${thread.decisionId}`, at: post?.at ?? "", icon: "info", tone: "yours", title: "Waiting for your decision", about: plainWords(decision.question), onOpen: openThread });
		}
	}
	return <HistoryLines lines={lines.sort((first, second) => (second.at ?? "").localeCompare(first.at ?? ""))} />;
}

// Changes in batches: one kind, by one person, each within a few minutes of
// the one before it; in the order they happened. A change whose author the
// record no longer says (its document since taken out) stays with the batch
// beside it.
function batchesOf(changes) {
	const batches = [];
	const sorted = [...changes].sort((first, second) => (first.at ?? "").localeCompare(second.at ?? ""));
	for (const change of sorted) {
		const last = batches[batches.length - 1];
		const lastAt = last != null ? Date.parse(last.items[last.items.length - 1].at) : NaN;
		const isSameHand = last != null && (last.byId == change.byId || last.byId == null || change.byId == null);
		if (isSameHand && last.kind == change.kind && Date.parse(change.at) - lastAt <= BATCH_GAP_MS) {
			last.items.push(change);
			last.byId = last.byId ?? change.byId;
		} else {
			batches.push({ kind: change.kind, byId: change.byId, items: [change] });
		}
	}
	return batches;
}

// The member a prototype record names by first or full name.
function memberNamed(name) {
	if ((name ?? "") == "") return null;
	return members.find(member => member.name == name || member.name.split(" ")[0] == name) ?? null;
}

function HistoryLines({ lines }) {
	const [isAll, setAll] = useState(false);
	if (lines.length == 0) return null;
	const shown = isAll ? lines : lines.slice(0, SHOWN);
	return (
		<section className={styles.section} aria-label="What happened">
			<h2 className={styles.title}>What happened</h2>
			<ul className={styles.events}>
				{shown.map(line => (
					<li key={line.key}>
						<button type="button" className={styles.event} disabled={line.onOpen == null} onClick={() => line.onOpen?.()}>
							{line.member != null
								? <span className={styles.event_face}><Avatar member={line.member} size={36} showsOnline={false} /></span>
								: <span className={styles.event_sign} data-tone={line.tone}><Icon name={line.icon} size={18} /></span>}
							<span className={styles.event_body}>
								<span className={line.tone == "yours" ? `${styles.event_text} ${styles.event_open}` : styles.event_text}>{line.title}</span>
								<span className={styles.event_about}>{line.about}</span>
							</span>
							<span className={styles.event_when}>{shortWhen(line.at)}</span>
						</button>
					</li>
				))}
			</ul>
			{lines.length > SHOWN ? (
				<button type="button" className={styles.more} onClick={() => setAll(all => !all)}>
					{isAll ? "Show fewer" : `Show all ${lines.length}`}
				</button>
			) : null}
		</section>
	);
}

// A document opens in its own application; in the prototype, on its page.
async function openDocument(item, isReal, open) {
	if (isReal != true) {
		if (item.documentId != null) open({ kind: "document", documentId: item.documentId });
		return;
	}
	if (item.kind == "gone" || item.kind == "missing") {
		toast(item.detail ?? `${item.name} is not in the Matter's folder on this computer.`, { tone: "seal" });
		return;
	}
	openDocumentLens({ projectId: openMatterRealId(), documentId: item.documentId, name: item.name });
}



// "6:18 PM" today, "Yesterday", "Oct 28" before.
function shortWhen(iso) {
	if (iso == null || iso == "") return "";
	const day = formatDay(iso);
	if (day == "Today") return formatClock(iso);
	if (day == "Yesterday") return "Yesterday";
	const date = new Date(iso);
	return Number.isNaN(date.getTime()) ? "" : new Intl.DateTimeFormat("en-US", { month: "short", day: "numeric" }).format(date);
}
