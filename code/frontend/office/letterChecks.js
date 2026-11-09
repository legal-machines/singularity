// What Outlook's pane says of a reply before the person sends it: the version
// of each agreement the Matter holds now, and what to raise - an attachment
// that is not that version, words that say something is attached when nothing
// is, a recipient who has not written in the Matter before. Pure: it reads
// what it is given.

// The versions the Matter holds now, one for each agreement a colleague
// reviewed: the latest accepted, else the latest waiting for a decision.
export function versionsOf(data) {
	const shown = [];
	for (const review of data.reviews ?? []) {
		const versions = [...(review.versions ?? [])].filter(version => version.resultDocumentId);
		const accepted = versions.filter(version => version.state == "accepted").at(-1);
		const waiting = versions.filter(version => version.state == "ready_for_review").at(-1);
		const version = accepted ?? waiting ?? null;
		if (version == null) continue;
		const title = (review.sourceFileName || "the agreement").replace(/\.[^.]+$/, "");
		const decided = (review.decisions ?? []).filter(decision => decision.version == version.number && decision.decision == "accepted")
			.at(-1)?.decidedAt ?? "";
		shown.push({
			documentId: version.resultDocumentId,
			fileName: version.resultFileName,
			review,
			number: version.number,
			isAccepted: accepted != null,
			words: accepted != null
				? `Version ${version.number} of ${title}, accepted${decided ? ` ${dayWords(decided)}` : ""}`
				: `Version ${version.number} of ${title}, still waiting for your decision`,
		});
	}
	return shown.slice(0, 3);
}

// What to raise before the letter goes: an attachment that is not the version
// the Matter holds now, words that say something is attached when nothing is,
// and a recipient who has not written in the Matter before.
export function checksOf(draft, letters, versions, matterName) {
	const checks = [];
	const names = draft.attachments.map(attachment => (attachment.name ?? "").toLowerCase());
	for (const version of versions.filter(entry => entry.isAccepted)) {
		const review = version.review;
		const older = (review.versions ?? []).filter(entry => entry.number < version.number && entry.resultFileName);
		for (const entry of older) {
			if (names.includes(entry.resultFileName.toLowerCase())) {
				checks.push({ tone: "warn", words: `${entry.resultFileName} is version ${entry.number}; version ${version.number} was accepted since.` });
			}
		}
		if (review.sourceFileName && names.includes(review.sourceFileName.toLowerCase())) {
			checks.push({ tone: "warn", words: `${review.sourceFileName} is the agreement before the review; version ${version.number} was accepted since.` });
		}
	}
	// Only the words written now, not the letter quoted under them.
	const written = draft.text.split(/\n(?:From:|-{2,}\s*Original Message|On .{4,80} wrote:)/)[0] ?? "";
	if (draft.attachments.length == 0 && /\b(attached|attachment|enclosed|enclosure)\b/i.test(written)) {
		checks.push({ tone: "warn", words: "The letter says something is attached, and nothing is." });
	}
	const known = new Set([draft.mailbox.toLowerCase()]);
	for (const letter of letters) {
		for (const party of [letter.from, ...(letter.to ?? []), ...(letter.cc ?? [])]) {
			if (party?.address) known.add(party.address.toLowerCase());
		}
	}
	for (const party of [...draft.to, ...draft.cc]) {
		const address = (party?.address ?? "").toLowerCase();
		if (address == "" || known.has(address)) continue;
		checks.push({ tone: "note", words: `${party.name || party.address} (${party.address}) has not written in ${matterName} before.` });
	}
	return checks;
}

export function dayWords(moment) {
	if (!moment) return "on a day Outlook did not give";
	const date = new Date(moment);
	if (Number.isNaN(date.getTime())) return moment;
	return date.toLocaleDateString("en-US", { month: "short", day: "numeric", year: date.getFullYear() == new Date().getFullYear() ? undefined : "numeric" });
}
