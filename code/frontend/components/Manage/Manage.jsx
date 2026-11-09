import { useState } from "react";

import { Button, IconButton } from "../Button/Button.jsx";
import { Dialog } from "../Dialog/Dialog.jsx";
import { Field, Input, TextArea } from "../Field/Field.jsx";
import { Menu, useMenu } from "../Menu/Menu.jsx";

import styles from "./Manage.module.css";

// What a member may do to a Matter, a thread or a member of the firm: archive
// it, delete it, change it. Each stands behind one quiet button, its menu;
// what cannot be undone asks once, in words that say what goes and what
// stays.

export function MoreButton({ label, items, size = "medium" }) {
	const menu = useMenu();
	const shown = items.filter(Boolean);
	if (shown.length == 0) return null;
	return (
		<>
			<IconButton icon="more" size={size} label={label} pressed={menu.isOpen} onClick={event => menu.toggle(event.currentTarget)} />
			{menu.isOpen ? <Menu anchor={menu.anchor} label={label} minWidth={220} placement="bottom-end" onClose={menu.close} items={shown} /> : null}
		</>
	);
}

// One step that is done once it is confirmed: its title says what, its words
// what goes and what stays.
export function ConfirmDialog({ title, description, confirm, onConfirm, onClose }) {
	const [isBusy, setBusy] = useState(false);
	const [error, setError] = useState(null);

	async function go() {
		if (isBusy) return;
		setBusy(true);
		setError(null);
		try {
			await onConfirm();
			onClose();
		} catch (reason) {
			setError(reason?.detail ?? reason?.message ?? "It could not be done. Try again.");
			setBusy(false);
		}
	}

	return (
		<Dialog
			title={title}
			description={description}
			size="small"
			onClose={onClose}
			initialFocus="button"
			actions={(
				<>
					<Button disabled={isBusy} onClick={onClose}>Cancel</Button>
					<Button variant="seal" busy={isBusy} onClick={go}>{confirm}</Button>
				</>
			)}
		>
			{error != null ? <p className={styles.error} role="alert">{error}</p> : null}
		</Dialog>
	);
}

// A form in a dialog that saves once: its fields, and Save while they hold
// what it needs.
function EditDialog({ title, description, canSave, onSave, onClose, children }) {
	const [isBusy, setBusy] = useState(false);
	const [error, setError] = useState(null);

	async function save() {
		if (isBusy || canSave == false) return;
		setBusy(true);
		setError(null);
		try {
			await onSave();
			onClose();
		} catch (reason) {
			setError(reason?.detail ?? reason?.message ?? "It could not be saved. Try again.");
			setBusy(false);
		}
	}

	return (
		<Dialog
			layout="spacious"
			title={title}
			description={description}
			initialFocus="input"
			onClose={onClose}
			actions={(
				<>
					<Button disabled={isBusy} onClick={onClose}>Cancel</Button>
					<Button variant="primary" busy={isBusy} disabled={canSave == false} onClick={save}>Save</Button>
				</>
			)}
		>
			<form className={styles.fields} onSubmit={event => { event.preventDefault(); save(); }}>
				{children}
				{error != null ? <p className={styles.error} role="alert">{error}</p> : null}
			</form>
		</Dialog>
	);
}

// A Matter's name, its client and what the work is, as New Matter asked them.
export function EditMatterDialog({ words, onSave, onClose }) {
	const [name, setName] = useState(words.name);
	const [client, setClient] = useState(words.client);
	const [about, setAbout] = useState(words.about);
	return (
		<EditDialog title="Edit the Matter" canSave={name.trim() != ""} onSave={() => onSave({ name, client, about })} onClose={onClose}>
			<Field label="Name">
				<Input value={name} maxLength={120} spellCheck={false} autoComplete="off" onChange={event => setName(event.target.value)} />
			</Field>
			<Field label="Client" optional>
				<Input value={client} maxLength={120} autoComplete="off" onChange={event => setClient(event.target.value)} />
			</Field>
			<Field label="About" optional>
				<TextArea value={about} minRows={3} maxRows={8} maxLength={1200} placeholder="What the work is" onChange={event => setAbout(event.target.value)} />
			</Field>
		</EditDialog>
	);
}

// A thread's title.
export function RenameThreadDialog({ title, onSave, onClose }) {
	const [words, setWords] = useState(title);
	return (
		<EditDialog title="Rename the thread" canSave={words.trim() != ""} onSave={() => onSave(words)} onClose={onClose}>
			<Field label="Title">
				<Input value={words} maxLength={160} autoComplete="off" onChange={event => setWords(event.target.value)} />
			</Field>
		</EditDialog>
	);
}
