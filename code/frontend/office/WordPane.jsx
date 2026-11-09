import { useEffect, useMemo, useState } from "react";

import { decide, deskFor, familyOf, isUnderway, markKey, marksOfSource, messagesOf, openInOffice, postToMatter, threadFor, titleOf, useConversation, useMatter, useReveal, workOn } from "./data.js";
import { clauseAtCursor, markupBy, onSelectionChanged, revealPlace, showInDocument } from "./host.js";
import { colleagueMember, Face, MatterMark, personMember, presenceOf } from "./people.jsx";
import { when } from "./words.js";

import styles from "./pane.module.css";

// Singularity in Word: the Matter of the document the lawyer is in, at the
// document. The document stays in Word; the Matter, its thread, its
// colleagues and the decisions belong to Singularity, and the pane shows only
// what concerns this document and the clause at the cursor.
//   - where the work on it stands, and the decision the lawyer holds;
//   - the clause at the cursor, why the colleague changed it, what the
//     thread said about it, and a word to a colleague about it, which goes to
//     the Matter's thread like any message, with the clause as its place;
//   - the thread's latest messages, each with the clause it is about, and the
//     version's changes, folded.
// A place in it asked from Singularity's window ("Show in Word") is gone to.
export function WordPane({ place, reveal }) {
	const matterId = place.matterId;
	const documentId = place.documentId;
	const { data, problem, reload } = useMatter(matterId);
	const [clause, setClause] = useState(null);
	const [version, setVersion] = useState(0);

	useReveal(documentId, reveal, request => {
		revealPlace(request).catch(() => {});
	});

	// The clause follows the cursor.
	useEffect(() => {
		let isCurrent = true;
		let timer = null;
		const read = () => {
			clearTimeout(timer);
			timer = setTimeout(() => {
				clauseAtCursor().then(found => { if (isCurrent) setClause(found); }, () => {});
			}, 250);
		};
		read();
		const stop = onSelectionChanged(read);
		return () => {
			isCurrent = false;
			clearTimeout(timer);
			stop();
		};
	}, []);

	useEffect(() => setVersion(value => value + 1), [data]);

	const document = data?.documents.find(entry => entry.id == documentId) ?? null;
	const work = data != null ? workOn(documentId, data.reviews) : { kind: "none", review: null, version: null };
	const chatId = data != null ? threadFor(work, data.threads, titleOf(document)) : null;
	const conversation = useConversation(matterId, chatId, version);
	const messages = useMemo(() => (data != null ? messagesOf(conversation, data.agents) : []), [conversation, data]);

	if (data == null) return <p className={styles.quiet}>{problem ?? "Opening the Matter…"}</p>;
	// What the thread said about the clause at the cursor, in this document or
	// in another version of the same agreement.
	const family = familyOf(documentId, work);
	const about = clause?.number != null
		? messages.filter(message => message.place != null && family.has(message.place.documentId) && message.place.section == clause.number)
		: [];

	return (
		<div className={styles.pane}>
			<Head data={data} document={document} place={place} work={work} />
			<Decision data={data} work={work} onDecided={reload} />
			<Clause data={data} work={work} clause={clause} about={about.slice(-3)} document={document} documentId={documentId} chatId={chatId} onPosted={reload} />
			<Thread data={data} chatId={chatId} title={conversation?.chatTitle} messages={messages} shownAbove={new Set(about.slice(-3).map(message => message.id))} family={family} />
			{work.kind == "draft" ? <Changes work={work} /> : null}
		</div>
	);
}

// ---- Where the work stands ------------------------------------------------------------

