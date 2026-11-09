import { useState } from "react";

import { useNavigation } from "../Navigation/navigation.js";
import { Button } from "../Button/Button.jsx";
import { familyOf } from "../FileKind/FileKind.jsx";
import { FileTile, kindOf, wordOf } from "../FileTile/FileTile.jsx";
import { NotChecked } from "../Result/Result.jsx";
import { Status } from "../Status/Status.jsx";
import { toast } from "../Toast/toast.js";
import { answerAssignmentDecision, decideReview, openMatterRealId, realDocument, retryReview, reviewById, reviewOfDocument, saveReviewCopy, showInFolder, versionOfPost } from "../../data/native.js";
import { openDocumentLens } from "../DocumentLens/lens.js";
import { viewerId } from "../../data/workspace.js";

import styles from "./Review.module.css";

// A colleague's document review where it is handed in: under the message
// that hands in a version, the revised draft as a file, its changes clause by
// clause, how the work was done, and what the lawyer can do with it - accept
// it, or return it with comments. Everything here is the backend's record
// (DocumentReview), read again whenever it changes; nothing is kept here.
export function PostReview({ post, thread, onReturn }) {
	const found = versionOfPost(post);
	if (found == null) return null;
	const { review, version } = found;
	// The draft itself stands among the message's files, as anyone's file does;
	// a message from before files were kept shows it here.
	if (version != null) return <VersionBlock review={review} version={version} onReturn={onReturn} isFileShown={(post.realFiles?.length ?? 0) == 0} />;
	// A message of the review that hands nothing in: the colleague saying it
	// stopped. The latest of them offers the way on.
	const latest = review.versions.at(-1);
	const ownPosts = thread.posts.filter(entry => entry.reviewId == review.id && entry.authorId != viewerId);
	const isLatestMessage = ownPosts.at(-1)?.id == post.id;
	if (latest?.state == "failed" && isLatestMessage) return <TryAgain review={review} version={latest} />;
	return null;
}

// The files a message hands over, a person's or a colleague's alike, as the
// same tiles: a press opens the document over the window, as written and as
// an AI provider receives it; a colleague's revised draft says how many
// tracked changes it carries, and saves as a copy; any other file is shown
// where it is on this computer. `projectId` is the project the files are
// in, the open Matter's when not given.
export function RealFiles({ files, notChecked = null, projectId = null }) {
	return (
		<div className={styles.files}>
			{files.map(file => {
				const found = reviewOfDocument(file.documentId);
				const written = (found?.version.result?.changes ?? []).filter(change => change.writeState == "written").length;
				const isSuperseded = found?.version.state == "superseded";
				const line = found == null ? wordOf(kindOf(file.name))
					: isSuperseded ? found.version.stateMessage || "Replaced by a later version" : changesWords(written, file.name);
				return (
					<FileTile
						key={file.documentId}
						name={file.name}
						line={line}
						size={sizeWords(file.sizeBytes)}
						isOld={isSuperseded}
						// Over the window, as written and as an AI provider receives
						// it; its own application opens it from there.
						onOpen={() => openDocumentLens({ projectId: projectId ?? openMatterRealId(), documentId: file.documentId, name: file.name })}
						onDownload={found != null
							? async () => {
								try {
									if (await saveReviewCopy(found.review.id, found.version.number) === true) toast("A copy is saved.");
								} catch (error) {
									toast(error?.message ?? "The copy could not be saved.", { tone: "seal" });
								}
							}
							: () => showInFolder(file.documentId).catch(error => toast(error?.message ?? "The file could not be shown.", { tone: "seal" }))}
						downloadLabel={found != null ? "Save a copy" : "Show in folder"}
						downloadIcon={found != null ? "download" : "external"}
					/>
				);
			})}
			<NotChecked text={notChecked} />
		</div>
	);
}

function TryAgain({ review, version }) {
	const [isAsking, setAsking] = useState(false);
	return (
		<div className={styles.answers}>
			<Button
				variant="primary"
				disabled={isAsking}
				onClick={async () => {
					setAsking(true);
					try {
						await retryReview(review.id);
					} catch (error) {
						toast(error?.message ?? "The review could not begin again.", { tone: "seal" });
						setAsking(false);
					}
				}}
			>
				Try again
			</Button>
			<span className={styles.after}>{version.execution?.attempt > 1 ? `Tried ${version.execution.attempt} times` : null}</span>
		</div>
	);
}

