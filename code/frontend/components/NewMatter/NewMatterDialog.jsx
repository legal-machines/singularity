import { useRef, useState } from "react";

import { Avatar } from "../Avatar/Avatar.jsx";
import { Button } from "../Button/Button.jsx";
import { Dialog } from "../Dialog/Dialog.jsx";
import { Field, Input, TextArea } from "../Field/Field.jsx";
import { Icon } from "../Icon/Icon.jsx";
import { MarkChooser } from "../ProjectMark/ProjectMark.jsx";
import { projectColors } from "../ProjectMark/looks.js";
import { dropNotice, endNotice, notice, toast } from "../Toast/toast.js";
import { createFirmMatter, hasNative, nativeApp, teamMembers } from "../../data/native.js";
import { viewerId } from "../../data/workspace.js";
import { Dots } from "../../onboarding/Dots.jsx";
import { positionOf } from "../../pages/words.js";

import styles from "./NewMatterDialog.module.css";

// A new Matter, as a lawyer opens one (D-211): what the firm calls it, its
// client, what the work is, the folder its `.lm` stands in beside its
// documents, and the lawyers of the firm it goes to at once (D-205). Each
// chosen lawyer's computer asks them to accept it, and it travels sealed to
// them alone. Opening it goes on after the dialog has gone, as a notice at the
// window's top (the owner, 2026-11-02): a folder of many documents takes a
// while, and the member is never held in the dialog for it.
//
// Two steps, as onboarding goes (the owner, 2026-11-02: "share with must be
// the second stage, as in onboarding"): first the Matter itself, then who it
// goes to, with the steps' dots at the dialog's foot. Where the firm has no
// other lawyer, the Matter is the only step.
const STEPS = ["The Matter", "Share with"];

