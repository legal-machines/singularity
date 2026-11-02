import { useEffect, useState } from "react";

import { Segments } from "../DocumentLens/DocumentLens.jsx";
import { IconButton } from "../Button/Button.jsx";
import { maskText, sentSegments } from "../../data/masking.js";
import { toggleProviderView, useProviderView } from "./providerView.js";

import styles from "./ProviderView.module.css";

// A conversation as an AI provider receives it (D-233): each message, and the
// draft before it is sent, with Papinian Guard's symbols where its words
// stood. The Guard runs here, on this computer, on the Project's identities;
// nothing is sent to show it.

// The button in the conversation's corner that shows it so, or as written.
export function ProviderViewButton({ chatId }) {
	const isOn = useProviderView(chatId);
	return (
		<IconButton icon="eye" pressed={isOn}
			label={isOn ? "Show it as written" : "Show it as an AI provider receives it"}
			onClick={() => toggleProviderView(chatId)} />
	);
}

// The masked text of `text` from `projectId`, asked `wait` ms after it last
// changed; null while it is read, { problem } when it cannot be.
function useMasked(projectId, text, wait = 0) {
	const [masked, setMasked] = useState(null);
	useEffect(() => {
		if (projectId == null || (text ?? "").trim() == "") {
			setMasked(null);
			return undefined;
		}
		let isLive = true;
		const timer = setTimeout(() => {
			maskText(projectId, text).then(
				result => { if (isLive) setMasked({ ...result, text }); },
				error => { if (isLive) setMasked({ problem: error?.userMessage ?? error?.message ?? "Papinian Guard could not read it.", text }); },
			);
		}, wait);
		return () => {
			isLive = false;
			clearTimeout(timer);
		};
	}, [projectId, text, wait]);
	return masked;
}

// A message's words as they go out. Until the Guard has read them, they stand
// quiet, as written.
export function MaskedText({ projectId, text }) {
	const masked = useMasked(projectId, text);
	if (masked == null || masked.text != text) return <p className={styles.waiting}>{text}</p>;
	if (masked.problem != null) return <p className={styles.problem}>{masked.problem}</p>;
	return <p className={styles.sent}><Segments segments={sentSegments(masked.protectedText, masked.replacements)} isSent /></p>;
}

// The draft as it would go out, over the reply box, read again a moment after
// each change: before it is sent (the owner: "this must happen before
// sending").
export function MaskedDraft({ projectId, text }) {
	const masked = useMasked(projectId, text, 350);
	if ((text ?? "").trim() == "") return null;
	const isCurrent = masked != null && masked.text == text;
	return (
		<div className={styles.draft} aria-live="polite">
			<p className={styles.draft_label}>{masked?.problem != null ? "Papinian Guard" : masked?.isOn === false ? "Goes out as written: Papinian Guard is off on this computer" : "Goes out as"}</p>
			{masked?.problem != null ? <p className={styles.problem}>{masked.problem}</p>
				: <p className={isCurrent ? styles.sent : styles.waiting}>{isCurrent ? <Segments segments={sentSegments(masked.protectedText, masked.replacements)} isSent /> : text}</p>}
		</div>
	);
}