function stateOf(work, data) {
	const agent = data.agents.find(entry => entry.id == work.review?.agentId);
	const name = agent?.name ?? work.review?.agentName ?? "The colleague";
	if (work.kind == "draft") {
		const lines = { title: `${name}'s draft, version ${work.version.number}` };
		// A later version stands for this one: what it waits for is said here.
		const later = laterVersionOf(work);
		if (later != null) {
			if (isUnderway(later)) return { ...lines, line: `Returned: ${name} is working on version ${later.number}`, tone: "underway" };
			if (later.state == "ready_for_review") return { ...lines, line: `Version ${later.number} replaced it and waits for your decision`, tone: "yours" };
			if (later.state == "accepted") return { ...lines, line: `Version ${later.number} replaced it and was accepted`, tone: "settled" };
			if (later.state == "failed") return { ...lines, line: `${name} stopped on version ${later.number}: ${later.stateMessage || "try again"}`, tone: "yours" };
			return { ...lines, line: `Version ${later.number} replaced it`, tone: "quiet" };
		}
		switch (work.version.state) {
			case "ready_for_review": return { ...lines, line: "Waits for your decision", tone: "yours" };
			case "accepted": return { ...lines, line: "Accepted", tone: "settled" };
			case "superseded": return { ...lines, line: "A later version replaced it", tone: "quiet" };
			case "returned": return { ...lines, line: `Returned: ${name} is working on it`, tone: "underway" };
			default: return { ...lines, line: "", tone: "quiet" };
		}
	}
	if (work.kind == "source") {
		const version = work.version;
		if (isUnderway(version)) return { title: "The agreement under review", line: `${name} is working on it`, tone: "underway" };
		if (version?.state == "ready_for_review") return { title: "The agreement under review", line: `${name} handed in version ${version.number}`, tone: "yours" };
		if (version?.state == "accepted") return { title: "The agreement under review", line: `Version ${version.number} was accepted`, tone: "settled" };
		if (version?.state == "failed") return { title: "The agreement under review", line: `${name} stopped: ${version.stateMessage || "try again"}`, tone: "yours" };
		return { title: "The agreement under review", line: "", tone: "quiet" };
	}
	return { title: "No colleague has worked on it yet", line: "Ask one about it below", tone: "quiet" };
}

// The review's latest version, when it is later than the draft open here.
function laterVersionOf(work) {
	if (work.kind != "draft") return null;
	const latest = work.review?.versions?.at(-1) ?? null;
	return latest != null && latest.number > work.version.number ? latest : null;
}

function Head({ data, document, place, work }) {
	const state = stateOf(work, data);
	const working = new Set(data.reviews.filter(review => isUnderway(review.versions?.at(-1))).map(review => review.agentId));
	return (
		<header className={styles.head}>
			<div className={styles.head_row}>
				<MatterMark matter={data.matter} size={32} />
				<div className={styles.head_text}>
					<p className={styles.matter}>{data.matter?.name ?? place.matterName}</p>
					<p className={styles.document}>{document?.displayName ?? place.relativePath}</p>
				</div>
				<div className={styles.faces} aria-label="The colleagues on the Matter">
					{data.agents.map(agent => (
						<span key={agent.id} className={styles.face} title={agent.name}>
							<Face member={colleagueMember(agent)} size={24} presence={presenceOf(agent, working.has(agent.id) || agent.isRunning)} />
						</span>
					))}
				</div>
			</div>
			<div className={styles.state_text}>
				<p className={styles.state_title}>{state.title}</p>
				{state.line != "" ? <p className={`${styles.state_line} ${styles[`tone_${state.tone}`] ?? ""}`}>{state.line}</p> : null}
			</div>
		</header>
	);
}

// ---- The decision the lawyer holds ------------------------------------------------------

