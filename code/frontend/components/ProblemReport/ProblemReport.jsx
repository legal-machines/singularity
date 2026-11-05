import { useEffect, useRef, useState } from "react";

import { Button } from "../Button/Button.jsx";
import { Dialog } from "../Dialog/Dialog.jsx";
import { Checkbox, Field, Input, Select, TextArea } from "../Field/Field.jsx";
import { Icon } from "../Icon/Icon.jsx";
import { toast } from "../Toast/toast.js";
import { noteDiagnosticsConsent, useSeatFacts } from "../../data/designPartner.js";
import { DIAGNOSTICS_CONSENT_VERSION, PROBLEM_CATEGORIES, reportProblem, technicalDetails, useCanReportProblems } from "../../data/problems.js";
import { LEGAL_LINKS } from "../../onboarding/legal.js";
import styles from "./ProblemReport.module.css";

// The pictures a report takes, as the board keeps them.
const PICTURE_TYPES = ["image/png", "image/jpeg", "image/webp"];
const MOST_PICTURES = 3;
const MOST_PICTURE_BYTES = 10 * 1024 * 1024;

// The bug icon (the owner, 2026-11-05; D-257): a problem told straight to
// Legal Machines' board, with its screenshots and the build's details. It
// shows in a Debug build, Legal Machines' own, and for a design partner's
// seat in any build; nowhere else.
export function ProblemButton() {
	const [isOpen, setOpen] = useState(false);
	const canReport = useCanReportProblems();
	if (canReport == false) return null;
	return (
		<>
			<button className={styles.button} type="button" aria-label="Report a problem" data-tooltip="Report a problem" onClick={() => setOpen(true)}>
				<Icon name="bug" size={18} />
			</button>
			{isOpen ? <ProblemDialog onClose={() => setOpen(false)} /> : null}
		</>
	);
}

