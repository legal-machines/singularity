import { useSyncExternalStore } from "react";

import { endNotice, notice, toast } from "../components/Toast/toast.js";
import { hasNative, nativeApp } from "./native.js";
import { sizeWords } from "./transfers.js";

// Singularity's new versions come from Legal Machines' own site (the owner,
// 2026-11-02, going public: "do all of this"). The backend asks the site for
// the latest version for this computer's system and processor, keeps it only
// when Legal Machines' release key signed it, downloads the installer only
// at the member's asking, into Downloads, keeps it only when it is exactly the
// one signed, and shows it in its folder (Node/AppUpdate). The window asks a
// little after Singularity opens and every few hours; a new version is said
// once, calmly, with the way to download it, and Settings shows it at all
// times with the way to check now. A check that could not be made is never
// an alarm: on its own it says nothing, and asked for it says so in Settings.

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
	nativeApp().getUpdateStatus().then(status => publish(status)).catch(() => {});
	setTimeout(() => checkForUpdates(), FIRST_CHECK_MILLISECONDS);
	setInterval(() => checkForUpdates(), EVERY_MILLISECONDS);
}

// Asks the site now: `isAsked` when the member asked, in Settings.
export async function checkForUpdates({ isAsked = false } = {}) {
	if (hasUpdates() == false) return current;
	const isKept = current.state == "downloading" || current.state == "downloaded";
	publish({ state: isKept ? current.state : "checking", isAsked });
	try {
		const status = await nativeApp().checkForUpdate();
		publish({ ...status, isAsked });
		if (status.state == "available" && status.release != null && isAsked == false) tellAvailable(status.release);
	} catch {
		publish({ state: isKept ? current.state : "offline", detail: COULD_NOT_CHECK, isAsked });
	}
	return current;
}

// A new version, said calmly: a passing notice with the way to download it.
// Settings keeps saying it.
export function tellAvailable(release, { now = Date.now(), storage = safeStorage() } = {}) {
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
	toast(`Singularity ${release.version} is available.`, { duration: 12000, action: { label: "Download", run: () => downloadUpdate() } });
	return true;
}

function safeStorage() {
	try {
		return window.localStorage;
	} catch {
		return null;
	}
}

// Downloads the version found, as a notice that fills as it comes (D-207's
// notice), ending with the way to the installer.
export async function downloadUpdate() {
	const release = current.release;
	if (isDownloading || release == null || hasUpdates() == false) return;
	isDownloading = true;
	const key = "update";
	const say = received => notice(key, {
		text: `Downloading Singularity ${release.version}: ${sizeWords(received)} of ${sizeWords(release.bytes)}`,
		progress: release.bytes > 0 ? Math.min(1, received / release.bytes) : null,
		tone: "working",
	});
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
		if (status.state == "downloaded") {
			endNotice(key, {
				text: `Singularity ${release.version} is downloaded. Quit Singularity, then run the installer.`,
				duration: 15000,
				action: { label: "Show in folder", run: () => showUpdate() },
			});
		} else {
			endNotice(key, { text: status.detail || "The download stopped. Try again.", tone: "problem", duration: 8000 });
		}
	} catch (error) {
		endNotice(key, { text: error?.message ?? "The download stopped. Try again.", tone: "problem", duration: 8000 });
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
