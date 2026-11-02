// Onboarding's account with Legal Machines (D-133), through the bridge: the
// organisation's email first; a known organisation signs in with its password
// and a second step - the code its authenticator app shows, or for an older
// one the code mailed to that email. A new one is created from its details
// and agreements alone, confirms its email, then sets its administrator's
// password and two-factor authentication and is given its recovery codes
// (D-146). The password goes to the native side with each call and is kept
// nowhere here; what the email's confirmation gives to finish the setup stays
// native. Signed in, the native side records the account and downloads
// Papinian's protection model.

// Short words for each refusal (UI-28). Where the words must say what to
// correct (a registration's details), the bridge's own are used.
const REFUSALS = {
	"account.wrong_credentials": "Email or password is wrong.",
	"account.wrong_code": "That code doesn’t match. Try again.",
	"account.too_many_attempts": "Too many tries. Wait a few minutes.",
	"account.offline": "Can’t reach Legal Machines. Check the internet.",
	"account.service_unavailable": "Legal Machines isn’t answering. Try again soon.",
	"account.refused": "Legal Machines declined. Try again in a few minutes.",
	"account.already_registered": "This organisation is registered already.",
	"account.setup_expired": "This setup has expired. Sign in to go on.",
};

export function refusalOf(error) {
	return REFUSALS[error?.code] ?? error?.userMessage ?? error?.message ?? "Something went wrong. Try again.";
}

// The account in the app. In a browser preview (`?onboarding`, no bridge) a
// stand-in answers instead: it signs nothing in and sends nothing, an email at
// example.com is a known organisation that signs in with an authenticator app
// (one that begins "mail" with a mailed code), and every code it asks for is
// PREVIEW_CODE. The page never shows it (the owner, 2026-10-29).
//
// A test of the whole product in the app may stand in for Legal Machines the
// same way (the owner, 2026-10-31: "I can't give you the code from the mail;
// at the onboarding stage emulate it, like 2FA"). Only a Debug build started
// for such a test says so (accountEmulation "register" for a new firm, "join"
// for a colleague signing in to a seat the firm made, with the firm's name); a
// Release build
// never does, and Legal Machines' own checks are never weakened. The test's
// firm then waits a few seconds for its approval, and its name is the one
// this computer keeps for an installation nobody signed in to.
export const PREVIEW_CODE = "246810";
export const PREVIEW_SECRET = "JBSWY3DPEHPK3PXPJBSWY3DPEHPK3PXP";
const PREVIEW_RECOVERY_CODES = [
	"7K4Q-M2XF", "R9DW-3HZP", "C6TN-8YVB", "Q2JM-5KXR", "W8FH-4NCT",
	"H3VZ-9QDL", "M5RB-2WJY", "X7PC-6GTK", "D4LN-8FHQ", "B9YT-3MVW",
];

