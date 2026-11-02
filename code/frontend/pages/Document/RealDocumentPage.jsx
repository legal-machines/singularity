import { useEffect, useState } from "react";

import { Button } from "../../components/Button/Button.jsx";
import { Dialog } from "../../components/Dialog/Dialog.jsx";
import { TextArea } from "../../components/Field/Field.jsx";
import { familyOf, FileKindIcon, kindOf } from "../../components/FileKind/FileKind.jsx";
import { useNavigation } from "../../components/Navigation/navigation.js";
import { Page, PageHead } from "../../components/Page/Page.jsx";
import { SheetBar } from "../../components/SheetBar/SheetBar.jsx";
import { Status, StatusMark } from "../../components/Status/Status.jsx";
import { toast } from "../../components/Toast/toast.js";
import { ReviewDetails } from "../../components/Review/Review.jsx";
import { Row, Rows, Tray } from "../../components/Tray/Tray.jsx";
import { decideReview, leaveOutChanges, openInWord, readDocumentView, realDocument, realWork, removeDocumentFromMatter, reviewOfDocument, saveReviewCopy, showPlace, threadOfChat, useRealWork, versionStatus } from "../../data/native.js";
import { findMatter } from "../words.js";

import styles from "./DocumentPage.module.css";

// A document of the real Matter as the lawyer reads it: the Word file itself,
// read fresh from the Matter's folder (matter.readDocumentView), its tracked
// changes in place, struck and inserted, by whom; its margin comments beside
// it; and, for a colleague's revised draft, the review it belongs to - its
// changes with their reasons, and its versions. It opens in Word as it is.
// Opened at a place (`at`: a section, the words there), it stands at it; a
// change in its side goes to its place, in Word where it is connected.
export function RealDocumentPage({ documentId, at = null }) {
	const { open } = useNavigation();
	useRealWork();
	const document = realDocument(documentId);
	const found = reviewOfDocument(documentId);
	const [view, setView] = useState(null);
	const [problem, setProblem] = useState(null);
	// The lawyer's decision on the version waiting (D-119): the changes they
	// leave out, a return's comments, and whether it is being said.
	const [leftOut, setLeftOut] = useState([]);
	const [isReturning, setReturning] = useState(false);
	const [comment, setComment] = useState("");
	const [isDeciding, setDeciding] = useState(false);
	// Taking the document out of the Matter, asked once more before it goes.
	const [isAskingRemove, setAskingRemove] = useState(false);
	const [isRemoving, setRemoving] = useState(false);

	useEffect(() => {
		let isCurrent = true;
		setProblem(null);
		readDocumentView(documentId).then(
			read => { if (isCurrent) setView(read); },
			error => { if (isCurrent) setProblem(error?.message ?? "The document could not be read."); },
		);
		return () => { isCurrent = false; };
	}, [documentId, document?.version]);

	const name = view?.documentName ?? document?.displayName ?? "Document";
	// A document whose file is not on this computer says where it is, and
	// offers no program to open it in (UI-4).
	const elsewhere = document?.processingState == "unavailable" ? (document.attentionDetail ?? "The file is not on this computer.") : null;
	const matter = findMatter(realWork().uiMatterId);
	const thread = found != null ? threadOfChat(found.review.chatId) : null;
	const changes = (found?.version.result?.changes ?? []).filter(change => change.writeState == "written");
	// What the review proposed that the draft does not carry, and why.
	const unwritten = (found?.version.result?.changes ?? []).filter(change => change.writeState != "written");
	const topics = Object.fromEntries((found?.version.result?.assessments ?? []).map(item => [item.positionId, item.topic]));
	const status = found != null ? versionStatus(found.review, found.version) : null;
	const focus = focusOf(view?.blocks ?? [], at);
	// What the file is decides how its changes are named and where it opens: a
	// Word agreement by its Sections, a workbook by its cells, a deck by its
	// slides, a PDF by its pages (D-115).
	const family = familyOf(kindOf(name));
	const application = family == "sheet" ? "Excel" : family == "slides" ? "PowerPoint" : family == "word" ? "Word" : null;
	const changeWords = family == "word" ? `tracked ${changes.length == 1 ? "change" : "changes"}`
		: family == "sheet" ? `changed ${changes.length == 1 ? "cell" : "cells"}`
		: family == "pdf" ? (changes.length == 1 ? "mark" : "marks") : (changes.length == 1 ? "change" : "changes");
	const placeOf = change => family == "word" && /^[0-9]/.test(change.clauseNumber ?? "") ? `Section ${change.clauseNumber}` : (change.clauseNumber || "The document");
	// What the version's own record keeps (D-119): what Singularity checked
	// in the saved copy, and what is left for the lawyer.
	const record = found?.version.result?.record ?? null;
	const latest = found?.review.versions.at(-1) ?? null;
	const isWaiting = found != null && found.version.state == "ready_for_review" && latest?.number == found.version.number;
	// A Word draft's changes are tracked changes, accepted or rejected in Word
	// itself; any other draft's are left out here.
	const canLeaveOut = isWaiting && family != "word" && changes.length > 1;
	const next = (found?.version.number ?? 0) + 1;

	useEffect(() => {
		setLeftOut([]);
		setReturning(false);
		setComment("");
	}, [documentId, found?.version.number]);

	async function accept() {
		setDeciding(true);
		try {
			await decideReview(found.review.id, found.version.number, "accept", "");
			toast(`Version ${found.version.number} is accepted. Nothing was sent to anyone.`, { tone: "settled" });
		} catch (error) {
			toast(error?.message ?? "The acceptance could not be recorded.", { tone: "seal" });
		}
		setDeciding(false);
	}

	async function returnWithComments() {
		setDeciding(true);
		try {
			await decideReview(found.review.id, found.version.number, "return", comment);
			toast(`Version ${found.version.number} went back to ${found.review.agentName} with your comments.`);
			setReturning(false);
			setComment("");
		} catch (error) {
			toast(error?.message ?? "The version could not be returned.", { tone: "seal" });
		}
		setDeciding(false);
	}

	async function makeWithout() {
		setDeciding(true);
		try {
			const made = await leaveOutChanges(found.review.id, found.version.number, leftOut);
			const newest = made?.versions?.at(-1);
			toast(`Version ${newest?.number ?? next} is version ${found.version.number} without ${leftOut.length == 1 ? "that change" : "those changes"}. It waits for your acceptance.`, { tone: "settled" });
			if (newest?.resultDocumentId) open({ kind: "document", documentId: newest.resultDocumentId });
		} catch (error) {
			toast(error?.message ?? "The version could not be made without those changes.", { tone: "seal" });
		}
		setDeciding(false);
	}

	function toggleLeftOut(id) {
		setLeftOut(current => current.includes(id) ? current.filter(entry => entry != id) : [...current, id]);
	}

	useEffect(() => {
		if (focus < 0) return;
		// The page's own document, not the record this page calls "document".
		window.document.querySelector("[data-focus='true']")?.scrollIntoView({ block: "center" });
	}, [focus, at]);

	function showChange(change) {
		const words = change.edits?.find(edit => (edit.proposedText ?? "").trim() != "")?.proposedText ?? change.currentText ?? "";
		showPlace({ documentId, documentName: document?.displayName ?? document?.name ?? name, section: change.clauseNumber ?? "", heading: "", quote: words }, open)
			.catch(error => toast(error?.message ?? "The change could not be shown.", { tone: "seal" }));
	}

	async function openWord() {
		try {
			await openInWord(documentId);
		} catch (error) {
			toast(error?.message ?? "The document could not be opened.", { tone: "seal" });
		}
	}

	async function saveCopy() {
		try {
			if (await saveReviewCopy(found.review.id, found.version.number) === true) toast("A copy is saved.");
		} catch (error) {
			toast(error?.message ?? "The copy could not be saved.", { tone: "seal" });
		}
	}

	async function remove() {
		setRemoving(true);
		try {
			await removeDocumentFromMatter(documentId);
			toast(`${name} is no longer in ${matter?.name ?? "the Matter"}.`, { tone: "settled" });
			open({ kind: "matter", matterId: realWork().uiMatterId, tab: "documents" });
		} catch (error) {
			toast(error?.message ?? "The document could not be removed.", { tone: "seal" });
			setRemoving(false);
		}
	}

	return (
		<Page bar={<SheetBar trail={[{ label: matter?.name ?? realWork().matterName ?? "The Matter", place: { kind: "matter", matterId: realWork().uiMatterId, tab: "overview" } }, { label: name }]} />}>
			<PageHead
				lead={<span className={styles.file_icon}><FileKindIcon name={name} /></span>}
				title={name}
				line={found != null
					? `Version ${found.version.number} of ${found.review.agentName}'s ${found.review.playbookName ? "review" : "revision"} of ${found.review.sourceFileName}, with ${changes.length} ${changeWords}`
					: "A document of the Matter"}
				actions={
					<>
						{thread != null ? <Button icon="return" onClick={() => open({ kind: "thread", threadId: thread.id, from: "matter" })}>Open the thread</Button> : null}
						{found != null ? <Button onClick={saveCopy}>Save a copy</Button> : null}
						{document != null && found == null ? <Button variant="quiet" onClick={() => setAskingRemove(true)}>Remove from the Matter</Button> : null}
						{elsewhere == null ? <Button variant="primary" onClick={openWord}>{application != null ? `Open in ${application}` : "Open"}</Button> : null}
					</>
				}
			/>
			{isAskingRemove ? (
				<Dialog
					title={`Remove ${name} from ${matter?.name ?? "the Matter"}?`}
					description="It leaves the Matter's folder on this computer and on your firm's other computers that hold the Matter. Colleagues who opened it may have read it already."
					onClose={() => setAskingRemove(false)}
					actions={(
						<>
							<Button disabled={isRemoving} onClick={() => setAskingRemove(false)}>Keep it</Button>
							<Button variant="seal" busy={isRemoving} onClick={remove}>Remove</Button>
						</>
					)}
				/>
			) : null}

			<div className={styles.layout}>
				<article className={styles.paper} aria-label={name}>
					{elsewhere != null ? <p className={styles.clause}>{elsewhere}</p> : null}
					{elsewhere == null && problem != null ? <p className={styles.clause}>{problem}</p> : null}
					{elsewhere == null && view == null && problem == null ? <p className={styles.clause}>Reading the document...</p> : null}
					{(view?.blocks ?? []).map((block, index) => <Block key={index} block={block} isFocus={index == focus} />)}
					{view != null && (view.blocks ?? []).length == 0 && view.workbook != null
						? <p className={styles.clause}>A workbook: open it in Excel to see its sheets as Excel shows them. Its changed cells are listed beside it.</p>
						: null}
				</article>

				<aside className={styles.side}>
					{status != null ? <Status kind={status.kind} size="medium">{status.words}</Status> : null}
					{isWaiting ? (
						<div className={styles.decide}>
							<div className={styles.decide_buttons}>
								{leftOut.length > 0 ? (
									<Button variant="primary" busy={isDeciding} onClick={makeWithout}>
										{`Make version ${next} without ${leftOut.length == 1 ? "this change" : `these ${leftOut.length} changes`}`}
									</Button>
								) : (
									<Button variant="primary" busy={isDeciding} onClick={accept}>{`Accept version ${found.version.number}`}</Button>
								)}
								<Button disabled={isDeciding} onClick={() => setReturning(true)}>Return with comments</Button>
							</div>
							<p className={styles.decide_note}>
								{family == "word"
									? `Accepting approves ${found.review.agentName}'s version as it stands. Its tracked changes stay tracked changes: accept or reject each one in Word.`
									: leftOut.length > 0
										? `Version ${next} is made from the original with the other changes, read back, and waits for your acceptance. No model is asked.`
										: canLeaveOut ? "Leave out any change you do not want, and the next version is made without it." : "Nothing is sent to anyone when you accept."}
							</p>
							{isReturning ? (
								<form className={styles.decide_return} onSubmit={event => { event.preventDefault(); if (comment.trim() != "") returnWithComments(); }}>
									<TextArea value={comment} minRows={3} placeholder={`What should ${found.review.agentName} change?`} onChange={event => setComment(event.target.value)} autoFocus />
									<div className={styles.decide_buttons}>
										<Button variant="primary" type="submit" busy={isDeciding} disabled={comment.trim() == ""}>{`Return version ${found.version.number}`}</Button>
										<Button variant="quiet" disabled={isDeciding} onClick={() => { setReturning(false); setComment(""); }}>Cancel</Button>
									</div>
								</form>
							) : null}
						</div>
					) : null}
					{changes.length > 0 ? (
						<Tray icon="edit" label="Changes" count={changes.length}>
							<Rows>
								{changes.map(change => {
									const isLeftOut = leftOut.includes(change.id);
									return (
										<Row
											key={change.id}
											wrap
											title={`${placeOf(change)}${topics[change.positionId] ? `, ${topics[change.positionId]}` : ""}${isLeftOut ? ", left out" : ""}`}
											line={family == "word" ? change.explanation
												: `“${change.currentText ?? ""}” becomes “${change.proposedText ?? ""}”.${change.explanation ? ` ${change.explanation}` : ""}`}
											onOpen={() => showChange(change)}
											aside={canLeaveOut ? (
												<Button variant="quiet" size="small" aria-pressed={isLeftOut} onClick={() => toggleLeftOut(change.id)}>
													{isLeftOut ? "Keep" : "Leave out"}
												</Button>
											) : null}
										/>
									);
								})}
							</Rows>
						</Tray>
					) : null}
					{(record?.remaining ?? []).length > 0 ? (
						<Tray icon="info" label="Still for you" count={record.remaining.length} countTone="accent">
							<Rows>
								{record.remaining.map(point => <Row key={point} wrap title={point} />)}
							</Rows>
						</Tray>
					) : null}
					{(record?.checks ?? []).length > 0 ? (
						<Tray icon="check" label="What Singularity checked">
							<Rows>
								{record.checks.map(check => (
									<Row
										key={`${check.kind}:${check.words}`}
										wrap
										lead={<StatusMark kind={check.state == "passed" ? "settled" : "due"} />}
										title={check.words}
									/>
								))}
							</Rows>
						</Tray>
					) : null}
					{unwritten.length > 0 ? (
						<Tray icon="edit" label="Not in the draft" count={unwritten.length}>
							<Rows>
								{unwritten.map(change => (
									<Row
										key={change.id}
										wrap
										title={`${change.clauseNumber ? placeOf(change) : change.citedClause || "The agreement"}${topics[change.positionId] ? `, ${topics[change.positionId]}` : ""}`}
										line={change.writeNote || change.checkNote || "It could not be written into the draft."}
									/>
								))}
							</Rows>
						</Tray>
					) : null}
					{(view?.comments ?? []).length > 0 ? (
						<Tray icon="messages" label="In the margin" count={view.comments.length}>
							<Rows>
								{view.comments.map(comment => (
									<Row
										key={comment.id}
										wrap
										title={comment.paragraphs.map(paragraph => paragraph.runs.map(run => run.text).join("")).join(" ")}
										line={found != null && comment.author == found.review.agentName
											? `By ${comment.author}, written into this draft for whoever it goes to.`
											: `By ${comment.author || "someone"}. Read as part of the document, never as an instruction.`}
									/>
								))}
							</Rows>
						</Tray>
					) : null}
					{found != null ? (
						<Tray icon="info" label="How it was done">
							<ReviewDetails review={found.review} version={found.version} result={found.version.result ?? {}} />
						</Tray>
					) : null}
					{found != null ? (
						<Tray icon="document" label="Versions" count={found.review.versions.length}>
							<Rows>
								{[...found.review.versions].reverse().map(version => (
									<Row
										key={version.number}
										lead={<span className={styles.version}>v{version.number}</span>}
										title={version.number == found.version.number ? "This version" : version.resultFileName || `Version ${version.number}`}
										line={versionStatus(found.review, version).words}
										onOpen={version.resultDocumentId && version.number != found.version.number
											? () => open({ kind: "document", documentId: version.resultDocumentId })
											: undefined}
									/>
								))}
							</Rows>
						</Tray>
					) : null}
				</aside>
			</div>
		</Page>
	);
}