// Under the message that hands a version in, what a chat shows of it and no
// more, as Twist shows a message with its file: the draft itself, and, while
// it waits for the lawyer, the decision it waits for. Its changes clause by
// clause and how it was done are the document's, a press on the file away.
function VersionBlock({ review, version, onReturn, isFileShown = true }) {
	const { open: openPlace } = useNavigation();
	const [isDeciding, setDeciding] = useState(false);
	const latest = review.versions.at(-1);
	const result = version.result ?? {};
	const changes = result.changes ?? [];
	const written = changes.filter(change => change.writeState == "written");
	const document = version.resultDocumentId ? realDocument(version.resultDocumentId) : null;
	const isSuperseded = version.state == "superseded";
	// The version that waits for the lawyer: the latest, or the one before it
	// when the latest changed nothing.
	const waiting = [...review.versions].reverse().find(entry => entry.state == "ready_for_review") ?? null;
	const isWaiting = waiting?.number == version.number && (latest?.number == version.number || latest?.state == "unchanged");

	const hasFile = Boolean(version.resultFileName) && isFileShown;
	if (hasFile == false && isWaiting == false) return null;

	async function accept() {
		setDeciding(true);
		try {
			await decideReview(review.id, version.number, "accept", "");
			toast(`Version ${version.number} is accepted. Nothing was sent to anyone.`, { tone: "settled" });
		} catch (error) {
			toast(error?.message ?? "The acceptance could not be recorded.", { tone: "seal" });
		}
		setDeciding(false);
	}

	async function saveCopy() {
		try {
			const saved = await saveReviewCopy(review.id, version.number);
			if (saved === true) toast("A copy is saved.");
		} catch (error) {
			toast(error?.message ?? "The copy could not be saved.", { tone: "seal" });
		}
	}

	return (
		<div className={styles.review}>
			{hasFile ? (
				<FileTile
					name={version.resultFileName}
					line={isSuperseded ? version.stateMessage || "Replaced by a later version" : changesWords(written.length, version.resultFileName)}
					size={sizeWords(document?.sizeBytes)}
					isOld={isSuperseded}
					onOpen={version.resultDocumentId ? () => openPlace({ kind: "document", documentId: version.resultDocumentId }) : undefined}
					onDownload={saveCopy}
				/>
			) : null}
			{isWaiting ? (
				<div className={styles.answers}>
					{version.resultDocumentId ? (
						<Button variant="primary" disabled={isDeciding} onClick={() => openPlace({ kind: "document", documentId: version.resultDocumentId })}>Review changes</Button>
					) : null}
					<Button variant="secondary" disabled={isDeciding} onClick={accept}>Accept version {version.number}</Button>
					<Button variant="quiet" disabled={isDeciding} onClick={() => onReturn(review, version)}>Return with comments</Button>
				</div>
			) : null}
		</div>
	);
}

// The documents an assignment's card already shows as its results: the
// message that carries the card shows them there, not again as files.
export function resultDocumentIdsOf(assignment) {
	const ids = new Set();
	for (const step of assignment?.steps ?? []) {
		const review = reviewById(step.reviewId);
		for (const version of review?.versions ?? []) if (version.resultDocumentId) ids.add(version.resultDocumentId);
		if (step.documentId && (step.reviewId ?? "") == "") ids.add(step.documentId);
	}
	return ids;
}

// What a colleague's work handed in, under the message in which it says it
// has finished: each document it wrote or revised, and the email it
// drafted, as the files any message hands over, opened over the window as
// written and as an AI provider receives it. No verdict is asked of the
// member. Its reasoning stands in the document it wrote, each change with
// why it was made, never under the message; how the work was done is the
// message's menu's.
export function AssignmentBlock({ assignment }) {
	const email = assignment.email ?? {};
	const files = [];
	for (const step of assignment.steps ?? []) {
		const review = reviewById(step.reviewId);
		const version = review?.versions.find(entry => entry.number == (step.latestVersion || step.version)) ?? review?.versions.at(-1);
		const documentId = version?.resultDocumentId || step.documentId;
		if (!documentId || files.some(file => file.documentId == documentId)) continue;
		const document = realDocument(documentId);
		files.push({ documentId, name: document?.name ?? step.sourceName ?? "A document", sizeBytes: document?.sizeBytes ?? null });
	}
	if (email.isWanted && email.documentId && files.some(file => file.documentId == email.documentId) == false)
		files.push({ documentId: email.documentId, name: email.name || "Email.eml", sizeBytes: null });

	// Nothing handed in yet: no card, and no room for one under the
	// message.
	if (files.length == 0) return null;

	return (
		<div className={styles.assignment}>
			<RealFiles files={files} />
		</div>
	);
}

