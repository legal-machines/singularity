// Outlook, through Office's own script (office.js, Mailbox requirement sets):
// the letter open now - read or being written - what it says of itself, its
// attachments' bytes, and the one thing the pane does to a reply: attach a
// version. Everything else - the Matters, their letters, colleagues and
// threads - comes from Singularity (data.js).

const office = () => window.Office;
const mailbox = () => office()?.context?.mailbox;

// Whether this Outlook has a Mailbox requirement set, as it reports it.
export function hasMailbox(version) {
	try {
		return office()?.context?.requirements?.isSetSupported("Mailbox", version) == true;
	} catch {
		return false;
	}
}

// "read" for a letter received or sent, "compose" for one being written.
export function letterMode() {
	const item = mailbox()?.item;
	if (item == null) return "none";
	return Array.isArray(item.to) || item.from != null && typeof item.from.getAsync != "function" ? "read" : "compose";
}

// The mailbox's own address: whose letters these are.
export function ownAddress() {
	return mailbox()?.userProfile?.emailAddress ?? "";
}

// Calls back when the person selects another letter (a pinned pane).
export function onLetterChanged(callback) {
	const box = mailbox();
	if (box?.addHandlerAsync == null || !hasMailbox("1.5")) return () => {};
	const handler = () => callback();
	box.addHandlerAsync(office().EventType.ItemChanged, handler);
	return () => box.removeHandlerAsync(office().EventType.ItemChanged, { handler });
}

function asked(call) {
	return new Promise((resolve, reject) => {
		call(result => {
			if (result.status == office().AsyncResultStatus.Succeeded) resolve(result.value);
			else reject(new Error(result.error?.message || "Outlook did not answer."));
		});
	});
}

const partyOf = detail => (detail == null ? null : { name: detail.displayName ?? "", address: detail.emailAddress ?? "" });

// A draft's words as an HTML letter holds them: each paragraph, set apart by
// an empty line, a paragraph; each other line break a break; nothing in them
// read as markup.
export function htmlOfWords(text) {
	const escaped = text.replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/>/g, "&gt;").replace(/"/g, "&quot;");
	return escaped
		.split(/\r?\n[ \t]*\r?\n/)
		.filter(paragraph => paragraph.trim() != "")
		.map(paragraph => `<p>${paragraph.replace(/\r?\n/g, "<br>")}</p>`)
		.join("");
}

// The letter open in the reading pane, as Singularity files it: its
// identities, people, words and attachments (never their bytes).
export async function readLetter() {
	const item = mailbox().item;
	const text = await asked(done => item.body.getAsync(office().CoercionType.Text, done)).catch(() => "");
	return {
		internetMessageId: item.internetMessageId ?? "",
		conversationId: item.conversationId ?? "",
		mailbox: ownAddress(),
		subject: item.subject ?? "",
		from: partyOf(item.from),
		to: (item.to ?? []).map(partyOf),
		cc: (item.cc ?? []).map(partyOf),
		sentAt: item.dateTimeCreated != null ? new Date(item.dateTimeCreated).toISOString() : "",
		text: (text ?? "").slice(0, 200000),
		attachments: (item.attachments ?? [])
			.filter(attachment => attachment.isInline != true && attachment.attachmentType == office().MailboxEnums.AttachmentType.File)
			.map(attachment => ({ name: attachment.name, contentType: attachment.contentType ?? "", size: attachment.size ?? 0, providerId: attachment.id })),
	};
}

// An attachment's bytes, as Base64 (Mailbox 1.8).
export async function attachmentBase64(providerId) {
	const content = await asked(done => mailbox().item.getAttachmentContentAsync(providerId, done));
	if (content.format != office().MailboxEnums.AttachmentContentFormat.Base64) throw new Error("Outlook gave this attachment in a form Singularity does not save.");
	return content.content;
}

// The letter being written: its conversation, subject, recipients, words and
// attachments (Mailbox 1.8 for the attachments).
export async function composedLetter() {
	const item = mailbox().item;
	const [subject, to, cc, text, attachments] = await Promise.all([
		asked(done => item.subject.getAsync(done)).catch(() => ""),
		asked(done => item.to.getAsync(done)).catch(() => []),
		asked(done => item.cc.getAsync(done)).catch(() => []),
		asked(done => item.body.getAsync(office().CoercionType.Text, done)).catch(() => ""),
		hasMailbox("1.8") ? asked(done => item.getAttachmentsAsync(done)).catch(() => []) : Promise.resolve([]),
	]);
	return {
		conversationId: item.conversationId ?? "",
		mailbox: ownAddress(),
		subject: subject ?? "",
		to: (to ?? []).map(partyOf),
		cc: (cc ?? []).map(partyOf),
		text: text ?? "",
		attachments: (attachments ?? []).filter(attachment => attachment.isInline != true).map(attachment => ({ name: attachment.name, size: attachment.size ?? 0 })),
	};
}

// Attaches a file to the letter being written (Mailbox 1.8). It never sends.
export function attachToLetter(name, base64) {
	return asked(done => mailbox().item.addFileAttachmentFromBase64Async(base64, name, { isInline: false }, done));
}

// Puts a colleague's email draft into the letter being written, at the
// person's press (D-119): its subject and words, its recipients where it names
// any, and its attachments, each as the draft carries it. The person reads it,
// edits it and sends it with Outlook's own Send; this never sends.
export async function writeIntoLetter(draft) {
	const item = mailbox().item;
	const addresses = text => (text ?? "").split(/[,;]/).map(part => part.trim()).filter(part => part.includes("@"))
		.map(part => {
			const found = /^(.*)<([^>]+)>$/.exec(part);
			return found != null ? { displayName: found[1].trim().replace(/^"|"$/g, ""), emailAddress: found[2].trim() } : part;
		});
	await asked(done => item.subject.setAsync(draft.subject ?? "", done));
	// A letter Outlook writes as HTML takes the words as paragraphs: given as
	// text, Outlook for Mac ran them into one.
	const type = await asked(done => item.body.getTypeAsync(done)).catch(() => office().CoercionType.Text);
	if (type == office().CoercionType.Html) {
		await asked(done => item.body.setAsync(htmlOfWords(draft.body ?? ""), { coercionType: office().CoercionType.Html }, done));
	} else {
		await asked(done => item.body.setAsync(draft.body ?? "", { coercionType: office().CoercionType.Text }, done));
	}
	if (addresses(draft.to).length > 0) await asked(done => item.to.setAsync(addresses(draft.to), done));
	if (addresses(draft.cc).length > 0) await asked(done => item.cc.setAsync(addresses(draft.cc), done));
	for (const attachment of draft.attachments ?? []) await attachToLetter(attachment.name, attachment.base64);
}