function ProblemDialog({ onClose }) {
	const [title, setTitle] = useState("");
	const [category, setCategory] = useState("");
	const [description, setDescription] = useState("");
	const [pictures, setPictures] = useState([]);
	const [technical, setTechnical] = useState("");
	const [isSending, setSending] = useState(false);
	const [error, setError] = useState(null);
	const chooser = useRef(null);

	useEffect(() => {
		let isLive = true;
		technicalDetails().then(text => isLive && setTechnical(text), () => {});
		return () => {
			isLive = false;
		};
	}, []);
	useEffect(() => () => {
		for (const picture of pictures) URL.revokeObjectURL(picture.preview);
	}, []);

	function add(files) {
		const taken = [...files].filter(file => PICTURE_TYPES.includes(file.type));
		if (taken.length < files.length) toast("A screenshot is a PNG, JPEG or WebP picture.");
		const fitting = taken.filter(file => file.size <= MOST_PICTURE_BYTES);
		if (fitting.length < taken.length) toast("A screenshot can be 10 MB at most.");
		setPictures(list => {
			const next = [...list, ...fitting.map(file => ({ file, preview: URL.createObjectURL(file) }))];
			if (next.length > MOST_PICTURES) toast("A report carries three screenshots at most.");
			for (const extra of next.slice(MOST_PICTURES)) URL.revokeObjectURL(extra.preview);
			return next.slice(0, MOST_PICTURES);
		});
	}
	function remove(index) {
		setPictures(list => {
			URL.revokeObjectURL(list[index].preview);
			return list.filter((_, at) => at != index);
		});
	}

	// A design partner's seat accepts the Diagnostics Consent before its first
	// report, once (D-257); Legal Machines' own Debug-build reports ask none.
	const facts = useSeatFacts();
	const needsConsent = facts.designPartner && facts.diagnosticsConsent != DIAGNOSTICS_CONSENT_VERSION;
	const [consents, setConsents] = useState(false);
	const isReady = title.trim() != "" && category != "" && (needsConsent == false || consents);

	async function send() {
		if (isReady == false || isSending) return;
		setSending(true);
		setError(null);
		try {
			const { number } = await reportProblem({
				title: title.trim(),
				category,
				description: description.trim(),
				technical,
				pictures: pictures.map(picture => picture.file),
				acceptedConsent: needsConsent ? DIAGNOSTICS_CONSENT_VERSION : undefined,
			});
			if (needsConsent) noteDiagnosticsConsent(DIAGNOSTICS_CONSENT_VERSION);
			toast(`Sent to Legal Machines' board as #${number}. Thank you.`);
			onClose();
		} catch (failure) {
			setError(failure?.userMessage ?? failure?.message ?? "The report could not be sent. Try again.");
		}
		setSending(false);
	}

	// The consent Send waits on, and what went wrong with a send, over the
	// actions, never under the form's scroll (the owner, 2026-11-05).
	const footnote = needsConsent || error != null ? (
		<>
			{needsConsent ? (
				<Checkbox
					label="I accept the Diagnostics Consent"
					description={(
						<>
							It covers what you send Legal Machines from this seat: bug reports like this one, and work logs.{" "}
							<a href={LEGAL_LINKS.diagnostics} target="_blank" rel="noreferrer">Read it</a>
						</>
					)}
					checked={consents}
					onChange={setConsents}
				/>
			) : null}
			{error != null ? <p className={styles.error} role="alert">{error}</p> : null}
		</>
	) : null;

	return (
		<Dialog
			layout="spacious"
			title="Report a problem"
			description="It goes straight to Legal Machines' board for this version, with the details of this build and this window."
			onClose={onClose}
			footnote={footnote}
			actions={(
				<>
					<Button onClick={onClose}>Cancel</Button>
					<Button variant="primary" busy={isSending} disabled={isReady == false} onClick={send}>Send</Button>
				</>
			)}
		>
			<div
				className={styles.form}
				onPaste={event => {
					const files = [...(event.clipboardData?.files ?? [])];
					if (files.length > 0) {
						event.preventDefault();
						add(files);
					}
				}}
				onDragOver={event => event.preventDefault()}
				onDrop={event => {
					event.preventDefault();
					add([...(event.dataTransfer?.files ?? [])]);
				}}
			>
				<Field label="What went wrong">
					<Input value={title} maxLength={120} placeholder="The bill shows no statement" onChange={event => setTitle(event.target.value)} />
				</Field>
				<Field label="What it's about">
					<Select value={category} aria-label="What it's about" onChange={event => setCategory(event.target.value)}>
						<option value="" disabled>Choose one</option>
						{PROBLEM_CATEGORIES.map(choice => <option key={choice.id} value={choice.id}>{choice.words}</option>)}
					</Select>
				</Field>
				<Field label="What you did, and what you expected" optional>
					<TextArea value={description} minRows={3} maxRows={8} onChange={event => setDescription(event.target.value)} />
				</Field>
				<div className={styles.pictures}>
					{pictures.map((picture, index) => (
						<div key={picture.preview} className={styles.picture}>
							<img src={picture.preview} alt="" />
							<button className={styles.picture_remove} type="button" aria-label={`Take ${picture.file.name} off`} onClick={() => remove(index)}>
								<Icon name="close" size={14} />
							</button>
						</div>
					))}
					{pictures.length < MOST_PICTURES ? (
						<button className={styles.add} type="button" onClick={() => chooser.current?.click()}>
							<Icon name="plus" size={16} />
							<span>Add a screenshot</span>
						</button>
					) : null}
					<input ref={chooser} className={styles.chooser} type="file" accept={PICTURE_TYPES.join(",")} multiple
						onChange={event => {
							add([...(event.target.files ?? [])]);
							event.target.value = "";
						}} />
				</div>
				<p className={styles.note}>Paste or drop a screenshot here too. Check it shows nothing of a client's you would not send.</p>
				<details className={styles.details}>
					<summary>What else goes with it</summary>
					<pre>{technical || "Reading this build's details..."}</pre>
				</details>
			</div>
		</Dialog>
	);
}