function Decision({ data, work, onDecided }) {
	const [isReturning, setReturning] = useState(false);
	const [words, setWords] = useState("");
	const [markup, setMarkup] = useState(null);
	const [withMarkup, setWithMarkup] = useState(true);
	const [busy, setBusy] = useState(false);
	const [problem, setProblem] = useState(null);

	const waiting = work.kind == "draft" && work.version.state == "ready_for_review" && laterVersionOf(work) == null;
	// The version to open: the one the agreement's review handed in, or the
	// later one that replaced the draft open here.
	const later = laterVersionOf(work);
	const handedIn = work.kind == "source" && work.version?.state == "ready_for_review" ? work.version
		: later != null && later.state == "ready_for_review" && later.resultDocumentId ? later : null;
	const agent = data.agents.find(entry => entry.id == work.review?.agentId);
	const name = agent?.name ?? work.review?.agentName ?? "the colleague";

	// The lawyer's own mark-up in the draft: not the colleague's, and not what
	// the agreement carried before (the counterparty's comments and changes).
	useEffect(() => {
		if (!isReturning) return;
		let isCurrent = true;
		Promise.all([markupBy(name), marksOfSource(data.matter?.id, work.review?.sourceDocumentId)]).then(([found, source]) => {
			if (!isCurrent) return;
			setMarkup({
				changes: found.changes.filter(change => !source.authors.has((change.author ?? "").trim().toLowerCase())),
				comments: found.comments.filter(comment => !source.comments.has(markKey(comment.author, comment.text))),
			});
		}, () => { if (isCurrent) setMarkup({ changes: [], comments: [] }); });
		return () => { isCurrent = false; };
	}, [isReturning, name, data.matter?.id, work.review?.sourceDocumentId]);

	if (handedIn != null) {
		return (
			<section className={styles.section}>
				<button className={styles.primary} type="button" onClick={() => openInOffice(data.matter.id, handedIn.resultDocumentId)}>
					Open version {handedIn.number} in Word
				</button>
			</section>
		);
	}
	if (!waiting) return null;

	const marked = (markup?.changes.length ?? 0) + (markup?.comments.length ?? 0);
	const act = async (decision, comment) => {
		setBusy(true);
		setProblem(null);
		try {
			await decide(data.matter.id, work.review.id, work.version.number, decision, comment);
			setReturning(false);
			setWords("");
			onDecided();
		} catch (error) {
			setProblem(error?.userMessage || error?.message || "Singularity did not take the decision.");
		}
		setBusy(false);
	};

	return (
		<section className={styles.section}>
			{isReturning ? (
				<div className={styles.returning}>
					<textarea
						className={styles.field}
						rows={3}
						value={words}
						placeholder={`What should ${name} change?`}
						onChange={event => setWords(event.target.value)}
						autoFocus
					/>
					{marked > 0 ? (
						<label className={styles.check}>
							<input type="checkbox" checked={withMarkup} onChange={event => setWithMarkup(event.target.checked)} />
							<span>With your mark-up in this draft: {phrase(markup.changes.length, "change")}{markup.comments.length > 0 ? ` and ${phrase(markup.comments.length, "comment")}` : ""}</span>
						</label>
					) : null}
					<div className={styles.actions}>
						<button className={styles.primary} type="button" disabled={busy || (words.trim() == "" && !(withMarkup && marked > 0))}
							onClick={() => act("return", returnWords(words, withMarkup ? markup : null))}>
							Return to {name}
						</button>
						<button className={styles.plain} type="button" disabled={busy} onClick={() => setReturning(false)}>Cancel</button>
					</div>
				</div>
			) : (
				<div className={styles.actions}>
					<button className={styles.primary} type="button" disabled={busy} onClick={() => act("accept", "")}>Accept version {work.version.number}</button>
					<button className={styles.plain} type="button" disabled={busy} onClick={() => setReturning(true)}>Return with comments</button>
				</div>
			)}
			{problem != null ? <p className={styles.problem}>{problem}</p> : null}
		</section>
	);
}

function phrase(count, word) {
	return `${count} ${word}${count == 1 ? "" : "s"}`;
}

// The lawyer's words, then their mark-up as the colleague is to read it: each
// change and comment with the words it stands on.
function returnWords(words, markup) {
	const lines = [];
	if (words.trim() != "") lines.push(words.trim());
	if (markup != null) {
		for (const comment of markup.comments) {
			lines.push(`On “${short(comment.anchor)}”, I commented: ${comment.text.trim()}`);
		}
		for (const change of markup.changes) {
			const verb = change.type == "Deleted" ? "deleted" : change.type == "Added" ? "added" : "changed";
			lines.push(`I ${verb} “${short(change.text)}” in the draft.`);
		}
	}
	return lines.join("\n\n");
}

function limited(text, most) {
	return (text ?? "").replace(/\s+/g, " ").trim().slice(0, most);
}

function short(text) {
	const words = (text ?? "").replace(/\s+/g, " ").trim();
	return words.length > 160 ? `${words.slice(0, 157)}…` : words;
}

// ---- The clause at the cursor ---------------------------------------------------------------

