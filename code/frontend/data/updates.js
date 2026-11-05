import { useSyncExternalStore } from "react";

import { endNotice, notice, toast } from "../components/Toast/toast.js";
import { hasNative, nativeApp } from "./native.js";
import { sizeWords } from "./transfers.js";

// Singularity's new versions come from Legal Machines' own site (the owner,
// 2026-11-02, going public: "do all of this"). The backend asks the site for
// the latest version for this computer's system and processor and keeps it
// only when Legal Machines' release key signed it (Node/AppUpdate). From 0.1.1
// on Singularity installs it itself (D-261; the owner, 2026-11-05: "make it so
// that from version 0.1.1 everything is automatic"): the window asks a little
// after Singularity opens and every few hours, and a version found downloads
// on its own, quietly; once it is readied, it is said once, calmly, with
// Restart now, and it installs as Singularity ends - then, or the next time
// Singularity closes. Where it cannot install itself, it says why, with the
// way to the installer. Settings shows it at all times. A check or a download
// of its own that did not succeed is never an alarm: it says nothing, and
// Settings keeps the way to try again.

const FIRST_CHECK_MILLISECONDS = 20 * 1000;
const EVERY_MILLISECONDS = 6 * 60 * 60 * 1000;
const PROGRESS_MILLISECONDS = 500;
// A version said once a day at most, if the member let it pass.
const TOLD_KEY = "singularity.updateTold";
const TOLD_AGAIN_MILLISECONDS = 24 * 60 * 60 * 1000;
export const COULD_NOT_CHECK = "Couldn't check for updates. Try again later.";

let current = {
	state: "unknown", currentVersion: "", checkedAt: "", received: 0, installer: "", detail: "", release: null,
	// Whether the last check was the member's own asking.
	isAsked: false,
};
const listeners = new Set();
let isWatching = false;
let isDownloading = false;

function publish(next) {
	current = { ...current, ...next };
	for (const listener of listeners) listener();
}

export function useUpdates() {
	return useSyncExternalStore(listener => {
		listeners.add(listener);
		return () => listeners.delete(listener);
	}, () => current);
}

export function updates() {
	return current;
}

function hasUpdates() {
	return hasNative() && typeof nativeApp().checkForUpdate == "function";
}

// Begins looking, once; safe to call again.
export function watchUpdates() {
	if (isWatching || hasUpdates() == false) return;
	isWatching = true;
	nativeApp().getUpdateStatus().then(status => {
		publish(status);
		tellLastInstall(status);
	}).catch(() => {});
	setTimeout(() => checkForUpdates(), FIRST_CHECK_MILLISECONDS);
	setInterval(() => checkForUpdates(), EVERY_MILLISECONDS);
}

// Asks the site now: `isAsked` when the member asked, in Settings. A version
// found downloads on its own where this build installs itself; a build that
// does not says it, with the way to download it.
export async function checkForUpdates({ isAsked = false } = {}) {
	if (hasUpdates() == false) return current;
	const isKept = current.state == "downloading" || current.state == "downloaded" || current.state == "ready";
	publish({ state: isKept ? current.state : "checking", isAsked });
	try {
		const status = await nativeApp().checkForUpdate();
		publish({ ...status, isAsked });
		if (status.state == "available" && status.release != null) {
			if (status.automatic) downloadUpdate({ isQuiet: true });
			else if (isAsked == false) tellAvailable(status.release);
		}
	} catch {
		publish({ state: isKept ? current.state : "offline", detail: COULD_NOT_CHECK, isAsked });
	}
	return current;
}

// Whether `release` may be said now: once a day at most, if the member let it
// pass; and from now on it counts as said.
function mayTell(release, now, storage) {
	let told = null;
	try {
		told = JSON.parse(storage?.getItem(TOLD_KEY) ?? "null");
	} catch {
		told = null;
	}
	if (told?.version == release.version && now - (told.at ?? 0) < TOLD_AGAIN_MILLISECONDS) return false;
	try {
		storage?.setItem(TOLD_KEY, JSON.stringify({ version: release.version, at: now }));
	} catch {
		// Said again next time.
	}
	return true;
}

