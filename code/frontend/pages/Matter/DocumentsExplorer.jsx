import { useEffect, useState } from "react";

import { Avatar } from "../../components/Avatar/Avatar.jsx";
import { Button } from "../../components/Button/Button.jsx";
import { ConfirmDialog } from "../../components/Manage/Manage.jsx";
import { FileKindIcon } from "../../components/FileKind/FileKind.jsx";
import { Icon } from "../../components/Icon/Icon.jsx";
import { useNavigation } from "../../components/Navigation/navigation.js";
import { Word } from "../../components/Parts/Parts.jsx";
import { sizeWords } from "../../components/Review/Review.jsx";
import { toast } from "../../components/Toast/toast.js";
import { openDocumentLens } from "../../components/DocumentLens/lens.js";
import { nativeApp, openMatterFolder, removeDocumentFromMatter, shownMembers, showInFolder, viewerSeatId } from "../../data/native.js";
import { viewerId } from "../../data/workspace.js";
import { findMember, formatDay, formatClock } from "../words.js";

import styles from "./DocumentsExplorer.module.css";

// A Matter's documents as the computer's own file browser shows a folder (the
// owner, 2026-11-02: "the documents should be viewed as in an explorer, by
// directories, and every file opens not inside the app but in the Office
// applications themselves"): where one is in the Matter's folder over the
// list; its folders first, then its files, each with when it came and its
// size. A folder opens in place; a file opens in its own application - Word,
// Excel, PowerPoint, a PDF's viewer - and its place in Finder or File Explorer
// is one press away. A document is taken out of the Matter from its row too
// (the owner, 2026-11-02: "I must be able to stop tracking a document inside
// the app"): one no longer in the folder is no longer tracked; one still in it
// goes to the computer's bin, as on its own page.
export function DocumentsExplorer({ matter, documents, isReal }) {
	const { open } = useNavigation();
	const [folder, setFolder] = useState([]);
	const [removing, setRemoving] = useState(null);
	const addedBy = useAddedBy(matter, documents, isReal);
	const at = folder.join("/");
	const { folders, files } = contentsOf(documents, folder);
	const finder = finderWords();

	async function openFile(item) {
		if (isReal != true) {
			if (item.documentId != null) open({ kind: "document", documentId: item.documentId });
			return;
		}
		if (item.kind == "gone" || item.kind == "missing") {
			toast(item.detail ?? `${item.name} is not in the Matter's folder on this computer.`, { tone: "seal" });
			return;
		}
		// Over the window, as written and as an AI provider receives it; its
		// own application opens it from there (D-233).
		openDocumentLens({ projectId: matter.realMatterId, documentId: item.documentId, name: item.name });
	}

	// The folder shown now: the Matter's own, or one of its files' folder, its
	// first file chosen there.
	async function showHere() {
		try {
			const inside = files.find(item => item.documentId != null && item.kind != "gone" && item.kind != "missing")
				?? documents.find(item => item.documentId != null && item.kind != "gone" && item.kind != "missing"
					&& (item.path ?? "").startsWith(`${at}/`));
			if (folder.length == 0 || inside == null) await openMatterFolder(matter.id);
			else await showInFolder(inside.documentId);
		} catch (error) {
			toast(error?.message ?? "The folder could not be shown.", { tone: "seal" });
		}
	}

	async function reveal(item) {
		try {
			await showInFolder(item.documentId);
		} catch (error) {
			toast(error?.message ?? `${item.name} could not be shown.`, { tone: "seal" });
		}
	}

	return (
		<section className={styles.explorer} aria-label={`${matter.name}: documents`}>
			{/* The Matter's folder in the computer's own file browser, where its
			    name stood (the owner, 2026-11-02: "remove Add documents, and in place
			    of Harbor make a button Show in Finder / File Explorer"); inside a
			    folder, that folder, and the way back over the list. */}
			<div className={styles.bar}>
				{isReal ? <Button size="small" icon="matter" onClick={() => showHere()}>{finder}</Button> : null}
				{folder.length > 0 ? (
					<nav className={styles.path} aria-label="Where in the Matter's folder">
						{["All documents", ...folder].map((name, index) => {
							const isHere = index == folder.length;
							return (
								<span key={`${index}-${name}`} className={styles.step}>
									{index > 0 ? <span className={styles.slash} aria-hidden="true">/</span> : null}
									{isHere
										? <span className={styles.here} aria-current="location">{name}</span>
										: <button type="button" className={styles.up} onClick={() => setFolder(folder.slice(0, index))}>{name}</button>}
								</span>
							);
						})}
					</nav>
				) : null}
			</div>

			{folders.length + files.length == 0 ? (
				<p className={styles.empty}>{at == "" ? `No document is in ${matter.name} yet. Add the files you work on, or drop them into its folder.` : "This folder is empty."}</p>
			) : (
				<div className={styles.table} role="table" aria-label={at == "" ? matter.name : folder.at(-1)}>
					<div className={styles.head} role="row">
						<span role="columnheader">Name</span>
						<span role="columnheader">Added by</span>
						<span role="columnheader">Added</span>
						<span role="columnheader" className={styles.size_head}>Size</span>
					</div>
					{folders.map(entry => (
						<div key={`folder:${entry.name}`} className={styles.row} role="row">
							<button type="button" className={styles.open} onClick={() => setFolder([...folder, entry.name])}>
								<span className={styles.name} role="cell">
									<FolderIcon />
									<span className={styles.label}>{entry.name}</span>
								</span>
								<span role="cell" />
								<span className={styles.when} role="cell">{entry.latest != null ? whenWords(entry.latest) : ""}</span>
								<span className={styles.size} role="cell">{entry.count == 1 ? "1 file" : `${entry.count} files`}</span>
							</button>
						</div>
					))}
					{files.map(item => (
						<div key={item.documentId ?? item.name} className={styles.row} role="row">
							<button type="button" className={styles.open} onClick={() => openFile(item)} title={isReal ? `Open ${item.name}` : undefined}>
								<span className={styles.name} role="cell">
									<FileKindIcon name={item.name} size="row" dim={["missing", "gone"].includes(item.kind)} />
									<span className={styles.label}>{item.name}</span>
									{stateOf(item)}
								</span>
								<span className={styles.by} role="cell">{whoAdded(item, addedBy, isReal)}</span>
								<span className={styles.when} role="cell">{item.added != null ? whenWords(item.added) : ""}</span>
								<span className={styles.size} role="cell">{item.sizeBytes != null ? sizeWords(item.sizeBytes) : item.pages != null ? `${item.pages} pages` : ""}</span>
							</button>
							{isReal && item.documentId != null ? (
								<span className={styles.actions}>
									{item.kind != "gone" && item.kind != "missing" ? (
										<button type="button" aria-label={`${finder}: ${item.name}`} data-tooltip={finder} onClick={() => reveal(item)}><Icon name="matter" size={17} /></button>
									) : null}
									<button type="button" aria-label={`${isAway(item) ? "Stop tracking" : "Remove from the Matter"}: ${item.name}`} data-tooltip={isAway(item) ? "Stop tracking" : "Remove from the Matter"} onClick={() => setRemoving(item)}><Icon name="trash" size={17} /></button>
								</span>
							) : null}
						</div>
					))}
				</div>
			)}
			{removing != null ? (
				<ConfirmDialog
					title={isAway(removing) ? `Stop tracking ${removing.name}?` : `Remove ${removing.name} from ${matter.name}?`}
					description={isAway(removing)
						? `It is not in the Matter's folder on this computer. ${matter.name} stops listing it, here and on your firm's other computers that hold the Matter.`
						: "It goes to this computer's bin and leaves the Matter's folder on your firm's other computers that hold the Matter. Colleagues who opened it may have read it already."}
					confirm={isAway(removing) ? "Stop tracking" : "Remove"}
					onClose={() => setRemoving(null)}
					onConfirm={async () => {
						await removeDocumentFromMatter(removing.documentId);
						toast(`${removing.name} is no longer in ${matter.name}.`);
					}} />
			) : null}
		</section>
	);
}