function Clause({ data, work, clause, about, document, documentId, chatId, onPosted }) {
	const [words, setWords] = useState("");
	const [agentId, setAgentId] = useState(null);
	const [busy, setBusy] = useState(false);
	const [notice, setNotice] = useState(null);

	// The colleague who worked on this document is asked first.
	const agent = data.takers.find(entry => entry.id == (agentId ?? work.review?.agentId)) ?? deskFor(document, data.takers);
	const changes = work.kind == "draft" ? (work.version.result?.changes ?? []) : [];
	const here = clause?.number != null ? changes.filter(change => change.clauseNumber == clause.number) : [];
	const label = clause?.number != null ? `${clause.number}${clause.heading ? ` ${clause.heading}` : ""}` : null;
	// In a colleague's draft, Word gives a clause's words with the struck and
	// the inserted run together; the clause reads as the backend read it back
	// from the saved draft, with its changes made.
	const readsNow = here.find(change => (change.readBack ?? "") != "")?.readBack ?? null;
	const clauseWords = readsNow ?? clause?.text ?? "";

	const send = async () => {
		if (agent == null || words.trim() == "") return;
		setBusy(true);
		setNotice(null);
		// The clause goes with the words as their place: the colleague reads
		// where it is, and the thread and the window can go back to it.
		const quoted = readsNow ?? ((clause?.selected ?? "") != "" ? clause.selected : clause?.text ?? "");
		const where = clause?.number != null || (clause?.selected ?? "") != ""
			? { documentId, section: clause?.number ?? "", heading: clause?.heading ?? "", quote: limited(quoted, 600) }
			: null;
		// A document no one has reviewed is handed over with the words; one
		// under review is the thread's already.
		const handed = work.kind == "none" ? [documentId] : [];
		try {
			await postToMatter(data.matter.id, chatId, `@${agent.name} ${words.trim()}`, [agent.id], handed, titleOf(document), where);
			setWords("");
			setNotice(`${agent.name} has it, in the Matter's thread.`);
			onPosted();
		} catch (error) {
			setNotice(error?.userMessage || error?.message || "The message did not reach the thread.");
		}
		setBusy(false);
	};

	return (
		<section className={styles.section}>
			{label != null ? <p className={styles.clause}>{label}</p> : <p className={styles.clause_quiet}>Put the cursor in a clause</p>}
			{label != null && clauseWords != "" ? <p className={styles.clause_text}>{short(clauseWords)}</p> : null}
			{here.map((change, index) => (
				<div key={index} className={styles.reason}>
					<p className={styles.reason_who}>{work.review?.agentName ?? "The colleague"} {change.writeState == "written" ? "changed it" : "meant to change it, and could not"}</p>
					<p className={styles.reason_text}>{change.explanation}</p>
				</div>
			))}
			{about.length > 0 ? (
				<ol className={styles.messages} aria-label={`Said about ${clause.number}`}>
					{about.map(message => <Message key={message.id} data={data} message={message} />)}
				</ol>
			) : null}
			<div className={styles.composer}>
				<textarea
					className={styles.field}
					rows={2}
					value={words}
					placeholder={agent == null ? "No colleague works on this Matter" : label != null ? `Ask ${agent.name} about ${clause.number}…` : `Write to ${agent.name}…`}
					disabled={agent == null || busy}
					onChange={event => setWords(event.target.value)}
					onKeyDown={event => {
						if (event.key == "Enter" && (event.ctrlKey || event.metaKey)) send();
					}}
				/>
				{/* Whom it goes to, beside the send: the colleagues' faces. */}
				<div className={styles.composer_foot}>
					<div className={styles.to} role="group" aria-label="To">
						{data.takers.map(entry => (
							<button
								key={entry.id}
								type="button"
								className={entry.id == agent?.id ? `${styles.to_face} ${styles.to_face_on}` : styles.to_face}
								title={`To ${entry.name}`}
								aria-pressed={entry.id == agent?.id}
								onClick={() => setAgentId(entry.id)}
							>
								<Face member={colleagueMember(entry)} size={20} />
							</button>
						))}
					</div>
					<button className={styles.primary} type="button" disabled={busy || agent == null || words.trim() == ""} onClick={send}>Send</button>
				</div>
			</div>
			{notice != null ? <p className={styles.notice}>{notice}</p> : null}
		</section>
	);
}