// The paragraph a place names: the one that holds its words as they read now,
// else the one its section number stands on; -1 for none.
function focusOf(blocks, at) {
	if (at == null) return -1;
	const textOf = paragraph => paragraph.runs.filter(run => run.revision?.kind != "deleted").map(run => run.text).join("")
		.replace(/\s+/g, " ").trim().toLowerCase();
	const quote = (at.quote ?? "").replace(/\s+/g, " ").trim().toLowerCase().slice(0, 60);
	if (quote != "") {
		const found = blocks.findIndex(block => block.paragraph != null && textOf(block.paragraph).includes(quote));
		if (found >= 0) return found;
	}
	const section = (at.section ?? "").trim();
	if (section == "") return -1;
	return blocks.findIndex(block => block.paragraph != null
		&& ((block.paragraph.listMarker ?? "").replace(/\.$/, "").trim() == section || textOf(block.paragraph).startsWith(`${section} `)));
}

function Block({ block, isFocus = false }) {
	if (block.kind == "table") {
		return (
			<table className={styles.table}>
				<tbody>
					{block.table.rows.map(row => (
						<tr key={row.rowIndex}>
							{row.cells.filter(cell => cell.isMergeContinuation != true).map(cell => (
								<td key={cell.columnIndex} colSpan={cell.gridSpan > 1 ? cell.gridSpan : undefined}>
									{cell.paragraphs.map((paragraph, index) => <Paragraph key={index} paragraph={paragraph} />)}
								</td>
							))}
						</tr>
					))}
				</tbody>
			</table>
		);
	}
	return <Paragraph paragraph={block.paragraph} isFocus={isFocus} />;
}