// Who added each document, as the Matter's own record of its documents says
// (project.sharedActivity: its latest changes, each with who made it), read
// as the list opens and again as documents come (the owner, 2026-11-02:
// "write who added this document - a face and a small name are enough").
function useAddedBy(matter, documents, isReal) {
	const [byDocument, setByDocument] = useState({});
	const realMatterId = isReal ? matter.realMatterId ?? null : null;
	const count = documents.length;
	useEffect(() => {
		if (realMatterId == null || typeof nativeApp()?.getSharedActivity != "function") return undefined;
		let isCurrent = true;
		nativeApp().getSharedActivity(realMatterId).then(activity => {
			if (isCurrent == false) return;
			const found = {};
			for (const change of activity?.changes ?? []) {
				if (change.kind != "added" || (change.documentId ?? "") == "") continue;
				if (change.author != "you" && change.author != "member") continue;
				found[change.documentId] = { isYou: change.author == "you", name: (change.authorName ?? "").trim() };
			}
			setByDocument(found);
		}, () => {});
		return () => {
			isCurrent = false;
		};
	}, [realMatterId, count]);
	return byDocument;
}

// The member who added it, as a small face and name; nothing for a file found
// in the folder, which no one in the firm can be named for.
function whoAdded(item, addedBy, isReal) {
	let member = null;
	if (isReal) {
		// The seat whose computer first recorded it; else the Matter's record of
		// who changed it.
		const actorId = item.addedByActorId ?? "";
		const viewer = findMember(viewerId);
		if (actorId != "") {
			const isViewer = (viewer?.actorId ?? "") == actorId || (viewerSeatId() != null && actorId == `seat:${viewerSeatId()}`);
			member = isViewer ? viewer : shownMembers().find(person => (person.actorId ?? "") == actorId) ?? null;
		}
		const by = member == null && item.documentId != null ? addedBy[item.documentId] : null;
		if (by != null) {
			member = by.isYou ? viewer : shownMembers().find(person => person.name == by.name)
				?? (by.name != "" ? { id: `by:${by.name}`, kind: "person", name: by.name } : null);
		}
	} else if ((item.by ?? "") != "") {
		member = shownMembers().find(person => person.name == item.by || person.name.split(" ")[0] == item.by) ?? { id: `by:${item.by}`, kind: "person", name: item.by };
	}
	if (member == null) return null;
	return (
		<>
			<Avatar member={member} size={20} />
			<span className={styles.by_name}>{member.id == viewerId ? "You" : member.name}</span>
		</>
	);
}