export function accountOf(nativeApp) {
	const emulation = emulationOf(nativeApp);
	if (typeof nativeApp?.findLegalMachinesOrganisation == "function" && emulation == null) {
		return {
			preview: false,
			state: () => nativeApp.getAccountState(),
			find: (email) => nativeApp.findLegalMachinesOrganisation(email),
			// The firm's email, a seat's id and its password (D-204): the
			// administrator's seat goes on to its code, a colleague's is signed in.
			signIn: (email, seat, password) => nativeApp.signInToLegalMachinesWithEmail(email, seat, password),
			verify: (challengeId, code, organisationId, seatId, password, email) =>
				nativeApp.verifyLegalMachinesCode(challengeId, code, organisationId, seatId, password, email),
			// A new organisation from its details and agreements, no password yet; a
			// code goes to its email.
			create: (registration) => nativeApp.createLegalMachinesOrganisation(registration),
			// {state, setup}: setup names what the new organisation sets next,
			// "password" or "two_factor"; none for one already set up.
			confirm: (organisationId, code) => nativeApp.confirmLegalMachinesEmail(organisationId, code),
			resend: (organisationId) => nativeApp.resendLegalMachinesEmailCode(organisationId),
			setPassword: (organisationId, password) => nativeApp.setLegalMachinesPassword(organisationId, password),
			// {secret, uri}: the key the authenticator app keeps, and its setup link.
			startTwoFactor: (organisationId) => nativeApp.startLegalMachinesTwoFactor(organisationId),
			// {state, recoveryCodes, signedIn?}: two-factor is on, the codes that
			// stand in for the app once each, and, for an organisation active at
			// the end of its setup, its sign-in here, with no code again (D-152).
			confirmTwoFactor: (organisationId, code, seatId, password, email) =>
				nativeApp.confirmLegalMachinesTwoFactor(organisationId, code, seatId, password, email),
			// {saved}: the recovery codes written to a file the person chooses.
			saveRecoveryCodes: (organisationId) => nativeApp.saveLegalMachinesRecoveryCodes(organisationId),
			// {printed}: the recovery codes as a page to print, opened with its
			// print dialog in the system's browser.
			printRecoveryCodes: (organisationId) => nativeApp.printLegalMachinesRecoveryCodes(organisationId),
			// Whether Legal Machines approved the organisation the page waits for (D-030).
			approved: (organisationId) => typeof nativeApp.getLegalMachinesApprovalState == "function"
				? nativeApp.getLegalMachinesApprovalState(organisationId)
				: Promise.resolve({ approved: false }),
			// A colleague's seat sets up its own two-step sign-in, with its
			// session (D-215): {secret, uri}, then {recoveryCodes}.
			startOwnTwoFactor: () => nativeApp.startOwnTwoFactor(),
			confirmOwnTwoFactor: (code) => nativeApp.confirmOwnTwoFactor(code),
			// The administrator's forgotten password, by the firm's email (D-215).
			requestRecovery: (email) => nativeApp.requestAccountRecovery(email),
			resetPassword: (email, code, twoStepCode, password) => nativeApp.resetAccountPassword(email, code, twoStepCode, password),
		};
	}
	return standIn(emulation);
}

// What a Debug build started for a whole-product test says it stands in for:
// a new firm ("register") or a colleague signing in to their seat ("join").
export function emulationOf(nativeApp) {
	const mode = nativeApp?.accountEmulation;
	if (mode !== "register" && mode !== "join") return null;
	return { mode, firm: typeof nativeApp.emulatedFirm == "string" ? nativeApp.emulatedFirm.trim() : "" };
}

// The firm the stand-in signed in to in a whole-product test, for the name
// onboarding keeps on this computer with the member's own.
let emulatedFirmName = null;

export function emulatedFirm() {
	return emulatedFirmName;
}

// How long a test's new firm waits for Legal Machines' approval.
export const EMULATED_APPROVAL_MS = 6000;