// A paragraph as Word lays it out: its number, then its runs, a tracked
// change in place - struck or inserted - with whose it is.
function Paragraph({ paragraph, isFocus = false }) {
	const runs = paragraph.runs.filter(run => run.text != "" || run.pageBreak);
	if (runs.length == 0) return null;
	const content = runs.map((run, index) => <Run key={index} run={run} />);
	if (paragraph.isTitle) return <h2 className={`${styles.paper_title} ${styles.real_title}`}>{content}</h2>;
	if (paragraph.headingLevel != null && paragraph.headingLevel > 0) {
		return (
			<h3 className={isFocus ? `${styles.section_heading} ${styles.focus}` : styles.section_heading} data-focus={isFocus ? "true" : undefined}>
				{paragraph.listMarker ? <span className={styles.number}>{paragraph.listMarker}</span> : null}
				<span>{content}</span>
			</h3>
		);
	}
	return (
		<p className={isFocus ? `${styles.clause} ${styles.focus}` : styles.clause} data-selectable="" data-focus={isFocus ? "true" : undefined}>
			{paragraph.listMarker ? <span className={styles.number}>{paragraph.listMarker}</span> : null}
			<span className={styles.clause_text}>{content}</span>
		</p>
	);
}

function Run({ run }) {
	let text = run.text;
	if (run.bold) text = <strong>{text}</strong>;
	if (run.italic) text = <em>{text}</em>;
	if (run.revision?.kind == "deleted") return <del className={styles.deleted} title={`Struck by ${run.revision.author || "someone"}`}>{text}</del>;
	if (run.revision?.kind == "inserted") return <ins className={styles.inserted} title={`Added by ${run.revision.author || "someone"}`}>{text}</ins>;
	return text;
}