export function NewMatterDialog({ onClose, onCreated }) {
	const [step, setStep] = useState(0);
	// The first step's height, which the second keeps, so the dialog does not
	// jump between them.
	const fieldsRef = useRef(null);
	const [stepHeight, setStepHeight] = useState(null);
	const [name, setName] = useState("");
	// Its mark, chosen here or left as it starts: a colour of the palette and
	// its initials.
	const [look, setLook] = useState(() => ({ color: projectColors[Math.floor(Math.random() * projectColors.length)], icon: null }));
	const [client, setClient] = useState("");
	const [about, setAbout] = useState("");
	const [folder, setFolder] = useState(null);
	const [chosen, setChosen] = useState([]);
	const [error, setError] = useState(null);
	const lawyers = teamMembers().filter(member => member.kind != "ai_colleague" && member.id != viewerId
		&& (member.actorId ?? "").startsWith("seat:"));
	const mayChooseFolder = hasNative() && typeof nativeApp()?.chooseNewMatterFolder == "function";
	const hasShare = lawyers.length > 0;
	const hasName = name.trim() != "";

	async function chooseFolder() {
		try {
			const picked = await nativeApp().chooseNewMatterFolder();
			if (picked == null) return;
			if (picked.holdsMatter == true) {
				setError(`${picked.name} holds a Matter already. Open it from Matters instead.`);
				return;
			}
			setFolder(picked);
			setError(null);
			if (name.trim() == "") setName(picked.name);
		} catch (reason) {
			setError(reason?.message ?? "The folder could not be chosen. Try again.");
		}
	}

	function toggle(member) {
		setChosen(list => list.includes(member.actorId) ? list.filter(actor => actor != member.actorId) : [...list, member.actorId]);
	}

	function make() {
		if (hasName == false) {
			setStep(0);
			setError("Name the Matter as the firm will call it.");
			return;
		}
		const job = { name: name.trim(), client, about, folderToken: folder?.token ?? null, shareWith: chosen, look };
		const firstNames = new Map(lawyers.map(member => [member.actorId, member.name.split(/\s+/)[0]]));
		onClose();
		openInBackground(job, firstNames, onCreated);
	}

	// The Matter named, it goes on to whom it goes to, or opens where there is
	// no one to share it with.
	function next() {
		if (hasName == false) {
			setError("Name the Matter as the firm will call it.");
			return;
		}
		if (hasShare) go(1);
		else make();
	}

	function go(at) {
		if (step == 0 && fieldsRef.current != null) setStepHeight(fieldsRef.current.offsetHeight);
		setStep(at);
	}

	const isLast = step == 1 || hasShare == false;
	return (
		<Dialog
			layout="spacious"
			title={step == 0 ? "New Matter" : `Share ${name.trim()}`}
			description={step == 0 ? "What the firm calls it, and where its files are" : "Each lawyer you choose gets it on their computers, end‑to‑end encrypted"}
			initialFocus="input"
			onClose={onClose}
			actions={(
				<>
					{hasShare ? (
						<Dots className={styles.steps} steps={STEPS} current={step} label="Opening a new Matter"
							canGo={at => at != step && hasName} onGo={go} />
					) : null}
					{step == 0 ? <Button onClick={onClose}>Cancel</Button> : <Button onClick={() => go(0)}>Back</Button>}
					{isLast
						? <Button variant="primary" disabled={hasName == false} onClick={make}>Open the Matter</Button>
						: <Button variant="primary" disabled={hasName == false} onClick={next}>Next</Button>}
				</>
			)}
		>
			{step == 0 ? (
				<div ref={fieldsRef} className={styles.fields}>
					<Field label="Name" error={error ?? undefined}>
						<div className={styles.named}>
							<MarkChooser name={name.trim() || "New Matter"} look={look} onChange={setLook} />
							<Input value={name} maxLength={120} spellCheck={false} autoComplete="off" placeholder="Westbrook Capital, Series B"
								onChange={event => { setName(event.target.value); setError(null); }}
								onKeyDown={event => { if (event.key == "Enter") next(); }} />
						</div>
					</Field>
					<Field label="Client" optional>
						<Input value={client} maxLength={120} autoComplete="off" onChange={event => setClient(event.target.value)} />
					</Field>
					<Field label="About" optional>
						<TextArea value={about} minRows={3} maxRows={8} maxLength={1200} placeholder="What the work is"
							onChange={event => setAbout(event.target.value)} />
					</Field>
					{mayChooseFolder ? (
						<div className={styles.group}>
							<span className={styles.label}>Folder</span>
							<div className={styles.folder}>
								<div className={styles.folderText}>
									<span className={styles.folderName}>{folder != null ? folder.name : "A new folder"}</span>
									<span className={styles.folderPath}>{folder != null ? folder.path : "Made in your Singularity folder, named after the Matter"}</span>
								</div>
								<Button size="small" onClick={chooseFolder}>{folder != null ? "Change" : "Choose"}</Button>
							</div>
						</div>
					) : null}
				</div>
			) : (
				<ul className={styles.people} aria-label="Share with" style={stepHeight != null ? { minHeight: stepHeight } : undefined}>
					{lawyers.map(member => {
						const isChosen = chosen.includes(member.actorId);
						return (
							<li key={member.id}>
								<button className={styles.person} type="button" aria-pressed={isChosen} onClick={() => toggle(member)}>
									<Avatar member={member} size={32} />
									<span className={styles.personText}>
										<span className={styles.personName}>{member.name}</span>
										<span className={styles.personLine}>{positionOf(member)}</span>
									</span>
									<span className={styles.check} data-on={isChosen || undefined} aria-hidden="true">
										{isChosen ? <Icon name="check" size={14} /> : null}
									</span>
								</button>
							</li>
						);
					})}
				</ul>
			)}
		</Dialog>
	);
}

let opened = 0;

// The Matter made and given, said at the window's top while it goes on, and
// then with the way to it.
async function openInBackground(job, firstNames, onOpen) {
	const key = `new-matter:${++opened}`;
	notice(key, { text: `Opening ${job.name}`, tone: "working" });
	try {
		const made = await createFirmMatter(job);
		const sent = made.given.filter(entry => entry.given == true).map(entry => firstNames.get(entry.actorId) ?? "your colleague");
		const failed = made.given.filter(entry => entry.given != true);
		dropNotice(key);
		toast(sent.length == 0 ? `${job.name} is open.`
			: `${job.name} is open, and on its way to ${sent.length == 1 ? sent[0] : `${sent.slice(0, -1).join(", ")} and ${sent.at(-1)}`}.`,
		{ action: { label: "Open", run: () => onOpen?.(made.uiId) }, duration: 8000 });
		if (failed.length > 0) {
			toast(`${job.name} could not go to ${firstNames.get(failed[0].actorId) ?? "a colleague"}: ${failed[0].problem || "try Share on its page."}`,
				{ tone: "seal" });
		}
	} catch (reason) {
		endNotice(key, { text: reason?.message ?? `${job.name} could not be opened. Try again.`, tone: "problem" });
	}
}