// Where a decision applies, each part of it named once: "Opening and Exhibit
// B", never "Opening, Opening and Exhibit B" (a DPA's decision listed
// "Opening" nine times, 2026-10-31). Said as it was when nothing repeats.
export function placeOnce(place) {
	const parts = (place ?? "").split(/, | and /).map(part => part.trim()).filter(Boolean);
	const kept = [...new Set(parts)];
	if (kept.length == parts.length) return place ?? "";
	if (kept.length == 1) return kept[0];
	return `${kept.slice(0, -1).join(", ")} and ${kept.at(-1)}`;
}

export function AssignmentDetails({ assignment }) {
	const recoveries = assignment.recoveries ?? [];
	const seconds = Math.round((assignment.milliseconds ?? 0) / 1000);
	const rows = [
		["Model", `${modelWords(assignment.modelId)}${assignment.providerId ? ` through ${providerWords(assignment.providerId)}` : ""}`],
		["Calls", `${assignment.calls ?? 0} to the model${assignment.retries > 0 ? `, ${assignment.retries} of them asked again` : ", none asked again"}`],
		["Tokens", `${number(assignment.inputTokens)} in, ${number(assignment.outputTokens)} out`],
		["Cost", assignment.costMicros != null ? `$${(assignment.costMicros / 1000000).toFixed(2)}, as ${providerWords(assignment.providerId)} reported it` : "Not reported"],
		["Time", seconds > 0 ? `${seconds} ${seconds == 1 ? "second" : "seconds"} from the request to the last word` : "Not measured"],
		...(recoveries.length > 0 ? [["Asked again", recoveries.join(" ")]] : []),
		...((assignment.setAside ?? []).length > 0 ? [["Set aside", assignment.setAside.join(" ")]] : []),
	];
	return (
		<dl className={styles.details}>
			{rows.map(([term, words]) => (
				<div key={term} className={styles.detail}>
					<dt>{term}</dt>
					<dd>{words}</dd>
				</div>
			))}
		</dl>
	);
}

// The decisions a colleague's message puts to the lawyer, under it: each
// question, why it matters, where it applies, and the lawyer's options, the
// colleague's recommendation first - or the lawyer's own words. An answer is
// recorded once, with who gave it, and goes to the colleague as the lawyer's
// word in the thread.
export function DecisionList({ items }) {
	return (
		<ul className={styles.decisions} aria-label="Decisions for you">
			{items.map(({ assignment, decision }) => <DecisionItem key={`${assignment.id}-${decision.id}`} assignment={assignment} decision={decision} />)}
		</ul>
	);
}

