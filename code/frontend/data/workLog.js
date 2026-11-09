// A design partner's work log of one conversation or thread, sent to Legal
// Machines. This computer keeps the model calls of each chat while its seat's
// firm is a design partner; the member chooses to send one chat's, which the
// native side seals to Legal Machines' keys and sends (account.shareWorkLog).

import { nativeApp } from "./native.js";

// Whether this window can send a work log: the native calls are there.
export function canShareWorkLogs() {
	const api = typeof window == "undefined" ? null : window.nativeApp;
	return typeof api?.shareWorkLog == "function" && typeof api?.getWorkLogPreview == "function";
}

// What would be sent: {calls, protectedCalls, unprotectedCalls, failedCalls,
// from, to, bytes, canSend, reason}.
export function workLogPreview(matterId, chatId) {
	return nativeApp().getWorkLogPreview(matterId, chatId);
}

// Sends it: {sent, sha256, recipients, bytes}.
export function shareWorkLog({ matterId, chatId, chatTitle, note, messages, acceptedConsent }) {
	return nativeApp().shareWorkLog({ matterId, chatId, chatTitle, note, messages: messages ?? [], acceptedConsent });
}

// "12 May, 14:05" in the member's own time.
export function whenOf(stamp) {
	const at = new Date(stamp);
	if (Number.isNaN(at.getTime())) return "";
	return at.toLocaleString(undefined, { day: "numeric", month: "short", hour: "2-digit", minute: "2-digit" });
}
