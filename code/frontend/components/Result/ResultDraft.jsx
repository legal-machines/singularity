import { useRef, useState } from "react";

import { FileTile } from "../FileTile/FileTile.jsx";
import { Icon } from "../Icon/Icon.jsx";
import { Menu, useMenu } from "../Menu/Menu.jsx";
import { documents } from "../../data/workspace.js";
import { fileLine } from "./Result.jsx";

import styles from "./ResultDraft.module.css";

// The size of a file from the computer, as a file's line says it.
export function sizeOf(bytes) {
	return bytes >= 1024 * 1024 ? `${(bytes / 1024 / 1024).toFixed(1)} MB` : `${Math.max(1, Math.round(bytes / 1024))} KB`;
}

// What a member hands in with a message while writing it, the same for a
// person as for an AI colleague (the owner, 2026-10-24): its files, what was
// not checked, and the answers to a question it asks.
export function useResultDraft() {
	const [files, setFiles] = useState([]);
	const [notChecked, setNotChecked] = useState("");
	const [options, setOptions] = useState(null);

	return {
		files,
		notChecked,
		options,
		addFile: file => setFiles(previous => previous.some(item => item.name == file.name) ? previous : [...previous, file]),
		removeFile: name => setFiles(previous => previous.filter(item => item.name != name)),
		setNotChecked,
		ask: () => setOptions(previous => previous ?? ["Yes", "Not yet"]),
		setOption: (index, value) => setOptions(previous => previous.map((option, at) => at == index ? value : option)),
		dropAsk: () => setOptions(null),
		hasFiles: files.length > 0,
		extras: () => ({
			files: files.length > 0 ? files : null,
			notChecked: files.length > 0 && notChecked.trim() != "" ? notChecked.trim() : null,
			ask: options != null && options.every(option => option.trim() != "") ? { options: options.map(option => option.trim()) } : null,
		}),
		reset: () => {
			setFiles([]);
			setNotChecked("");
			setOptions(null);
		},
	};
}

// The composer's "+": attach a file from the project's Documents or from the
// computer, or ask for an answer; a place may add its own acts after them. In
// a real Matter (`real`), the files are the Matter's own documents, and one
// from the computer is added to them through the system's file dialog.
export function AddButton({ matterId, draft, extra = [], real = null }) {
	const menu = useMenu();
	const pickerRef = useRef(null);
	const projectFiles = real != null
		? real.documents.map(item => ({ key: item.id, name: item.displayName ?? item.name, documentId: item.id, sizeBytes: item.sizeBytes }))
		: (documents[matterId] ?? []).filter(item => ["docx", "pdf", "xlsx"].includes(item.kind));

	return (
		<>
			<button
				className={styles.add}
				type="button"
				aria-label="Attach a file or ask for an answer"
				data-tooltip="Attach a file or ask for an answer"
				aria-haspopup="menu"
				aria-expanded={menu.isOpen}
				onClick={event => menu.toggle(event.currentTarget)}
			>
				<Icon name="plus" size={18} />
			</button>
			<input
				ref={pickerRef}
				className={styles.picker}
				type="file"
				multiple
				tabIndex={-1}
				aria-hidden="true"
				onChange={event => {
					for (const file of event.target.files ?? []) draft.addFile({ name: file.name, size: sizeOf(file.size) });
					event.target.value = "";
				}}
			/>
			{menu.isOpen ? (
				<Menu
					anchor={menu.anchor}
					label="Attach or ask"
					placement="top-start"
					minWidth={248}
					onClose={menu.close}
					items={[
						{
							key: "file",
							label: "Attach a file",
							icon: "attach",
							submenu: [
								...projectFiles.map(item => ({
									key: item.key ?? item.name,
									label: item.name,
									onSelect: () => draft.addFile(real != null
										? { name: item.name, documentId: item.documentId, size: item.sizeBytes ? sizeOf(item.sizeBytes) : undefined }
										: { name: item.name, pages: item.pages }),
								})),
								projectFiles.length > 0 ? { separator: true } : null,
								{
									key: "computer",
									label: "From this computer…",
									onSelect: real != null
										? async () => {
											for (const added of await real.onFromComputer()) {
												draft.addFile({ name: added.displayName ?? added.name, documentId: added.id, size: added.sizeBytes ? sizeOf(added.sizeBytes) : undefined });
											}
										}
										: () => pickerRef.current?.click(),
								},
							].filter(Boolean),
						},
						{ key: "ask", label: "Ask for an answer", icon: "messages", onSelect: draft.ask },
						...(extra.length > 0 ? [{ separator: true }, ...extra] : []),
					].filter(Boolean)}
				/>
			) : null}
		</>
	);
}

// Over the reply box, what the message will hand in: its files, a line for
// what was not checked, and the answers offered.
export function DraftTray({ draft }) {
	if (!draft.hasFiles && draft.options == null) return null;
	return (
		<div className={styles.tray}>
			{draft.hasFiles ? (
				<div className={styles.files}>
					{draft.files.map(file => <FileTile key={file.name} name={file.name} line={fileLine(file)} size={file.size} onOpen={() => {}} onRemove={() => draft.removeFile(file.name)} />)}
				</div>
			) : null}
			{draft.hasFiles ? (
				<input
					className={styles.note}
					value={draft.notChecked}
					placeholder="What you did not check (optional)"
					aria-label="What you did not check"
					onChange={event => draft.setNotChecked(event.target.value)}
				/>
			) : null}
			{draft.options != null ? (
				<div className={styles.ask}>
					<span className={styles.ask_label}>Answers</span>
					{draft.options.map((option, index) => (
						<input key={index} className={styles.option} value={option} aria-label={`Answer ${index + 1}`} onChange={event => draft.setOption(index, event.target.value)} />
					))}
					<button className={styles.ask_drop} type="button" aria-label="Ask nothing" data-tooltip="Ask nothing" onClick={draft.dropAsk}>
						<Icon name="close" size={14} />
					</button>
				</div>
			) : null}
		</div>
	);
}