// One decision, said once: what is to be decided, why it matters, the choices
// with the colleague's recommendation marked on its own choice and its
// reasoning beside it, the member's own instruction, what happens once they
// answer, and where it applies. A recommendation is never shown as a choice
// already taken: every choice looks the same until the member takes one.
function DecisionItem({ assignment, decision }) {
	const { open } = useNavigation();
	const options = decision.options ?? [];
	const [isOwnWords, setOwnWords] = useState(options.length == 0);
	const [words, setWords] = useState("");
	const [sending, setSending] = useState(null);
	const isAnswered = decision.state == "answered";
	const agent = assignment.agentName || "The colleague";
	const recommended = decision.isRecommended && options.length > 1 ? options[0] : null;
	const where = [placeOnce(decision.place), stemWords(decision.documentName)].filter(Boolean).join(" · ");
	// What follows an answer, from the decision's own structure, never the
	// model's words.
	const next = decision.isBlocking
		? `${agent} waits for your answer before finishing this.`
		: `${agent} carries on meanwhile and takes your answer into the work.`;

	async function answer(text) {
		const given = text.trim();
		if (given == "") return;
		setSending(given);
		try {
			await answerAssignmentDecision(assignment.id, decision.id, given);
		} catch (error) {
			toast(error?.message ?? "The answer could not be recorded.", { tone: "seal" });
			setSending(null);
		}
	}

	return (
		<li className={styles.decision}>
			<p className={styles.decision_question}>{decision.question}</p>
			{decision.why ? <p className={styles.decision_why}>{decision.why}</p> : null}
			{isAnswered ? (
				<Status kind="settled">{`${decision.answeredByName || "Answered"}: ${decision.answer}`}</Status>
			) : (
				<>
					{options.length > 0 ? (
						<div className={styles.choices} role="group" aria-label={decision.question}>
							{options.map(option => {
								const isRecommended = option == recommended;
								return (
									<button
										key={option}
										type="button"
										className={styles.choice}
										disabled={sending != null}
										aria-busy={sending == option || undefined}
										onClick={() => answer(option)}
									>
										<span className={styles.choice_words}>{option}</span>
										{isRecommended ? <span className={styles.choice_mark}>Recommended</span> : null}
										{isRecommended && decision.reasoning ? <span className={styles.choice_reason}>{decision.reasoning}</span> : null}
									</button>
								);
							})}
						</div>
					) : null}
					{isOwnWords ? (
						<form className={styles.decision_own} onSubmit={event => { event.preventDefault(); answer(words); }}>
							<input
								className={styles.decision_input}
								value={words}
								onChange={event => setWords(event.target.value)}
								placeholder={`Your instruction to ${agent}`}
								aria-label={`Your instruction: ${decision.question}`}
								disabled={sending != null}
							/>
							<Button size="small" variant="primary" type="submit" disabled={words.trim() == "" || sending != null} busy={sending != null && options.includes(sending) == false}>
								Send
							</Button>
						</form>
					) : null}
					<p className={styles.decision_after}>
						{isOwnWords ? null : (
							<button className={styles.link} type="button" disabled={sending != null} onClick={() => setOwnWords(true)}>Other instruction</button>
						)}
						<span className={styles.decision_next}>{next}</span>
					</p>
				</>
			)}
			{where != "" && decision.documentId ? (
				<button className={styles.decision_source} type="button" onClick={() => open({ kind: "document", documentId: decision.documentId })}>{where}</button>
			) : null}
		</li>
	);
}

function stemWords(name) {
	return (name ?? "").replace(/\.[a-z0-9]{2,5}$/i, "");
}

// How the work was done: the model and where it ran, what it read and left
// out, what it cost in tokens and money, and how long it took - Usage, kept
// visible to the lawyer.
export function ReviewDetails({ review, version, result }) {
	const execution = version.execution ?? {};
	const preparation = execution.preparation ?? {};
	const omitted = [...new Set((preparation.omissions ?? []).map(item => (item.reason ?? "").split(":")[0]).filter(Boolean))];
	const changes = result.changes ?? [];
	const matched = changes.filter(change => change.check == "verified" || change.check == "relocated").length;
	// The colleague's message, written after the draft from what it holds and
	// checked against it: how it came to be, and what it took.
	const handIn = preparation.handIn ?? null;
	const message = handIn?.attempts ?? [];
	const messageIn = message.reduce((sum, attempt) => sum + (attempt.inputTokens ?? 0), 0);
	const messageOut = message.reduce((sum, attempt) => sum + (attempt.outputTokens ?? 0), 0);
	// A deck, a workbook or a PDF is checked by reading its saved copy back;
	// an agreement, by finding each change's words in it.
	const isAgreement = result.kind == null;
	const rows = [
		["Model", `${modelWords(execution.modelId)}${execution.providerId == "local" ? ", on this computer" : execution.providerId ? ` through ${providerWords(execution.providerId)}` : ""}${execution.servedBy ? `, served by ${execution.servedBy}` : ""}`],
		["Protection", execution.providerId == "local" ? "Nothing left this computer"
			: execution.isProtectionApplied ? "Papinian Guard protected what left this computer"
				: "Sent without Papinian Guard's protection"],
		["Read", `${preparation.includedBlocks ?? "Every"} parts of ${review.sourceFileName || "the document"}${omitted.length > 0 ? `; left out ${listWords(omitted)}` : ""}`],
		["Tokens", execution.inputTokens > 0 || execution.outputTokens > 0
			? `${number(execution.inputTokens)} in, ${number(execution.outputTokens)} out${messageIn > 0 || messageOut > 0 ? `; the message ${number(messageIn)} in, ${number(messageOut)} out` : ""}`
			: "Not reported"],
		["Cost", costWords(execution, handIn)],
		["Time", execution.latencyMilliseconds > 0
			? `${seconds(execution.latencyMilliseconds)} with the model; the document was prepared in ${number(execution.preparationMilliseconds ?? 0)} ms`
			: "Not measured"],
		["Checked", changes.length == 0 ? "No changes to check"
			: isAgreement ? `${matched} of ${changes.length} ${changes.length == 1 ? "change" : "changes"} found word for word in the agreement`
				: `Every change read back from the saved copy`],
		...(handIn ? [["Message", messageWords(review.agentName || "The colleague", handIn)]] : []),
		...((result.record?.recoveries ?? []).length > 0 ? [["Asked again", result.record.recoveries.join(" ")]] : []),
	];
	return (
		<dl className={styles.details}>
			{rows.map(([term, words]) => (
				<div key={term} className={styles.detail}>
					<dt>{term}</dt>
					<dd>{words}</dd>
				</div>
			))}
		</dl>
	);
}