// A new version, said calmly, where this build does not install itself: a
// passing notice with the way to download it. Settings keeps saying it.
export function tellAvailable(release, { now = Date.now(), storage = safeStorage() } = {}) {
	if (mayTell(release, now, storage) == false) return false;
	toast(`Singularity ${release.version} is available.`, { duration: 12000, action: { label: "Download", run: () => downloadUpdate() } });
	return true;
}

// A version readied to install itself, said calmly once a day at most, with
// Restart now; otherwise it installs the next time Singularity closes.
// Settings keeps saying it.
export function tellReady(release, words, { now = Date.now(), storage = safeStorage(), restart = restartToUpdate } = {}) {
	if (mayTell(release, now, storage) == false) return false;
	toast(`Singularity ${release.version} is ready. ${words}`.trim(), { duration: 15000, action: { label: "Restart now", run: () => restart() } });
	return true;
}

// Restart now: Singularity ends, the version readied installs, and the new
// version opens.
export async function restartToUpdate() {
	if (hasUpdates() == false || typeof nativeApp().installUpdate != "function") return;
	try {
		await nativeApp().installUpdate();
	} catch (error) {
		toast(error?.message ?? "Singularity could not restart to update. Try again.", { tone: "seal" });
	}
}

// What the last install did, said once, when it was not what was meant: the
// system's permission refused. A failure is said by the installer's notice,
// with the way to install it by hand.
let isLastInstallSaid = false;
export function tellLastInstall(status) {
	if (isLastInstallSaid || status?.lastInstall?.state != "refused" || !status.lastInstall.detail) return false;
	isLastInstallSaid = true;
	toast(status.lastInstall.detail, { duration: 12000 });
	return true;
}

function safeStorage() {
	try {
		return window.localStorage;
	} catch {
		return null;
	}
}

// The first start of a newer version says so once, from the window's top, with
// the way to what is new in it (the owner, 2026-11-05: "after an update, inside
// Singularity itself, at the top, a notice: the app has been updated to version
// 0.1.1, see the release notes, with a link to the site"). This computer keeps
// the version it ran last; onboarding keeps the version it set the computer up
// with, so a fresh install is never told it was updated, and a computer that ran
// a version from before this notice, which kept nothing, is told once.
const SEEN_KEY = "singularity.versionSeen";
const RELEASE_NOTES = "https://legalmachines.org/singularity/releases/";

export function releaseNotesAddress(version) {
	return RELEASE_NOTES + encodeURIComponent(version);
}

function versionNumbers(version) {
	const parts = /^(\d+)\.(\d+)\.(\d+)$/.exec(String(version ?? ""));
	return parts ? parts.slice(1).map(Number) : null;
}

// Whether `version` is later than `than`, number by number: 0.1.10 after 0.1.9.
export function isLaterVersion(version, than) {
	const [a, b] = [versionNumbers(version), versionNumbers(than)];
	if (a == null || b == null) return false;
	for (let at = 0; at < 3; at += 1) if (a[at] != b[at]) return a[at] > b[at];
	return false;
}

async function versionHere() {
	try {
		return (await nativeApp().getRuntimeInfo())?.appVersion ?? "";
	} catch {
		return "";
	}
}

// Onboarding's end: the version this computer was set up with, said to no one.
export async function recordVersionSeen({ storage = safeStorage(), version } = {}) {
	const here = version ?? (hasNative() ? await versionHere() : "");
	if (storage == null || versionNumbers(here) == null) return;
	try {
		storage.setItem(SEEN_KEY, here);
	} catch {
		// Told once more at the next start; nothing else depends on it.
	}
}

// Whether the version running was just updated to, said once if so. Where the
// window cannot keep what it saw, it says nothing rather than say it each time.
export async function tellUpdated({ storage = safeStorage(), version, open = openReleaseNotes } = {}) {
	const here = version ?? (hasNative() ? await versionHere() : "");
	if (storage == null || versionNumbers(here) == null) return false;
	let seen;
	try {
		seen = storage.getItem(SEEN_KEY);
		storage.setItem(SEEN_KEY, here);
	} catch {
		return false;
	}
	if (seen != null && isLaterVersion(here, seen) == false) return false;
	toast(`Singularity has been updated to ${here}.`, {
		duration: 15000,
		action: { label: "What's new", run: () => open(releaseNotesAddress(here)) },
	});
	return true;
}

