import { useCallback, useEffect, useMemo, useState } from "react";

import {
	colleagueEmailDrafts, documentAt, fileLetter, lettersOf, matterSummaries, moveLetter, postToMatter, readEmailDraft, saveAttachment,
	deskFor, suggestMatter, threadFor, titleOf,
	useMatter, workOn,
} from "./data.js";
import { attachToLetter, attachmentBase64, composedLetter, hasMailbox, letterMode, onLetterChanged, readLetter, writeIntoLetter } from "./mail.js";
import { checksOf, dayWords, versionsOf } from "./letterChecks.js";
import { colleagueMember, Face, MatterMark } from "./people.jsx";

import styles from "./pane.module.css";

const native = () => window.nativeApp;

// Singularity in Outlook: the Matter of the letter open now. A letter read
// is filed to its Matter on the evidence Singularity shows - never by guess
// - and its attachments go to the Matter and to a colleague from there. A
// reply being written gets the version the Matter holds now, and is checked
// before the person sends it with Outlook's own Send. Singularity sends
// nothing.
export function OutlookPane() {
	const [mode, setMode] = useState(letterMode());
	const [turn, setTurn] = useState(0);

	// A pinned pane follows the letter the person selects.
	useEffect(() => onLetterChanged(() => {
		setMode(letterMode());
		setTurn(value => value + 1);
	}), []);

	if (!hasMailbox("1.3")) return <p className={styles.quiet}>This Outlook is older than Singularity's pane needs.</p>;
	if (mode == "read") return <ReadLetter key={turn} />;
	if (mode == "compose") return <ComposeLetter key={turn} />;
	return <p className={styles.quiet}>Select a letter, and its Matter opens here.</p>;
}

// ---- A letter read ------------------------------------------------------------------------

function ReadLetter() {
	const [letter, setLetter] = useState(null);
	const [where, setWhere] = useState(null);
	const [problem, setProblem] = useState(null);
	const [version, setVersion] = useState(0);

	useEffect(() => {
		readLetter().then(setLetter, error => setProblem(error?.message || "Outlook did not give the letter."));
	}, []);

	useEffect(() => {
		if (letter == null) return undefined;
		let isCurrent = true;
		suggestMatter(letter).then(
			found => { if (isCurrent) setWhere(found); },
			error => { if (isCurrent) setProblem(error?.userMessage || error?.message || "Singularity did not answer."); },
		);
		return () => { isCurrent = false; };
	}, [letter, version]);

	useEffect(() => {
		const off = native()?.on?.("matter.changed", () => setVersion(value => value + 1));
		return () => { if (typeof off == "function") off(); };
	}, []);

	// One Matter's intake files it by a rule: filed as Outlook shows it, with no
	// press, and then read as filed.
	useEffect(() => {
		if (letter == null || where?.outcome != "rule") return;
		const matter = where.matters[0];
		fileLetter(matter.matterId, letter, matter.evidence ?? [], true).then(
			() => setVersion(value => value + 1),
			error => setProblem(error?.userMessage || error?.message || "Singularity did not file it."),
		);
	}, [letter, where]);

	if (problem != null) return <p className={styles.problem}>{problem}</p>;
	if (letter == null || where == null) return <p className={styles.quiet}>Reading the letter…</p>;
	if (where.outcome == "rule") return <p className={styles.quiet}>Filing it in {where.matters[0]?.matterName} by its intake…</p>;
	const filed = where.outcome == "filed" ? where.matters[0] : null;

	return (
		<div className={styles.pane}>
			<LetterHead letter={letter} />
			<Filing letter={letter} where={where} onChanged={() => setVersion(value => value + 1)} />
			{filed != null ? <WorkOnLetter letter={letter} filed={filed} /> : null}
		</div>
	);
}

