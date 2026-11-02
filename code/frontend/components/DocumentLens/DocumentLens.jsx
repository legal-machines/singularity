import { useEffect, useRef, useState } from "react";
import { createPortal } from "react-dom";

import { Button } from "../Button/Button.jsx";
import { FileKindIcon, familyOf, kindOf } from "../FileKind/FileKind.jsx";
import { focusables } from "../Floating/position.js";
import { toast } from "../Toast/toast.js";
import { Views } from "../Views/Views.jsx";
import { remeasureTitlebarControls } from "../Window/titlebar.js";
import { keptDocumentMask, kindWord, maskDocument, sentSegments, writtenSegments } from "../../data/masking.js";
import { openInWord } from "../../data/native.js";
import { closeDocumentLens, useDocumentLens } from "./lens.js";

import styles from "./DocumentLens.module.css";

// A document over the window, read two ways (the owner, 2026-11-02: "pressing
// any document in the system renders it in a window on top, with a switch
// between how a person sees it and how the machine sees it - the document
// itself - and there mark what was there and what it was masked to"; D-233):
// as written, each spelling Papinian Guard masks marked with the symbol it
// goes out as; and as an AI provider receives it, each symbol where its words
// stood. It is the white every dialog is (Dialog.jsx), over the whole window,
// with its own way out; Escape leaves it too. Its own application opens it as
// it is laid out.
export function DocumentLensHost() {
	const lens = useDocumentLens();
	return lens != null ? <DocumentLens key={`${lens.projectId}:${lens.documentId}`} {...lens} onClose={closeDocumentLens} /> : null;
}

const VIEWS = [{ id: "written", label: "As written" }, { id: "sent", label: "As an AI provider receives it" }];

function DocumentLens({ projectId, documentId, name, onClose }) {
	const [view, setView] = useState("written");
	// The symbol a row of the table is pointed at, marked wherever it stands.
	const [focused, setFocused] = useState(null);
	const [preview, setPreview] = useState(() => keptDocumentMask(projectId, documentId));
	const [problem, setProblem] = useState(null);
	const sheetRef = useRef(null);

	useEffect(() => {
		let isLive = true;
		maskDocument(projectId, documentId).then(
			result => { if (isLive) setPreview(result); },
			error => { if (isLive) setProblem(error?.userMessage ?? error?.message ?? "Papinian Guard could not read the document. Try again."); },
		);
		return () => { isLive = false; };
	}, [projectId, documentId]);

	// As a dialog stands: the window's own controls over the white are put
	// away, the keys stay inside, Escape leaves, and the focus goes back.
	useEffect(() => {
		const root = document.documentElement;
		root.dataset.dialogOpen = String(Number(root.dataset.dialogOpen ?? 0) + 1);
		remeasureTitlebarControls();
		const sheet = sheetRef.current;
		const returnTo = document.activeElement;
		sheet?.focus();
		function keys(event) {
			if (event.key == "Escape") {
				event.preventDefault();
				event.stopPropagation();
				onClose();
			}
			if (event.key == "Tab" && sheet != null) {
				const list = focusables(sheet);
				if (list.length == 0) return;
				if (event.shiftKey && document.activeElement == list[0]) {
					event.preventDefault();
					list[list.length - 1].focus();
				} else if (!event.shiftKey && document.activeElement == list[list.length - 1]) {
					event.preventDefault();
					list[0].focus();
				}
			}
		}
		sheet?.addEventListener("keydown", keys);
		return () => {
			sheet?.removeEventListener("keydown", keys);
			const left = Number(root.dataset.dialogOpen ?? 1) - 1;
			if (left > 0) root.dataset.dialogOpen = String(left);
			else delete root.dataset.dialogOpen;
			remeasureTitlebarControls();
			returnTo?.focus?.();
		};
	}, [onClose]);

	const isSent = view == "sent";
	const replacements = preview?.replacements ?? [];
	const app = { word: "Word", sheet: "Excel", slides: "PowerPoint" }[familyOf(kindOf(name))];

	async function openInApp() {
		try {
			await openInWord(documentId);
		} catch (error) {
			toast(error?.message ?? `${name} could not be opened. Try again.`, { tone: "seal" });
		}
	}

	return createPortal(
		<div className={styles.lens}>
			<div className={styles.wash} aria-hidden="true" />
			<div ref={sheetRef} className={styles.sheet} role="dialog" aria-modal="true" aria-label={name} tabIndex={-1}>
				<header className={styles.head}>
					<div className={styles.name}>
						<FileKindIcon name={name} size="row" />
						<h2 className={styles.title}>{isSent && preview != null ? <Segments segments={sentSegments(preview.protectedName, replacements)} isSent /> : name}</h2>
					</div>
					<div className={styles.actions}>
						<Button onClick={openInApp}>{app != null ? `Open in ${app}` : "Open"}</Button>
						<Button variant="primary" onClick={onClose}>Done</Button>
					</div>
				</header>
				<div className={styles.bar}>
					<Views label="How to read it" views={VIEWS} view={view} onChange={setView} controls="document-lens-text" />
				</div>
				<div className={styles.body} id="document-lens-text">
					<div className={styles.column}>
						{problem != null ? <p className={styles.quiet}>{problem}</p>
							: preview == null ? <p className={styles.quiet}>Reading…</p>
							: preview.isRead != true ? <p className={styles.quiet}>Not read on this computer yet.</p>
							: (
								<>
									{preview.protectionMode == "on"
										? <MaskedTable replacements={replacements} focused={focused} onFocus={setFocused} />
										: <p className={styles.quiet}>Papinian Guard is off on this computer: it goes out as written.</p>}
									<section className={styles.section} aria-label="Text">
										<h3 className={styles.section_title}>Text</h3>
										<article>
											{preview.passages.map((passage, index) => (
												<Passage key={index} passage={passage} previous={preview.passages[index - 1]} replacements={replacements}
													isSent={isSent} isProtected={preview.protectionMode == "on"} focused={focused} />
											))}
										</article>
										{preview.passageCount > preview.passages.length ? (
											<p className={styles.quiet_after}>{preview.passageCount - preview.passages.length} more passages{app != null ? ` - open it in ${app} for all of it` : ""}.</p>
										) : null}
									</section>
								</>
							)}
					</div>
				</div>
			</div>
		</div>,
		document.body,
	);
}

