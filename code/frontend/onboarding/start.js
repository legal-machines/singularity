// What the window shows first: onboarding until it is complete, then the app.
// In the app the bridge decides: onboarding.getStatus reads the same record
// the native side sized the window by. In a browser, with no bridge, the app
// shows; `?onboarding` in the address previews onboarding.
export function firstView(nativeApp, search = "") {
	if (typeof nativeApp?.getOnboardingStatus == "function") return "asking";
	return new URLSearchParams(search).has("onboarding") ? "onboarding" : "app";
}

// The view the bridge's answer gives. Without an answer, the app: a member is
// never kept out by a status that cannot be read.
export function viewOf(status) {
	return status?.completed === false ? "onboarding" : "app";
}

// Whether the window opens on the page where a colleague's new computer waits
// for the firm: a member signed in whose seat's profile has not come, with no
// computer of the firm checked and no Matter carried here yet.
export async function waitsForFirm(nativeApp) {
	try {
		return (await isWithFirm(nativeApp)) == false;
	} catch {
		return false;
	}
}

// Whether this computer is the firm's already: its seat's profile has come,
// or it has a computer of the firm checked, or it carries the firm's Matters.
// An administrator's, or a computer signed in to no seat, always is.
export async function isWithFirm(api) {
	if (typeof api?.getFirmComputers != "function") return true;
	const [account, profiles, listed, coordination] = await Promise.all([
		api.getAccountState?.().catch(() => null),
		api.listSeatProfiles?.().catch(() => null),
		api.getFirmComputers().catch(() => null),
		api.getCoordination?.().catch(() => null),
	]);
	if (account?.signedIn != true || account?.role != "member") return true;
	const seat = (account?.seatId ?? "").trim();
	if ((profiles?.profiles ?? []).some(profile => profile.seatId == seat && (profile.name ?? "").trim() != "")) return true;
	if ((listed?.computers ?? []).some(computer => computer.state == "checked")) return true;
	return (coordination?.matters ?? []).some(matter => matter.state == "carried");
}

// Onboarding is complete: the bridge records it and moves the window to its
// main size. A failed record still opens the app in its main geometry, and the
// next start asks again.
export async function completeOnboarding(nativeApp) {
	if (typeof nativeApp?.completeOnboarding != "function") return;
	try {
		await nativeApp.completeOnboarding();
	} catch {
		await nativeApp.prepareMainWindow?.().catch(() => {});
	}
}