// A document whose file is not in the Matter's folder here: it is only no
// longer tracked when taken out.
function isAway(item) {
	return item.kind == "gone" || item.kind == "missing";
}

// What a folder holds: the folders in it, each with how many files are under
// it and when the latest came, and its own files, both by name as a file
// browser sorts them ("02" before "10").
function contentsOf(documents, folder) {
	const byName = (first, second) => first.name.localeCompare(second.name, undefined, { numeric: true, sensitivity: "base" });
	const folders = new Map();
	const files = [];
	for (const item of documents) {
		const parts = (item.path ?? item.name ?? "").split("/").filter(Boolean);
		if (parts.length <= folder.length || folder.some((name, index) => parts[index] != name)) continue;
		const rest = parts.slice(folder.length);
		if (rest.length == 1) {
			files.push(item);
			continue;
		}
		const entry = folders.get(rest[0]) ?? { name: rest[0], count: 0, latest: null };
		entry.count += 1;
		if (item.added != null && (entry.latest == null || item.added > entry.latest)) entry.latest = item.added;
		folders.set(rest[0], entry);
	}
	return { folders: [...folders.values()].sort(byName), files: files.sort(byName) };
}

// A file's state where it is not simply ready, as a word in its colour.
function stateOf(item) {
	switch (item.kind) {
		case "missing": return <Word tone="time">Missing</Word>;
		case "gone": return <Word tone="time">{item.detail != null ? "Not here" : "Missing"}</Word>;
		case "failed": return <Word tone="yours">Cannot be read</Word>;
		case "reading": return <Word tone="work">Reading</Word>;
		case "draft": return <Word>{item.draftOf != null ? `${item.draftOf.agentName}'s draft` : "Mark-up"}</Word>;
		default: return item.workProductId != null ? <Word>Mark-up</Word> : null;
	}
}

// "Today at 1:37 PM", "Yesterday at 9:02 AM", "Oct 28, 2026", as a file
// browser says when.
function whenWords(iso) {
	const day = formatDay(iso);
	if (day == "Today" || day == "Yesterday") return `${day} at ${formatClock(iso)}`;
	const date = new Date(iso);
	if (Number.isNaN(date.getTime())) return "";
	return new Intl.DateTimeFormat("en-US", { month: "short", day: "numeric", year: "numeric" }).format(date);
}

function finderWords() {
	const platform = window.nativeApp?.platform ?? (navigator.platform ?? "").slice(0, 3).toLowerCase();
	if (platform == "darwin" || platform == "mac") return "Show in Finder";
	if (platform == "win32" || platform == "win") return "Show in File Explorer";
	return "Show in Files";
}

// A folder as the file kinds' pages are drawn (FileKind.jsx): a white ground
// with a fine frame, its tab on top.
function FolderIcon() {
	return (
		<svg className={styles.folder} viewBox="0 0 32 26" aria-hidden="true">
			<path d="M2.75 4.5A2.25 2.25 0 0 1 5 2.25H11.6C12.2 2.25 12.77 2.49 13.19 2.91L15.03 4.75H27A2.25 2.25 0 0 1 29.25 7V22A2.25 2.25 0 0 1 27 24.25H5A2.25 2.25 0 0 1 2.75 22V4.5Z" />
			<path d="M2.75 8.75H29.25" />
		</svg>
	);
}