function LetterHead({ letter }) {
	const from = letter.from?.name || letter.from?.address || "Someone";
	const isOwn = (letter.from?.address ?? "").toLowerCase() == (letter.mailbox ?? "").toLowerCase();
	return (
		<header className={styles.head}>
			<div className={styles.state_text}>
				<p className={styles.state_title}>{letter.subject || "A letter with no subject"}</p>
				<p className={styles.state_line}>{isOwn ? `You wrote it, ${dayWords(letter.sentAt)}` : `From ${from}, ${dayWords(letter.sentAt)}`}</p>
			</div>
		</header>
	);
}

// Where the letter belongs: filed, suggested, ambiguous or not yet filed, with
// the evidence for each, and the one press that files it.
function Filing({ letter, where, onChanged }) {
	const [isChoosing, setChoosing] = useState(false);
	const [busy, setBusy] = useState(false);
	const [problem, setProblem] = useState(null);

	const act = async work => {
		setBusy(true);
		setProblem(null);
		try {
			await work();
			setChoosing(false);
			onChanged();
		} catch (error) {
			setProblem(error?.userMessage || error?.message || "Singularity did not file it.");
		}
		setBusy(false);
	};
	const file = (matter, evidence) => act(() => fileLetter(matter.matterId ?? matter.id, letter, evidence ?? []));

	const filed = where.outcome == "filed" ? where.matters[0] : null;
	const first = where.matters[0] ?? null;

	return (
		<section className={styles.section}>
			{filed != null ? (
				<>
					<div className={styles.filed}>
						<p className={`${styles.state_title} ${styles.tone_settled}`}>Filed in {filed.matterName}</p>
						<p className={styles.evidence}>{filed.evidence[0]}</p>
					</div>
					{isChoosing ? null : <button className={styles.link} type="button" onClick={() => setChoosing(true)}>Move to another Matter</button>}
				</>
			) : null}
			{where.outcome == "suggested" && first != null ? (
				<>
					<p className={styles.state_title}>Looks like {first.matterName}.</p>
					<Evidence lines={first.evidence} />
					<div className={styles.actions}>
						<button className={styles.primary} type="button" disabled={busy} onClick={() => file(first, first.evidence)}>File in {first.matterName}</button>
						{isChoosing ? null : <button className={styles.plain} type="button" disabled={busy} onClick={() => setChoosing(true)}>Another Matter</button>}
					</div>
				</>
			) : null}
			{where.outcome == "ambiguous" ? (
				<>
					<p className={styles.state_title}>Could be {listWords(where.matters.map(matter => matter.matterName))}.</p>
					<p className={styles.evidence}>Singularity does not choose between them for you.</p>
					{where.matters.map(matter => (
						<div key={matter.matterId} className={styles.choice}>
							<Evidence lines={matter.evidence} />
							<button className={styles.plain} type="button" disabled={busy} onClick={() => file(matter, matter.evidence)}>File in {matter.matterName}</button>
						</div>
					))}
				</>
			) : null}
			{where.outcome == "unfiled" ? (
				<>
					<p className={styles.state_title}>Not in a Matter yet.</p>
					<p className={styles.evidence}>Nothing in it points to one of your Matters.</p>
					{isChoosing ? null : <button className={styles.primary} type="button" onClick={() => setChoosing(true)}>Choose a Matter</button>}
				</>
			) : null}
			{isChoosing ? (
				<MatterChoice
					except={filed?.matterId}
					busy={busy}
					onChoose={matter => (filed != null ? act(() => moveLetter(filed.matterId, filed.letterId, matter.id)) : file(matter, []))}
					onCancel={() => setChoosing(false)}
				/>
			) : null}
			{problem != null ? <p className={styles.problem}>{problem}</p> : null}
		</section>
	);
}

function Evidence({ lines }) {
	return (
		<ul className={styles.evidence_list}>
			{lines.map(line => <li key={line} className={styles.evidence}>{line}</li>)}
		</ul>
	);
}

