import { useEffect, useState } from "react";

import { Button } from "../Button/Button.jsx";
import { Dialog } from "../Dialog/Dialog.jsx";
import { Checkbox, Field, TextArea } from "../Field/Field.jsx";
import { toast } from "../Toast/toast.js";
import { noteDiagnosticsConsent, useSeatFacts } from "../../data/designPartner.js";
import { DIAGNOSTICS_CONSENT_VERSION } from "../../data/problems.js";
import { canShareWorkLogs, shareWorkLog, whenOf, workLogPreview } from "../../data/workLog.js";
import { LEGAL_LINKS } from "../../onboarding/legal.js";
import styles from "./ShareWorkLog.module.css";

// Whether a conversation or thread offers its work log: where the seat's own
// record says this computer keeps one, on a design partner's seat with a build
// that can seal it to Legal Machines (D-257).
export function useCanShareWorkLog() {
	const facts = useSeatFacts();
	return facts.workLogs == true && canShareWorkLogs();
}

// A design partner's member sends Legal Machines one conversation's or
// thread's work log (D-257): what it holds and how it is protected are said
// before anything goes, the Diagnostics Consent is asked once for this seat,
// and nothing leaves this computer unsealed.
export function ShareWorkLogDialog({ matterId, chatId, title, messages, onClose }) {
	const [preview, setPreview] = useState(null);
	const [problem, setProblem] = useState(null);
	const [note, setNote] = useState("");
	const [consents, setConsents] = useState(false);
	const [isSending, setSending] = useState(false);
	const facts = useSeatFacts();
	const needsConsent = facts.diagnosticsConsent != DIAGNOSTICS_CONSENT_VERSION;

	useEffect(() => {
		let isLive = true;
		workLogPreview(matterId, chatId).then(
			found => isLive && setPreview(found),
			failure => isLive && setProblem(failure?.userMessage ?? failure?.message ?? "This conversation's work log could not be read."),
		);
		return () => {
			isLive = false;
		};
	}, [matterId, chatId]);

	const canSend = preview?.canSend == true && (needsConsent == false || consents) && isSending == false;

	async function send() {
		if (canSend == false) return;
		setSending(true);
		setProblem(null);
		try {
			await shareWorkLog({
				matterId,
				chatId,
				chatTitle: title ?? "",
				note: note.trim(),
				messages,
				acceptedConsent: needsConsent ? DIAGNOSTICS_CONSENT_VERSION : undefined,
			});
			if (needsConsent) noteDiagnosticsConsent(DIAGNOSTICS_CONSENT_VERSION);
			toast("The work log went to Legal Machines, sealed so only Legal Machines can open it. Thank you.");
			onClose();
		} catch (failure) {
			setProblem(failure?.userMessage ?? failure?.message ?? "The work log could not be sent. Try again.");
		}
		setSending(false);
	}

	// The consent Send waits on, and what went wrong, over the actions, never
	// under the scroll, as the bug report's (the owner, 2026-11-05).
	const asksConsent = preview?.canSend == true && needsConsent;
	const footnote = asksConsent || problem != null ? (
		<>
			{asksConsent ? (
				<Checkbox
					label="I accept the Diagnostics Consent"
					description={(
						<>
							It covers what you send Legal Machines from this seat: work logs like this one, and bug reports.{" "}
							<a href={LEGAL_LINKS.diagnostics} target="_blank" rel="noreferrer">Read it</a>
						</>
					)}
					checked={consents}
					onChange={setConsents}
				/>
			) : null}
			{problem != null ? <p className={styles.error} role="alert">{problem}</p> : null}
		</>
	) : null;

	return (
		<Dialog
			layout="spacious"
			title="Send the work log to Legal Machines"
			description="Everything Singularity and its AI models did in this conversation, so Legal Machines can see what went wrong and fix it."
			onClose={onClose}
			footnote={footnote}
			actions={(
				<>
					<Button onClick={onClose}>Cancel</Button>
					<Button variant="primary" busy={isSending} disabled={canSend == false} onClick={send}>Send</Button>
				</>
			)}
		>
			<div className={styles.form}>
				{preview == null && problem == null ? <p className={styles.line}>Reading what this computer kept of this conversation...</p> : null}
				{preview != null ? <WhatGoes preview={preview} /> : null}
				{preview?.canSend == true ? (
					<Field label="What went wrong" optional>
						<TextArea value={note} minRows={3} maxRows={8} maxLength={8000} onChange={event => setNote(event.target.value)} />
					</Field>
				) : null}
			</div>
		</Dialog>
	);
}

// What the log holds and how it is protected, in plain words, before it goes.
function WhatGoes({ preview }) {
	if (preview.canSend == false) return <p className={styles.line}>{preview.reason}</p>;
	const requests = preview.calls == 1 ? "1 request" : `${preview.calls} requests`;
	const span = preview.from != "" && preview.to != "" ? `, from ${whenOf(preview.from)} to ${whenOf(preview.to)}` : "";
	return (
		<div className={styles.facts}>
			<p className={styles.line}>
				It holds the conversation's messages as written, and {requests} to AI models{span}, each exactly as it was sent, with
				its answer{preview.failedCalls > 0 ? `, and what failed in ${preview.failedCalls}` : ""}.
			</p>
			<p className={styles.line}>
				{preview.unprotectedCalls == 0
					? "Every request went with client data protection on: client details in them are masked."
					: `${preview.unprotectedCalls} of them went without client data protection, so they hold client details as written.`}
			</p>
			<p className={styles.line}>
				It is sealed on this computer so that only Legal Machines can open it: the servers that carry it cannot read it.
			</p>
		</div>
	);
}
