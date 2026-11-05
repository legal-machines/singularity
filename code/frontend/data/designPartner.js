// What this seat's own record says of sending Legal Machines its bug reports
// and work logs (D-257): whether its firm is one of Legal Machines' design
// partners (the owner, 2026-11-05: "design partner is a fact of the account on
// the backend, so the logs and the bugs buttons show in the app, and the
// backend gives API access, only when they are sent from design partners'
// seats"), a mark Legal Machines alone sets on the firm; whether this
// computer keeps and offers work logs, which also needs a build holding Legal
// Machines' keys for them; and which version of the Diagnostics Consent this
// seat accepted ("before sending, on each seat, a
// checkbox: a separate consent to sending this data"), or none. Read from
// account.ownSeat once a window and again when the window comes back after ten
// minutes; nothing until read, and where it cannot be read.

import { useEffect, useState } from "react";

import { hasNative, nativeApp } from "./native.js";

const STALE_AFTER_MS = 10 * 60 * 1000;
const NONE = Object.freeze({ designPartner: false, workLogs: false, diagnosticsConsent: null });

let known = NONE;
let readAt = 0;
let reading = null;
const listeners = new Set();

function tell(facts) {
	known = facts;
	for (const listener of listeners) listener(facts);
}

// Reads the seat's record, once at a time.
export function readSeatFacts() {
	const api = hasNative() ? nativeApp() : null;
	if (typeof api?.getOwnSeat != "function") return Promise.resolve(NONE);
	if (reading != null) return reading;
	reading = Promise.resolve()
		.then(() => api.getOwnSeat())
		.then(
			seat => ({
				designPartner: seat?.designPartner === true,
				workLogs: seat?.workLogs === true,
				diagnosticsConsent: typeof seat?.diagnosticsConsent == "string" ? seat.diagnosticsConsent : null,
			}),
			() => known,
		)
		.then(facts => {
			readAt = Date.now();
			reading = null;
			tell(facts);
			return facts;
		});
	return reading;
}

// The seat has just accepted this version of the Diagnostics Consent: said at
// once, without waiting for the next reading.
export function noteDiagnosticsConsent(version) {
	tell({ ...known, diagnosticsConsent: version });
}

// For tests: forget what was read.
export function forgetDesignPartner() {
	known = NONE;
	readAt = 0;
	reading = null;
}

export function useSeatFacts() {
	const [facts, setFacts] = useState(known);
	useEffect(() => {
		listeners.add(setFacts);
		if (readAt == 0 || Date.now() - readAt > STALE_AFTER_MS) readSeatFacts();
		const again = () => {
			if (Date.now() - readAt > STALE_AFTER_MS) readSeatFacts();
		};
		window.addEventListener("focus", again);
		return () => {
			listeners.delete(setFacts);
			window.removeEventListener("focus", again);
		};
	}, []);
	return facts;
}

export function useDesignPartner() {
	return useSeatFacts().designPartner;
}