// The Matters open on this computer, to choose one by the person's own decision.
function MatterChoice({ except, busy, onChoose, onCancel }) {
	const [matters, setMatters] = useState(null);
	useEffect(() => {
		matterSummaries().then(setMatters, () => setMatters([]));
	}, []);
	if (matters == null) return <p className={styles.quiet}>Reading your Matters…</p>;
	const shown = matters.filter(matter => matter.id != except);
	return (
		<div className={styles.matter_choice}>
			{shown.length == 0 ? <p className={styles.quiet}>No other Matter is open on this computer.</p> : null}
			{shown.map(matter => (
				<button key={matter.id} className={styles.matter_row} type="button" disabled={busy} onClick={() => onChoose(matter)}>
					<MatterMark matter={matter} size={22} />
					<span>{matter.name}</span>
				</button>
			))}
			<button className={styles.link} type="button" onClick={onCancel}>Cancel</button>
		</div>
	);
}

// A filed letter's work: its attachments to the Matter, and a word to a
// colleague about it - with the attachments chosen handed over - in the
// Matter's thread, which the other side never sees.
function WorkOnLetter({ letter, filed }) {
	const { data } = useMatter(filed.matterId);
	const [kept, setKept] = useState(null);
	const [chosen, setChosen] = useState(() => new Set(letter.attachments.map(attachment => attachment.providerId)));
	const [words, setWords] = useState("");
	const [agentId, setAgentId] = useState(null);
	const [busy, setBusy] = useState(false);
	const [notice, setNotice] = useState(null);

	// The letter as the Matter keeps it: which attachments are saved, where.
	const readKept = useCallback(() => {
		lettersOf(filed.matterId).then(letters => setKept(letters.find(entry => entry.id == filed.letterId) ?? null), () => {});
	}, [filed.matterId, filed.letterId]);
	useEffect(() => { readKept(); }, [readKept, data]);

	if (data == null) return <p className={styles.quiet}>Opening {filed.matterName}…</p>;
	// The colleague the letter goes to: the one chosen, else the one whose brief
	// names what the letter is about, among those who take work now.
	const agent = data.takers.find(entry => entry.id == agentId) ?? deskFor({ displayName: letter.subject ?? "" }, data.takers);
	const savedOf = providerId => kept?.attachments.find(entry => entry.providerId == providerId && (entry.relativePath ?? "") != "") ?? null;

	// Saves an attachment once; the same bytes the Matter has are that file.
	const saved = async attachment => {
		const already = savedOf(attachment.providerId);
		if (already != null) return already;
		const bytes = await attachmentBase64(attachment.providerId);
		return saveAttachment(filed.matterId, filed.letterId, attachment.providerId, bytes);
	};

	const saveOne = async attachment => {
		setBusy(true);
		setNotice(null);
		try {
			const result = await saved(attachment);
			setNotice(result.wasAlreadyThere ? `${filed.matterName} already had it, as ${result.relativePath}.` : `Saved in ${filed.matterName} as ${result.relativePath}.`);
			readKept();
		} catch (error) {
			setNotice(error?.userMessage || error?.message || "The attachment was not saved.");
		}
		setBusy(false);
	};

	const send = async () => {
		if (agent == null || words.trim() == "") return;
		setBusy(true);
		setNotice(null);
		try {
			const handed = [];
			for (const attachment of letter.attachments.filter(entry => chosen.has(entry.providerId))) {
				const result = await saved(attachment);
				const document = await documentAt(filed.matterId, result.relativePath);
				if (document == null) throw new Error(`Singularity is still reading ${attachment.name}. Send it again in a moment.`);
				handed.push(document);
			}
			readKept();
			// The thread of the document handed over, as in Word; else one named
			// after the letter.
			const first = handed[0] ?? null;
			const title = first != null ? titleOf(first) : letter.subject || "A letter";
			const chatId = first != null ? threadFor(workOn(first.id, data.reviews), data.threads, title)
				: data.threads.find(thread => (thread.title ?? "").trim().toLowerCase() == title.trim().toLowerCase())?.id ?? null;
			const from = letter.from?.name || letter.from?.address || "someone";
			const about = `It came with ${from}'s letter of ${dayWords(letter.sentAt)}, “${letter.subject || "no subject"}”.`;
			await postToMatter(filed.matterId, chatId, `@${agent.name} ${words.trim()}\n\n${about}`, [agent.id], handed.map(document => document.id), title, null);
			setWords("");
			setNotice(`${agent.name} has it, in ${filed.matterName}'s thread. The other side sees nothing of it.`);
		} catch (error) {
			setNotice(error?.userMessage || error?.message || "The message did not reach the thread.");
		}
		setBusy(false);
	};

	return (
		<>
			{letter.attachments.length > 0 ? (
				<section className={styles.section}>
					<p className={styles.section_title}>Attachments</p>
					<ul className={styles.attachments}>
						{letter.attachments.map(attachment => {
							const kept = savedOf(attachment.providerId);
							return (
								<li key={attachment.providerId} className={styles.attachment}>
									<label className={styles.check}>
										<input
											type="checkbox"
											checked={chosen.has(attachment.providerId)}
											onChange={event => {
												const next = new Set(chosen);
												if (event.target.checked) next.add(attachment.providerId);
												else next.delete(attachment.providerId);
												setChosen(next);
											}}
										/>
										<span className={styles.attachment_name}>{attachment.name}</span>
									</label>
									{kept != null ? (
										<p className={styles.evidence}>{kept.wasAlreadyThere ? `${filed.matterName} already had it, as ${kept.relativePath}` : `In ${filed.matterName} as ${kept.relativePath}`}</p>
									) : (
										<button className={styles.link} type="button" disabled={busy} onClick={() => saveOne(attachment)}>Save to {filed.matterName}</button>
									)}
								</li>
							);
						})}
					</ul>
				</section>
			) : null}
			<section className={styles.section}>
				<p className={styles.section_title}>To a colleague, in {filed.matterName}</p>
				<div className={styles.composer}>
					<textarea
						className={styles.field}
						rows={2}
						value={words}
						placeholder={agent == null ? "No colleague works on this Matter" : chosen.size > 0 ? `What should ${agent.name} do with it?` : `Write to ${agent.name} about this letter`}
						disabled={agent == null || busy}
						onChange={event => setWords(event.target.value)}
						onKeyDown={event => {
							if (event.key == "Enter" && (event.ctrlKey || event.metaKey)) send();
						}}
					/>
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
		</>
	);
}