// What Papinian Guard masks in it, as a list: each symbol, the words it
// stands for and their kind (the owner, 2026-11-02: "make it a table, in
// Apple's manner, not a sentence"). Pointed at, a row marks its symbol
// wherever it stands in the text.
const SHOWN = 8;

function MaskedTable({ replacements, focused, onFocus }) {
	const [isAll, setAll] = useState(false);
	const sorted = [...replacements].sort((first, second) => first.placeholder.localeCompare(second.placeholder, "en", { numeric: true }));
	const shown = isAll ? sorted : sorted.slice(0, SHOWN);
	return (
		<section className={styles.section} aria-label="Masked">
			<h3 className={styles.section_title}>Masked{sorted.length > 0 ? <span className={styles.count}>{sorted.length}</span> : null}</h3>
			{sorted.length == 0 ? <p className={styles.quiet}>Nothing in it.</p> : (
				<div className={styles.table} role="table" aria-label="Masked">
					<div className={styles.table_head} role="row">
						<span role="columnheader">Symbol</span>
						<span role="columnheader">Written as</span>
						<span role="columnheader">Kind</span>
					</div>
					{shown.map(entry => {
						const forms = (entry.forms ?? []).map(form => form.replace(/\s+/g, " "));
						return (
							<div key={entry.placeholder} role="row" className={styles.table_row} data-focused={focused == entry.placeholder || undefined}
								onMouseEnter={() => onFocus(entry.placeholder)} onMouseLeave={() => onFocus(null)}>
								<span role="cell"><span className={styles.symbol}>{entry.placeholder}</span></span>
								<span role="cell" className={styles.written} data-tooltip={forms.length > 1 ? forms.join(", ") : undefined}>
									{forms[0] ?? ""}{forms.length > 1 ? <span className={styles.other_forms}> +{forms.length - 1}</span> : null}
								</span>
								<span role="cell" className={styles.kind}>{sentenceCase(kindWord(entry.category))}</span>
							</div>
						);
					})}
				</div>
			)}
			{sorted.length > SHOWN ? (
				<button type="button" className={styles.more} onClick={() => setAll(all => !all)}>{isAll ? "Show fewer" : `Show all ${sorted.length}`}</button>
			) : null}
		</section>
	);
}

const sentenceCase = text => text.charAt(0).toUpperCase() + text.slice(1);

// One passage: as written, or as it goes out; where a new page begins, its
// number over it.
function Passage({ passage, previous, replacements, isSent, isProtected, focused }) {
	const page = passage.page != null && passage.page != previous?.page ? passage.page : null;
	return (
		<section className={styles.passage}>
			{page != null ? <p className={styles.page}>Page {page}</p> : null}
			{isSent && passage.withheld
				? <p className={styles.withheld}>Not sent: Papinian Guard could not mask this passage.</p>
				: <p className={styles.words}><Segments segments={isSent ? sentSegments(passage.protectedText, replacements) : isProtected ? writtenSegments(passage.text, replacements) : [{ text: passage.text }]} isSent={isSent} focused={focused} /></p>}
		</section>
	);
}

// Words, and the masked ones marked: as written, a spelling with the symbol
// it goes out as under the pointer; as sent, the symbol with its words.
export function Segments({ segments, isSent, focused = null }) {
	return segments.map((segment, index) => {
		if (segment.tag == null) return <span key={index}>{segment.text}</span>;
		const isFocused = focused != null && segment.tag == focused;
		return isSent
			? <span key={index} className={styles.symbol} data-focused={isFocused || undefined} data-tooltip={segment.words != "" ? segment.words : undefined}>{segment.tag}</span>
			: <mark key={index} className={styles.masked} data-focused={isFocused || undefined} data-tooltip={segment.tag}>{segment.words}</mark>;
	});
}
