import { addressOf } from "../Navigation/navigation.js";
import { realDocument, realLetter, realWork, shownMatters, shownMembers, shownThreads } from "../../data/native.js";
import { findMail } from "../../data/mail.js";
import { instruments } from "../../data/instruments.js";

// What a place is, as the sidebar names it: its name, its Matter and its sign.
// A disposable description of a place open in this window; the native records
// remain truth.
export function describePlace(place) {
	let name, matterId, memberId, icon;
	if (place.kind == "thread") {
		const thread = shownThreads().find(item => item.id == place.threadId);
		if (thread == null) return null;
		name = thread.title; matterId = thread.matterId; icon = "notes";
	} else if (place.kind == "messages" && place.memberId != null) {
		const member = shownMembers().find(item => item.id == place.memberId);
		if (member == null) return null;
		name = member.name; memberId = member.id; icon = "messages";
	} else if (place.kind == "mail") {
		const mail = findMail(place.mailId);
		if (mail == null) return null;
		name = mail.subject; matterId = mail.matterId; icon = "mail";
	} else if (place.kind == "letter") {
		const letter = realLetter(place.letterId);
		if (letter == null) return null;
		name = letter.subject || "Letter"; matterId = realWork().uiMatterId; icon = "mail";
	} else if (place.kind == "document") {
		const document = realDocument(place.documentId);
		const instrument = instruments[place.documentId];
		name = document?.relativePath?.replaceAll("\\", "/").split("/").pop() ?? instrument?.file;
		if (!name) return null;
		matterId = document != null ? realWork().uiMatterId : instrument?.workProductId != null ? shownThreads().find(thread => thread.posts.some(post => post.workProductId == instrument.workProductId))?.matterId : null;
		icon = "document";
	} else return null;
	const matter = shownMatters().find(item => item.id == matterId);
	return { key: addressOf(place), place, name, matterId: matter?.id ?? null, detail: matter?.name ?? null, memberId, icon };
}