// ---- A letter being written -------------------------------------------------------------------

function ComposeLetter() {
	const [draft, setDraft] = useState(null);
	const [where, setWhere] = useState(null);
	const [letters, setLetters] = useState([]);
	const [notice, setNotice] = useState(null);
	const [busy, setBusy] = useState(false);
	const [turn, setTurn] = useState(0);
	// The email drafts colleagues prepared, which a press puts into this letter.
	const [drafts, setDrafts] = useState([]);

	useEffect(() => {
		let isCurrent = true;
		colleagueEmailDrafts().then(found => { if (isCurrent) setDrafts(found); }, () => { if (isCurrent) setDrafts([]); });
		return () => { isCurrent = false; };
	}, []);

	useEffect(() => {
		let isCurrent = true;
		composedLetter().then(async read => {
			if (!isCurrent) return;
			setDraft(read);
			const found = await suggestMatter({ ...read, internetMessageId: "", from: { name: "", address: read.mailbox } }).catch(() => null);
			if (isCurrent) setWhere(found);
		}, () => setNotice("Outlook did not give the letter being written."));
		return () => { isCurrent = false; };
	}, [turn]);

	const matter = where != null && where.outcome != "unfiled" && where.outcome != "ambiguous" ? where.matters[0] : null;
	const { data } = useMatter(matter?.matterId ?? null);
	useEffect(() => {
		if (matter == null) return;
		lettersOf(matter.matterId).then(setLetters, () => setLetters([]));
	}, [matter?.matterId]);

	const versions = useMemo(() => (data != null ? versionsOf(data) : []), [data]);
	const checks = useMemo(() => (draft != null && data != null ? checksOf(draft, letters, versions, matter?.matterName) : []), [draft, data, letters, versions, matter]);

	if (draft == null) return <p className={styles.quiet}>{notice ?? "Reading the letter…"}</p>;

	const putDraft = async found => {
		setBusy(true);
		setNotice(null);
		try {
			const draft = await readEmailDraft(found.matterId, found.documentId);
			await writeIntoLetter(draft);
			const count = (draft.attachments ?? []).length;
			setNotice(`${found.agentName || "The colleague"}'s draft is in this letter: its subject, its words${count > 0 ? ` and ${count == 1 ? "1 attachment" : `${count} attachments`}` : ""}. ${found.isReady ? "" : `${found.readiness}. `}Nothing is sent until you press Send in Outlook.`);
			setTurn(value => value + 1);
		} catch (error) {
			setNotice(error?.userMessage || error?.message || "The draft could not be put into this letter.");
		}
		setBusy(false);
	};

	const attach = async version => {
		setBusy(true);
		setNotice(null);
		try {
			const content = await native().readDocumentContent(matter.matterId, version.documentId);
			await attachToLetter(content.name, content.base64);
			setNotice(`${content.name} is attached. Nothing is sent until you press Send in Outlook.`);
			setTurn(value => value + 1);
		} catch (error) {
			setNotice(error?.userMessage || error?.message || "It was not attached.");
		}
		setBusy(false);
	};

	return (
		<div className={styles.pane}>
			<header className={styles.head}>
				<div className={styles.head_row}>
					{data?.matter != null ? <MatterMark matter={data.matter} size={32} /> : null}
					<div className={styles.head_text}>
						<p className={styles.matter}>{matter != null ? matter.matterName : "Not in a Matter yet"}</p>
						<p className={styles.document}>{draft.subject || "A letter with no subject"}</p>
					</div>
				</div>
				{matter == null ? (
					<p className={styles.state_line}>
						{where?.outcome == "ambiguous" ? `This conversation could be ${listWords(where.matters.map(entry => entry.matterName))}.` : "Nothing in this conversation is filed yet."}
					</p>
				) : null}
			</header>
			{drafts.length > 0 ? (
				<section className={styles.section}>
					<p className={styles.section_title}>Drafts your colleagues prepared</p>
					{drafts.map(found => (
						<div key={found.documentId} className={styles.version_row}>
							<div className={styles.version_text}>
								<p className={styles.version_words}>{`${found.agentName}: ${found.subject || "an email"}`}</p>
								<p className={found.isReady ? styles.evidence : styles.warn}>{found.readiness}</p>
							</div>
							<button className={styles.plain} type="button" disabled={busy} onClick={() => putDraft(found)}>Use it</button>
						</div>
					))}
				</section>
			) : null}
			{matter != null && versions.length > 0 ? (
				<section className={styles.section}>
					<p className={styles.section_title}>The version {matter.matterName} holds now</p>
					{versions.map(version => (
						<div key={version.documentId} className={styles.version_row}>
							<p className={styles.version_words}>{version.words}</p>
							<button className={styles.plain} type="button" disabled={busy} onClick={() => attach(version)}>Attach</button>
						</div>
					))}
				</section>
			) : null}
			{matter != null ? (
				<section className={styles.section}>
					<p className={styles.section_title}>Before you send</p>
					{checks.length == 0 ? <p className={styles.evidence}>Nothing to raise: the attachments are current and everyone has written in {matter.matterName} before.</p> : null}
					<ul className={styles.evidence_list}>
						{checks.map(check => <li key={check.words} className={check.tone == "warn" ? styles.warn : styles.evidence}>{check.words}</li>)}
					</ul>
					<button className={styles.link} type="button" onClick={() => setTurn(value => value + 1)}>Check again</button>
				</section>
			) : null}
			<section className={styles.section}>
				<p className={styles.evidence}>You send it with Outlook's own Send; Singularity sends nothing. Once it is sent, open it in Sent Items to file it{matter != null ? ` in ${matter.matterName}` : ""}.</p>
				{notice != null ? <p className={styles.notice}>{notice}</p> : null}
			</section>
		</div>
	);
}

function listWords(items) {
	if (items.length <= 1) return items.join("");
	return `${items.slice(0, -1).join(", ")} or ${items[items.length - 1]}`;
}
