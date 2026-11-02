import { useSyncExternalStore } from "react";

import { dropNotice, notice } from "../components/Toast/toast.js";
import { hasNative, nativeApp } from "./native.js";

// A sign-in by a recovery code (D-218), as every service treats one: the
// member signed in without their authenticator app, so once Singularity opens
// it asks, once and calmly, for a new app, and says how many codes are left.
// Only the count is kept, for this window, never a code.
const KEY = "singularity.recoverySignIn";
let recovered = null;
// Settings opens on changing the app when the member asked from that notice.
let isChangeAsked = false;

export function rememberRecoverySignIn(left) {
	recovered = { left: Number.isInteger(left) && left >= 0 ? left : null };
	try {
		window.sessionStorage.setItem(KEY, JSON.stringify(recovered));
	} catch {
		// Kept in this page alone.
	}
}

// The sign-in by a recovery code not yet told, or null; told once.
export function takeRecoverySignIn() {
	let found = recovered;
	try {
		const kept = window.sessionStorage.getItem(KEY);
		if (found == null && kept != null) found = JSON.parse(kept);
		window.sessionStorage.removeItem(KEY);
	} catch {
		// The page's own memory is enough.
	}
	recovered = null;
	if (found == null || typeof found != "object") return null;
	return { left: Number.isInteger(found.left) && found.left >= 0 ? found.left : null };
}

// What the notice says: that the member signed in by a recovery code, how
// many are left and what to do next.
export function recoverySignInLine({ left }) {
	const count = left == null ? "" : left == 0 ? " None are left." : left == 1 ? " 1 is left." : ` ${left} are left.`;
	return `You signed in with a recovery code.${count} Set up your authenticator app again.`;
}

export function askToChangeApp() {
	isChangeAsked = true;
}

export function takeChangeAsked() {
	const was = isChangeAsked;
	isChangeAsked = false;
	return was;
}

// The Recovery codes row's line in Settings: how many are left, where Legal
// Machines says it, and what to do when few are.
export function recoveryCodesLine(left) {
	if (left == null) return "Each signs you in once if you lose your phone";
	if (left == 0) return "None left. Make new ones so that a lost phone can’t lock you out";
	if (left <= 3) return `${left} left. Make new ones before they run out`;
	return `${left} left. Each signs you in once if you lose your phone`;
}

// Two-step sign-in to set up before Legal Machines opens anything else (D-215):
// a seat with no authenticator app may only set one up, and every other call
// of its session - the firm's seats and computers, the models - is refused
// with "account.two_step_setup_required" and told as the event
// account.twoStepSetupRequired. The window then leads the member to the
// stepped setup once; put off, it waits as a notice with Set up. Once it is
// done, the same session opens everything: Singularity's own rounds go on by
// themselves, and what the window read is read again (`setUps`).
const SETUP_NOTICE = "two-step-setup";
let setup = { isOpen: false, isPutOff: false, setUps: 0 };
const setupListeners = new Set();

function changeSetup(next) {
	setup = { ...setup, ...next };
	for (const listener of setupListeners) listener();
}

// { isOpen, isPutOff, setUps }: whether the setup is open now, whether the
// member put it off, and how many times it was done in this window.
export function readTwoStepSetup() {
	return setup;
}

export function useTwoStepSetup() {
	return useSyncExternalStore(
		listener => {
			setupListeners.add(listener);
			return () => setupListeners.delete(listener);
		},
		readTwoStepSetup,
	);
}

function tellSetupWaits() {
	notice(SETUP_NOTICE, {
		text: "Set up two-step sign-in to work with your firm.",
		tone: "waiting",
		action: { label: "Set up", run: () => leadToTwoStepSetup({ isAsked: true }) },
	});
}

// `isAsked`: the member asked for it, so the setup opens even when it was put
// off before.
export function leadToTwoStepSetup({ isAsked = false } = {}) {
	if (setup.isOpen) return;
	if (setup.isPutOff && isAsked == false) {
		tellSetupWaits();
		return;
	}
	dropNotice(SETUP_NOTICE);
	changeSetup({ isOpen: true });
}

export function twoStepSetUp() {
	dropNotice(SETUP_NOTICE);
	changeSetup({ isOpen: false, isPutOff: false, setUps: setup.setUps + 1 });
}

export function twoStepSetupPutOff() {
	changeSetup({ isOpen: false, isPutOff: true });
	tellSetupWaits();
}

// What Singularity met in the background - the firm's computers asked every
// few seconds, the protection model's download - leads the member there too.
export function watchTwoStepSetup() {
	if (hasNative() == false || typeof nativeApp().on != "function") return () => {};
	return nativeApp().on("account.twoStepSetupRequired", () => leadToTwoStepSetup()) ?? (() => {});
}