// ---- The thread -------------------------------------------------------------------------------

// The thread's latest messages - not those standing at the clause above - and
// all of them on a press, in order. A message about a clause names it; a press
// on the name goes to the clause in this document.
function Thread({ data, chatId, title, messages, shownAbove, family }) {
	const [isOpen, setOpen] = useState(false);
	if (chatId == null) return null;
	const thread = data.threads.find(entry => entry.id == chatId);
	const rest = messages.filter(message => !shownAbove.has(message.id));
	const shown = isOpen ? rest : rest.slice(-3);
	const earlier = rest.length - shown.length;
	return (
		<section className={styles.section}>
			<p className={styles.section_title}>{title || thread?.title || "Thread"}</p>
			{messages.length == 0 ? <p className={styles.quiet}>No messages yet.</p> : null}
			{messages.length > 0 && rest.length == 0 ? <p className={styles.quiet}>Its messages are about the clause above.</p> : null}
			{earlier > 0 ? (
				<button className={styles.link} type="button" onClick={() => setOpen(true)}>
					{earlier == 1 ? "Show the earlier message" : `Show ${earlier} earlier messages`}
				</button>
			) : null}
			<ol className={styles.messages}>
				{shown.map(message => <Message key={message.id} data={data} message={message} family={family} />)}
			</ol>
		</section>
	);
}

// A message as the pane shows it. With `family`, the clause it is about stands
// over its words: a press goes there in this document.
function Message({ data, message, family = null }) {
	const place = family != null ? message.place : null;
	const isHere = place != null && family.has(place.documentId);
	const placed = place == null ? null : placeWords(place, isHere ? null : data.documents.find(entry => entry.id == place.documentId));
	return (
		<li className={styles.message}>
			<span className={styles.message_face}>
				<Face member={message.agent != null ? colleagueMember(message.agent) : personMember(message.isViewer ? (data.viewer?.name ?? "You") : message.authorName)} size={24} />
			</span>
			<div className={styles.message_body}>
				<p className={styles.message_head}>
					<span className={styles.message_author}>{message.isViewer ? "You" : message.authorName}</span>
					<time className={styles.message_time} dateTime={message.at}>{when(message.at)}</time>
				</p>
				{placed != null ? (
					<button className={styles.message_place} type="button" disabled={!isHere} title={isHere ? "Show it in the document" : undefined}
						onClick={() => revealPlace(place).catch(() => {})}>
						{placed}
					</button>
				) : null}
				<p className={styles.message_text}>{message.text}</p>
			</div>
		</li>
	);
}

// "2.1 Purpose", or "6.1 in Harborline Mutual NDA" for another document.
function placeWords(place, otherDocument) {
	const where = place.section != "" ? `${place.section}${place.heading ? ` ${place.heading}` : ""}` : `“${short(place.quote).slice(0, 48)}”`;
	return otherDocument != null ? `${where} in ${titleOf(otherDocument)}` : where;
}

// ---- The version's changes ----------------------------------------------------------------------

function Changes({ work }) {
	const [isOpen, setOpen] = useState(false);
	const changes = work.version.result?.changes ?? [];
	const written = changes.filter(change => change.writeState == "written" || change.writeState == null);
	const notWritten = changes.length - written.length;
	if (changes.length == 0) return null;
	return (
		<section className={styles.section}>
			<button className={styles.fold} type="button" aria-expanded={isOpen} onClick={() => setOpen(!isOpen)}>
				<span>{phrase(written.length, "change")} in this version{notWritten > 0 ? `, ${notWritten} not written` : ""}</span>
				<span className={styles.fold_mark} aria-hidden="true">{isOpen ? "−" : "+"}</span>
			</button>
			{isOpen ? (
				<ol className={styles.changes}>
					{changes.map((change, index) => (
						<li key={index}>
							<button
								type="button"
								className={styles.change}
								onClick={() => showInDocument(change.edits?.find(edit => (edit.proposedText ?? "").trim() != "")?.proposedText ?? change.currentText)}
							>
								<span className={styles.change_clause}>{change.clauseNumber || ""}</span>
								<span className={styles.change_text}>{change.explanation}</span>
							</button>
						</li>
					))}
				</ol>
			) : null}
		</section>
	);
}