// What's new opens the notes in the member's browser; where it cannot, it says
// where they are rather than do nothing.
function openReleaseNotes(address) {
	const failed = () => toast("The release notes could not be opened. They are at legalmachines.org/singularity/releases.", { tone: "seal" });
	if (hasNative() == false || typeof nativeApp().openExternal != "function") {
		failed();
		return;
	}
	Promise.resolve(nativeApp().openExternal(address)).catch(failed);
}

// Downloads the version found. Asked for - a build that does not install
// itself - it is a notice that fills as it comes (D-207's notice); of its own
// (`isQuiet`) it says nothing while it comes, nor if it does not finish:
// Settings shows its progress and the way to try again. Either way it ends
// with what comes next: Restart now once it is readied, or why it is
// installed by hand, with the way to the installer.
export async function downloadUpdate({ isQuiet = false } = {}) {
	const release = current.release;
	if (isDownloading || release == null || hasUpdates() == false) return;
	isDownloading = true;
	const key = "update";
	const say = received => {
		if (isQuiet) return;
		notice(key, {
			text: `Downloading Singularity ${release.version}: ${sizeWords(received)} of ${sizeWords(release.bytes)}`,
			progress: release.bytes > 0 ? Math.min(1, received / release.bytes) : null,
			tone: "working",
		});
	};
	const end = options => (isQuiet ? toast(options.text, options) : endNotice(key, options));
	say(0);
	publish({ state: "downloading", received: 0 });
	const timer = setInterval(() => {
		nativeApp().getUpdateStatus().then(status => {
			if (status.state != "downloading") return;
			publish(status);
			say(status.received);
		}).catch(() => {});
	}, PROGRESS_MILLISECONDS);
	try {
		const status = await nativeApp().downloadUpdate();
		publish(status);
		if (status.state == "ready") {
			if (isQuiet) tellReady(release, status.detail);
			else end({ text: `Singularity ${release.version} is ready. ${status.detail}`.trim(), duration: 15000, action: { label: "Restart now", run: () => restartToUpdate() } });
		} else if (status.state == "downloaded") {
			end({
				text: status.detail || `Singularity ${release.version} is downloaded. Quit Singularity, then run the installer.`,
				duration: 15000,
				action: { label: "Show in folder", run: () => showUpdate() },
			});
		} else if (isQuiet == false) {
			end({ text: status.detail || "The download stopped. Try again.", tone: "problem", duration: 8000 });
		}
	} catch (error) {
		if (isQuiet == false) end({ text: error?.message ?? "The download stopped. Try again.", tone: "problem", duration: 8000 });
		nativeApp().getUpdateStatus().then(status => publish(status)).catch(() => {});
	} finally {
		clearInterval(timer);
		isDownloading = false;
	}
}

export async function showUpdate() {
	try {
		await nativeApp().showUpdate();
	} catch (error) {
		toast(error?.message ?? "The installer's folder could not be shown.", { tone: "seal" });
	}
}

// What Settings' Singularity row says, in words, from the state: the
// version here, and what is known of a later one.
export function updateLine(status, fallbackVersion = "") {
	const here = status.currentVersion != "" ? `Version ${status.currentVersion}` : fallbackVersion;
	const release = status.release;
	switch (status.state) {
		case "checking":
			return `${here}. Checking for updates`;
		case "available":
			return release != null ? `${here}. Version ${release.version} is available` : here;
		case "downloading":
			return release != null ? `Downloading version ${release.version}: ${sizeWords(status.received)} of ${sizeWords(release.bytes)}` : here;
		case "ready":
			return release != null ? `Version ${release.version} is ready. ${String(status.detail ?? "").replace(/\.$/, "")}`.trim() : here;
		case "downloaded":
			return release != null ? `Version ${release.version} is downloaded. Quit Singularity, then run the installer` : here;
		case "current":
			return status.isAsked ? `${here}, the latest` : here;
		case "failed":
			return `${here}. ${status.detail || "The download stopped. Try again."}`;
		case "offline":
		case "refused":
			return status.isAsked ? `${here}. ${COULD_NOT_CHECK}` : here;
		default:
			return here;
	}
}
