// A problem reported to Legal Machines from Singularity. The bug icon shows
// in a Debug build, Legal Machines' own, and where the seat's firm is a
// design partner (data/designPartner.js), in any build; the native side sends
// the report with this computer's sign-in to the board Legal Machines keeps
// for this version's line (account.reportProblem), which labels a design
// partner's. The technical details are the build, the computer and the
// window's state - never a Matter's or a client's name, a document's words, a
// credential or anything a request said.

import { useEffect, useState } from "react";

import { useDesignPartner } from "./designPartner.js";
import { nativeApp } from "./native.js";

// What a report is about, in words a member uses. Each goes by its id;
// Legal Machines' board labels it in its own, normalised words ("ux" is "UI
// / UX"), which the mail server's intake keeps.
export const PROBLEM_CATEGORIES = [
	{ id: "ux", words: "Hard to use or confusing" },
	{ id: "visual", words: "How it looks" },
	{ id: "ai", words: "AI colleagues and their answers" },
	{ id: "anonymization", words: "Client data protection" },
	{ id: "documents", words: "Documents and files" },
	{ id: "office", words: "Word, Excel or Outlook" },
	{ id: "exchange", words: "Sharing between the firm's computers" },
	{ id: "billing", words: "Billing and time" },
	{ id: "account", words: "Signing in and the firm's account" },
	{ id: "security", words: "Security" },
	{ id: "performance", words: "Slow, frozen or closed by itself" },
	{ id: "install", words: "Installing or updating" },
	{ id: "other", words: "Something else" },
];

// Whether this window can send a report: the native call is there.
export function canReportProblems() {
	const api = typeof window == "undefined" ? null : window.nativeApp;
	return typeof api?.reportProblem == "function";
}

// Which build this window runs in, as runtime info says it: "Debug" or
// "Release", read once.
let build = null;
function buildOf() {
	if (build == null)
		build = Promise.resolve()
			.then(() => (typeof window == "undefined" ? null : window.nativeApp?.getRuntimeInfo?.()))
			.then(info => info?.build ?? "", () => "");
	return build;
}

// For tests: forget the build read.
export function forgetBuild() {
	build = null;
}

// Whether the bug icon shows: in a Debug build, or for a design partner's seat.
export function useCanReportProblems() {
	const isDesignPartner = useDesignPartner();
	const [isDebug, setDebug] = useState(false);
	useEffect(() => {
		let isLive = true;
		buildOf().then(found => isLive && setDebug(found == "Debug"));
		return () => {
			isLive = false;
		};
	}, []);
	return canReportProblems() && (isDesignPartner || isDebug);
}

// The window's own errors since it opened, for the report: the last few, as
// the script said them, never what the page showed in them.
const MOST_ERRORS = 5;
const errors = [];
function keep(kind, message, where) {
	errors.push({ kind, message: String(message ?? "").slice(0, 300), where: String(where ?? "").slice(0, 200), at: new Date().toISOString() });
	if (errors.length > MOST_ERRORS) errors.shift();
}
if (typeof window != "undefined") {
	window.addEventListener("error", event => keep("error", event.message, event.filename ? `${event.filename}:${event.lineno}:${event.colno}` : ""));
	window.addEventListener("unhandledrejection", event => keep("rejection", event.reason?.message ?? event.reason, event.reason?.stack?.split("\n")[1]?.trim() ?? ""));
}

// The report's technical details, as lines a board reads: each fact the
// member sees in the dialog before it goes.
export async function technicalDetails() {
	const api = nativeApp();
	const info = await api.getRuntimeInfo?.().catch(() => null);
	const protection = await api.getPapinianModelStatus?.().catch(() => null);
	const zone = Intl.DateTimeFormat().resolvedOptions().timeZone;
	const facts = [
		["Singularity", `${info?.appVersion ?? "unknown"}, ${info?.build ?? "unknown"} build, Native API v${info?.apiVersion ?? "?"}`],
		["System", `${document.documentElement.dataset.nativePlatform ?? info?.platform ?? "unknown"}; ${navigator.userAgent}`],
		["Window", `${window.innerWidth} x ${window.innerHeight} at ${window.devicePixelRatio}x`],
		["Page", window.location.hash || "/"],
		["Language and time zone", `${navigator.language}, ${zone}`],
		["Client data protection", protection?.status ?? "unknown"],
		["Sent at", new Date().toISOString()],
	];
	const lines = facts.map(([name, value]) => `- ${name}: ${value}`);
	if (errors.length > 0) {
		lines.push("- The window's errors since it opened:");
		for (const error of errors) lines.push(`  - ${error.at} ${error.kind}: ${error.message}${error.where ? ` (${error.where})` : ""}`);
	}
	return lines.join("\n");
}

// A picture the member added, as the report carries it: its name and its
// bytes in base64.
function pictureOf(file) {
	return new Promise((resolve, reject) => {
		const reader = new FileReader();
		reader.onload = () => resolve({ name: file.name || "screenshot.png", data: String(reader.result).replace(/^data:[^,]*,/, "") });
		reader.onerror = () => reject(new Error(`${file.name || "A screenshot"} could not be read.`));
		reader.readAsDataURL(file);
	});
}

// The version of the Diagnostics Consent in force: a design partner's seat
// accepts it, by its checkbox, before its first bug report or work log.
// Legal Machines' account service keeps the version each seat accepted.
export const DIAGNOSTICS_CONSENT_VERSION = "1";

// Sends the report: {number} of its card on the board. With
// `acceptedConsent`, the seat accepts that version of the Diagnostics Consent
// first, and nothing is sent unless that is kept.
export async function reportProblem({ title, category, description, technical, pictures, acceptedConsent }) {
	const carried = await Promise.all((pictures ?? []).map(pictureOf));
	return nativeApp().reportProblem({ title, category, description, technical, pictures: carried, acceptedConsent });
}
