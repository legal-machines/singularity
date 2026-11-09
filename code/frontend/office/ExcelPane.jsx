import { useEffect, useMemo, useState } from "react";

import { deskFor, messagesOf, postToMatter, rangesMeet, threadFor, titleOf, useConversation, useMatter, useReveal } from "./data.js";
import { cellsAtSelection, onSelectionChanged, selectCells } from "./host.js";
import { colleagueMember, Face, MatterMark, personMember, presenceOf } from "./people.jsx";
import { when } from "./words.js";

import styles from "./pane.module.css";

// Singularity in Excel: the Matter of the workbook the lawyer is in, at the
// cells selected. The workbook stays in Excel; the Matter, its thread and its
// colleagues belong to Singularity, and the pane shows only what concerns this
// workbook and the cells selected.
//   - the cells, what they hold, what the thread said about them, and a word
//     to a colleague about them, which goes to the Matter's thread like any
//     message, with the cells - their sheet, address and what they hold - as
//     its place;
//   - the workbook's thread, each message with the cells it is about; a press
//     selects them.
// A colleague answers in the thread. Nothing is written into the workbook from
// here: its formulas stay as the lawyer keeps them.
// Cells asked from Singularity's window ("Show in Excel") are selected.
export function ExcelPane({ place, reveal }) {
	const matterId = place.matterId;
	const documentId = place.documentId;
	const { data, problem, reload } = useMatter(matterId);
	const [cells, setCells] = useState(null);
	const [version, setVersion] = useState(0);

	useReveal(documentId, reveal, request => {
		selectCells(request.heading, request.section).catch(() => {});
	});

	// The cells follow the selection.
	useEffect(() => {
		let isCurrent = true;
		let timer = null;
		const read = () => {
			clearTimeout(timer);
			timer = setTimeout(() => {
				cellsAtSelection().then(found => { if (isCurrent) setCells(found); }, () => {});
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
	const chatId = data != null ? threadFor({ review: null }, data.threads, titleOf(document)) : null;
	const conversation = useConversation(matterId, chatId, version);
	const messages = useMemo(() => (data != null ? messagesOf(conversation, data.agents) : []), [conversation, data]);

	if (data == null) return <p className={styles.quiet}>{problem ?? "Opening the Matter…"}</p>;
	// What the thread said about cells that share one with the selection.
	const about = cells != null
		? messages.filter(message => message.place?.documentId == documentId && (message.place.heading ?? "") == cells.sheet
			&& rangesMeet(message.place.section, cells.address))
		: [];

	return (
		<div className={styles.pane}>
			<Head data={data} document={document} place={place} messages={messages} />
			<Cells data={data} cells={cells} about={about.slice(-3)} document={document} documentId={documentId} chatId={chatId} onPosted={reload} />
			<Thread data={data} chatId={chatId} title={conversation?.chatTitle} messages={messages}
				shownAbove={new Set(about.slice(-3).map(message => message.id))} documentId={documentId} />
		</div>
	);
}

// ---- Where the work stands ------------------------------------------------------------

// A colleague named in the thread's last message, still at it.
function stateOf(data, messages) {
	const last = messages.at(-1) ?? null;
	const working = last != null && last.agent == null
		? data.agents.find(agent => agent.isRunning && (last.text ?? "").includes(`@${agent.name}`)) ?? null
		: null;
	if (working != null) return { title: `${working.name} is looking into it`, line: "The answer comes in the thread below", tone: "underway" };
	if (messages.length == 0) return { title: "No colleague has worked on it yet", line: "Select cells and ask one about them below", tone: "quiet" };
	return { title: "Select cells to see what was said about them", line: "", tone: "quiet" };
}

function Head({ data, document, place, messages }) {
	const state = stateOf(data, messages);
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
							<Face member={colleagueMember(agent)} size={24} presence={presenceOf(agent, agent.isRunning)} />
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

// ---- The cells selected ---------------------------------------------------------------------

// "Cell G2, Contracts" or "Cells A2:M2, Contracts".
function cellsLabel(address, sheet) {
	const noun = (address ?? "").includes(":") ? "Cells" : "Cell";
	return `${noun} ${address}${sheet ? `, ${sheet}` : ""}`;
}

function short(text) {
	const words = (text ?? "").replace(/\s+/g, " ").trim();
	return words.length > 160 ? `${words.slice(0, 157)}…` : words;
}

function Cells({ data, cells, about, document, documentId, chatId, onPosted }) {
	const [words, setWords] = useState("");
	const [agentId, setAgentId] = useState(null);
	const [busy, setBusy] = useState(false);
	const [notice, setNotice] = useState(null);

	const agent = data.takers.find(entry => entry.id == agentId) ?? deskFor(document, data.takers);
	const label = cells != null ? cellsLabel(cells.address, cells.sheet) : null;

	const send = async () => {
		if (agent == null || words.trim() == "") return;
		setBusy(true);
		setNotice(null);
		// The cells go with the words as their place: the colleague reads which
		// they are and what they hold, and the thread and the window can go back
		// to them.
		const where = cells != null ? { documentId, section: cells.address, heading: cells.sheet, quote: cells.reading } : null;
		try {
			await postToMatter(data.matter.id, chatId, `@${agent.name} ${words.trim()}`, [agent.id], [], titleOf(document), where);
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
			{label != null ? <p className={styles.clause}>{label}</p> : <p className={styles.clause_quiet}>Select the cells to ask about</p>}
			{cells != null && cells.reading != "" ? <p className={styles.clause_text}>{short(cells.reading)}</p> : null}
			{cells != null && cells.reading == "" ? <p className={styles.clause_text}>These cells are empty.</p> : null}
			{about.length > 0 ? (
				<ol className={styles.messages} aria-label={`Said about ${label}`}>
					{about.map(message => <Message key={message.id} data={data} message={message} documentId={documentId} />)}
				</ol>
			) : null}
			<div className={styles.composer}>
				<textarea
					className={styles.field}
					rows={2}
					value={words}
					placeholder={agent == null ? "No colleague works on this Matter" : cells != null ? `Ask ${agent.name} about ${cells.address}…` : `Write to ${agent.name}…`}
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

// The workbook's thread: its latest messages - not those standing at the cells
// above - and all of them on a press, in order.
function Thread({ data, chatId, title, messages, shownAbove, documentId }) {
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
			{messages.length > 0 && rest.length == 0 ? <p className={styles.quiet}>Its messages are about the cells above.</p> : null}
			{earlier > 0 ? (
				<button className={styles.link} type="button" onClick={() => setOpen(true)}>
					{earlier == 1 ? "Show the earlier message" : `Show ${earlier} earlier messages`}
				</button>
			) : null}
			<ol className={styles.messages}>
				{shown.map(message => <Message key={message.id} data={data} message={message} documentId={documentId} />)}
			</ol>
		</section>
	);
}

// A message as the pane shows it, with the cells it is about over its words:
// a press selects them in this workbook.
function Message({ data, message, documentId }) {
	const place = message.place;
	const isHere = place != null && place.documentId == documentId;
	const other = place != null && !isHere ? data.documents.find(entry => entry.id == place.documentId) : null;
	const placed = place == null ? null
		: isHere ? cellsLabel(place.section, place.heading)
		: `${place.section ? `${place.section}${place.heading ? ` ${place.heading}` : ""}` : "A passage"} in ${titleOf(other) || "another document"}`;
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
					<button className={styles.message_place} type="button" disabled={!isHere} title={isHere ? "Select them in the workbook" : undefined}
						onClick={() => selectCells(place.heading, place.section).catch(() => {})}>
						{placed}
					</button>
				) : null}
				<p className={styles.message_text}>{message.text}</p>
			</div>
		</li>
	);
}