// What a draft is and holds, by its kind: a Word document's tracked changes, a
// workbook's changed cells, a PDF's marks, a deck's changes.
function changesWords(count, name) {
	const kind = kindOf(name ?? "draft.docx");
	const family = familyOf(kind);
	const what = wordOf(kind);
	if (count == 0) return what;
	if (family == "word") return `${what} with ${count} tracked ${count == 1 ? "change" : "changes"}`;
	if (family == "sheet") return `${what} with ${count} changed ${count == 1 ? "cell" : "cells"}`;
	if (family == "pdf") return `${what} with ${count} ${count == 1 ? "mark" : "marks"}`;
	return `${what} with ${count} ${count == 1 ? "change" : "changes"}`;
}

export function sizeWords(bytes) {
	if (bytes == null || bytes <= 0) return null;
	if (bytes < 1024) return `${bytes} B`;
	if (bytes < 1024 * 1024) return `${Math.round(bytes / 1024)} KB`;
	return `${(bytes / (1024 * 1024)).toFixed(1)} MB`;
}

function number(value) {
	return new Intl.NumberFormat("en-US").format(value ?? 0);
}

function seconds(milliseconds) {
	const value = Math.round(milliseconds / 1000);
	return `${value} ${value == 1 ? "second" : "seconds"}`;
}

function costWords(execution, handIn) {
	if (execution.providerId == "local") return "Nothing: it ran on this computer";
	if (execution.costMicros == null) return "Not reported";
	const messageMicros = handIn?.costMicros ?? null;
	const money = micros => {
		const amount = micros / 1000000;
		return amount < 0.01 ? `$${amount.toFixed(4)}` : `$${amount.toFixed(2)}`;
	};
	const total = money(execution.costMicros + (messageMicros ?? 0));
	const split = messageMicros != null ? ` (${money(execution.costMicros)} the review, ${money(messageMicros)} the message)` : "";
	const isReported = execution.costSource == "provider_reported" && (messageMicros == null || handIn.costSource == "provider_reported");
	return isReported ? `${total}${split}, as ${providerWords(execution.providerId)} reported it` : `About ${total}${split}, estimated`;
}

// How the colleague's message under the draft was written.
function messageWords(agent, handIn) {
	const tries = (handIn.attempts ?? []).filter(attempt => attempt.failed == null).length;
	if (handIn.writtenBy == "colleague") {
		return tries > 1 ? `${agent}'s own words, written again once to agree with the draft`
			: `${agent}'s own words, checked against the draft`;
	}
	const why = (handIn.why ?? "").replace(/ The facts were said plainly\.$/, "");
	return `Said plainly from the draft${why ? `: ${why.charAt(0).toLowerCase()}${why.slice(1)}` : ""}`;
}

function providerWords(id) {
	if (id == "openrouter") return "OpenRouter";
	if (id == "openai") return "OpenAI";
	if (id == "anthropic") return "Anthropic";
	return id || "the provider";
}

// "anthropic/claude-haiku-5.5" is Claude Haiku 5.5.
function modelWords(id) {
	if (!id) return "The Matter's model";
	const name = id.split("/").pop();
	return name.split("-").map(word => /^[a-z]/.test(word) ? word[0].toUpperCase() + word.slice(1) : word).join(" ")
		.replace(/^Gpt\b/, "GPT");
}

function listWords(items) {
	if (items.length <= 1) return items.join("");
	return `${items.slice(0, -1).join(", ")} and ${items[items.length - 1]}`;
}