function standIn(emulation) {
	const pause = () => new Promise((resolve) => setTimeout(resolve, 650));
	const wrongCode = () => Object.assign(new Error("wrong code"), { code: "account.wrong_code" });
	const seat = { organisationId: "preview-firm", seatId: "preview-seat" };
	let firm = emulation?.firm || "Preview Legal LLC";
	let awaitingSince = null;
	const signedIn = (role = "administrator") => {
		if (emulation != null) emulatedFirmName = firm;
		return { status: "signed_in", account: { organisationName: firm, role } };
	};
	return {
		preview: true,
		emulation: emulation?.mode ?? null,
		state: async () => null,
		find: async (email) => {
			await pause();
			// A test's new firm is new; its colleague's firm is known.
			if (emulation?.mode === "register") return { exists: false };
			if (emulation?.mode === "join") return { exists: true };
			return { exists: email.trim().toLowerCase().endsWith("@example.com") };
		},
		// The administrator's seat is "preview-seat" and goes on to its code; any
		// other seat is a colleague's, signed in at once (D-204), as is a test's.
		signIn: async (email, seatId) => {
			await pause();
			if (emulation?.mode === "join" || (seatId ?? "").trim().toLowerCase() !== seat.seatId) {
				return { ...signedIn("member"), organisationId: seat.organisationId, seatId: (seatId ?? "").trim().toLowerCase() };
			}
			const method = email.trim().toLowerCase().startsWith("mail") ? "email" : "authenticator";
			return { status: "two_factor_required", method, challengeId: "preview", maskedEmail: null, ...seat };
		},
		verify: async (challengeId, code) => {
			await pause();
			if (code !== PREVIEW_CODE && !PREVIEW_RECOVERY_CODES.includes(code)) throw wrongCode();
			return signedIn();
		},
		create: async (registration) => {
			await pause();
			if (registration?.legalEntityName) firm = registration.legalEntityName.trim();
			return { ...seat, maskedEmail: null };
		},
		confirm: async (organisationId, code) => {
			await pause();
			if (code !== PREVIEW_CODE) throw wrongCode();
			return { state: "awaiting_approval", setup: "password" };
		},
		setPassword: async () => {
			await pause();
			return { setup: "two_factor" };
		},
		startTwoFactor: async () => {
			await pause();
			return {
				secret: PREVIEW_SECRET,
				uri: `otpauth://totp/Legal%20Machines:preview%40example.com?secret=${PREVIEW_SECRET}&issuer=Legal%20Machines&algorithm=SHA1&digits=6&period=30`,
			};
		},
		confirmTwoFactor: async (organisationId, code) => {
			await pause();
			if (code !== PREVIEW_CODE) throw wrongCode();
			// A test's new firm waits for its approval as a real one does.
			if (emulation != null) {
				awaitingSince = Date.now();
				return { state: "awaiting_approval", recoveryCodes: PREVIEW_RECOVERY_CODES };
			}
			return { state: "active", recoveryCodes: PREVIEW_RECOVERY_CODES, signedIn: signedIn() };
		},
		startOwnTwoFactor: async () => {
			await pause();
			return {
				secret: PREVIEW_SECRET,
				uri: `otpauth://totp/Legal%20Machines:preview-seat?secret=${PREVIEW_SECRET}&issuer=Legal%20Machines&algorithm=SHA1&digits=6&period=30`,
			};
		},
		confirmOwnTwoFactor: async (code) => {
			await pause();
			if (code !== PREVIEW_CODE) throw wrongCode();
			return { recoveryCodes: PREVIEW_RECOVERY_CODES };
		},
		requestRecovery: async () => {
			await pause();
			return { sent: true };
		},
		resetPassword: async (email, code, twoStepCode) => {
			await pause();
			if (code !== PREVIEW_CODE || (twoStepCode !== PREVIEW_CODE && !PREVIEW_RECOVERY_CODES.includes(twoStepCode))) throw wrongCode();
			return { changed: true };
		},
		// A browser prints the codes from a page of their own.
		printRecoveryCodes: async () => {
			const page = window.open("", "_blank");
			if (!page) return { printed: false };
			page.document.write(`<title>Legal Machines recovery codes</title><pre style="font:16px/1.8 ui-monospace,monospace">${PREVIEW_RECOVERY_CODES.join("\n")}</pre>`);
			page.document.close();
			page.print();
			return { printed: true };
		},
		// A browser keeps the codes as a downloaded file. A test in the app keeps
		// nothing: its codes are the stand-in's, known to the test, and nothing
		// is written to the computer's own folders.
		saveRecoveryCodes: async () => {
			if (emulation != null) return { saved: true };
			const text = `Legal Machines recovery codes\n\n${PREVIEW_RECOVERY_CODES.join("\n")}\n`;
			const link = Object.assign(document.createElement("a"), {
				href: URL.createObjectURL(new Blob([text], { type: "text/plain" })),
				download: "Legal Machines recovery codes.txt",
			});
			link.click();
			URL.revokeObjectURL(link.href);
			return { saved: true };
		},
		resend: async () => {
			await pause();
			return { sent: true };
		},
		approved: async () => ({ approved: awaitingSince != null && Date.now() - awaitingSince >= EMULATED_APPROVAL_MS }),
	};
}
