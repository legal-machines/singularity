// Onboarding after sign-in (D-134): one key thing a slide, as Apple's
// onboarding explains a product (the owner's reference, 2026-10-28), and with
// it the one thing that needs setting up for it here - told about AI
// colleagues, the member connects the models they think with. The app shows
// the thing on the right, itself at work on the deal of the first page
// (emulation/scenes.js), one action a slide.
//
import { PROTECTION_TEST } from "../legal.js";

// A slide is data: its scene, its words and its ask (tour/Tour.jsx). Adding a
// slide is adding an entry here and, where it needs one, a scene and an ask.
// A slide without a scene shows its `preview` (tour/SetupPreview.jsx).
//
// The order tells the firm first (AG-18): who works in it, how work moves and
// the member's own AI colleague; then what happens to its files and client
// data on this computer (D-195); then where work lives, the apps, and what the
// computer itself will ask.
export const PAGES = [
	{
		id: "team", scene: "team", ask: "role",
		title: "One firm. One team.",
		line: "People and AI colleagues work side by side, each with a role and responsibility.",
		action: "Introduce yourself",
		setupTitle: "Your place in the firm.",
		setupLine: "Give your colleagues a name and a role to know you by.",
	},
	{
		id: "colleagues", scene: "delegate", ask: "models",
		title: "Give a goal. Work moves.",
		line: "Colleagues plan the work, give people their part and bring back a result.",
		action: "Connect a model", skip: "Set up later",
		setupTitle: "Connect your model account.",
		setupLine: "Choose a service your colleagues can work with. It bills your account for what they use.",
		// Said at the foot of the slide, with the Terms of Use it points to
		// (the owner, 2026-11-01, D-189).
		disclaimer: "Illustrative example. Generative AI results can differ each time and depend in part on the AI model used.",
	},
	{
		id: "first-colleague", ask: "colleague",
		title: "Meet your first AI colleague.",
		line: "A member of your firm, with an area to lead and a name to know them by.",
		action: "Hire an AI colleague", skip: "Set up later",
		setupTitle: "Hire your first colleague.",
		setupLine: "Start with a name and a face. They join the whole firm, not one Matter.",
	},
	// What Singularity does to a firm's files around an AI colleague's work (the
	// owner, 2026-11-01, D-195), shown on the right with one clause of the deal:
	// each is what the code does today, never a plan (PROD-2).
	{
		id: "preparation", preview: "preparation", next: true,
		title: "Read as Word shows it.",
		line: "Singularity reads each document as Word does, and checks every change a colleague makes.",
		action: "Continue",
	},
	// Client data protection (D-195): what it does, that its model comes from
	// Legal Machines, how large it is and that it runs only on this computer.
	// The dated test's figures stand in the footnote, never as promises, with
	// links to its published results and to the Software License Agreement
	// (the owner, 2026-11-01, D-198). A size, a date, a range and Legal
	// Machines' name each stay on one line: non-breaking spaces.
	{
		id: "protection", preview: "protection", ask: "protection",
		title: "Masked here. Restored here.",
		line: "Client data protection masks the names and figures it finds before work leaves.",
		facts: [
			"Its model, about 630\u00a0MB, downloads from Legal\u00a0Machines and runs only on this computer.",
			"With protection on, nothing is sent if the model cannot run.",
		],
		action: "Set up protection",
		setupTitle: "How work leaves.",
		setupLine: "Choose whether names and figures are masked first.",
		disclaimer: "The example is illustrative. Masking may miss client details: in tests on 1\u00a0November\u00a02026 it masked 100% of them in English, and results in other languages may differ. Legal\u00a0Machines is not liable for what it misses.",
		disclaimerLinks: [{ text: "test results", href: PROTECTION_TEST }, "licenceModels"],
	},
	{
		id: "matters", scene: "matter", ask: "folder",
		title: "One Matter. The whole story.",
		line: "Conversations, documents and decisions stay together, where the team can find them.",
		action: "Choose how to share",
		setupTitle: "Keep your Matters together.",
		setupLine: "Choose how work travels between your firm’s computers.",
	},
	// TEMPORARY until Legal Machines' developer certificates exist (the owner,
	// 2026-11-01): remove this entry, and its words below, once macOS
	// notarises Singularity and Windows knows its publisher. Never on Linux.
	{
		id: "certificates", preview: "certificates", next: true, temporary: true,
		title: "Certificates on the way.",
		lines: {
			darwin: "Until Apple notarises Singularity, confirm each new version in Privacy & Security.",
			win32: "When Windows warns about a new version, choose More info, then Run anyway.",
		},
		note: "Download Singularity only from legalmachines.org. Singularity’s own certificate for Office is separate, and stays.",
		action: "Continue",
	},
	// What the computer itself must allow, asked here, before the work that
	// needs it, and checked (the owner, 2026-11-04: "a person could not connect
	// Office because the app forgot to ask in advance for the permissions it
	// needs; all of this must be asked by us in advance, and we must check that
	// it was given"). One press asks for each in turn, with the system's own
	// question; each says what came of it (Platform/SystemPermissions).
	{
		id: "permissions", preview: "permissions", ask: "permissions",
		title: "Your computer asks first.",
		line: "Singularity asks now for what it needs later, and checks that each was given.",
		permissions: {
			darwin: [
				{ key: "documents", name: "Documents folder", line: "Your Matters live in a Singularity folder in Documents, where you can always find their files." },
				{ key: "officeApps", name: "Word, Excel and PowerPoint", line: "Singularity’s add-in goes into each app’s own folder. macOS asks once for each app." },
				{ key: "localNetwork", name: "Local network", line: "Your firm’s computers on the office network reach each other directly, end‑to‑end encrypted." },
			],
			win32: [
				{ key: "controlledFolders", name: "Ransomware protection", line: "Your Matters live in a Singularity folder in Documents, which Windows’ ransomware protection guards. Allow Singularity there." },
				{ key: "officeCertificate", name: "Office certificate", line: "Word, Excel, PowerPoint and Outlook reach Singularity on this computer through a certificate of its own. Windows asks you to trust it." },
				{ key: "firewall", name: "Windows Firewall", line: "Your colleagues’ computers reach this one directly, end‑to‑end encrypted. Windows asks whether to allow it on the network you are on." },
			],
		},
		action: "Allow access", skip: "Set up later",
		setupTitle: "Allow access.",
		setupLine: "Your computer asks once for each. Choose Allow.",
	},
	// Office last, once the computer allows what it needs (the owner,
	// 2026-11-04: "swap Certificates on the way and connecting the add-in").
	{
		id: "apps", ask: "apps",
		title: "Your work. Connected.",
		line: "Your team and Matters, with you in Word, Excel, PowerPoint and Outlook.",
		action: "Connect your apps", skip: "Set up later",
		setupTitle: "Bring your apps together.",
		setupLine: "Choose an app to connect. You can finish the rest later.",
	},
];

// What the computer itself must allow Singularity here, by its keys
// (Platform/SystemPermissions).
export function permissionKeys(platform = platformOf()) {
	return (PAGES.find(page => page.ask === "permissions")?.permissions?.[platform] ?? []).map(item => item.key);
}

// The platform the app runs on, as the window reports it.
export function platformOf() {
	const declared = typeof document != "undefined" ? document.documentElement.dataset.nativePlatform : undefined;
	if (declared) return declared;
	const name = typeof navigator != "undefined" ? navigator.platform ?? "" : "";
	return name.startsWith("Mac") ? "darwin" : name.startsWith("Win") ? "win32" : "linux";
}

// The slides a member is shown. The firm's AI colleagues and the model
// accounts they think with are the firm's, set up by its administrative
// account (the owner, 2026-10-31: "a new employee logging in on a new device
// must not get the right to configure the provider or to create an AI
// colleague"; D-184). A colleague who joins is told so and goes on; the
// administrator is shown every slide, as before. A slide whose words belong to
// other systems (certificates, permissions) is left out where it has none, and
// Linux, where Word, Excel, PowerPoint and Outlook do not run, has no apps slide.
// `missingPermissions` are the keys of what this computer does not allow yet,
// as it said before the slides (null until it says): the slide that asks
// names only those, and is not shown at all when none is missing (the owner,
// 2026-11-05: "if some permission is already given, there is no need to ask
// about it at all", D-249).
export function pagesFor({ isMember = false, platform = platformOf(), missingPermissions = null } = {}) {
	const pages = PAGES
		.filter(page => !page.lines || page.lines[platform])
		.filter(page => !page.permissions || page.permissions[platform])
		.map(page => page.permissions && missingPermissions != null
			? { ...page, permissions: { ...page.permissions, [platform]: page.permissions[platform].filter(item => missingPermissions.includes(item.key)) } }
			: page)
		.filter(page => page.ask !== "permissions" || page.permissions[platform].length > 0)
		.filter(page => page.ask !== "apps" || platform !== "linux")
		.map(page => page.lines ? { ...page, line: page.lines[platform] } : page);
	if (!isMember) return pages;
	return pages
		.filter(page => page.ask !== "colleague")
		.map(page => {
			// Who a colleague is, the firm says (D-209): its administrator gave
			// their name, role and photo with their seat, and their computer takes
			// them from the firm's record.
			if (page.ask === "role") {
				return {
					...page,
					ask: null,
					next: true,
					action: "Continue",
					skip: undefined,
					note: "Your firm’s administrator fills in your profile. You can change it any time from your page in Team.",
				};
			}
			if (page.ask !== "models") return page;
			return {
				...page,
				ask: null,
				next: true,
				action: "Continue",
				skip: undefined,
				note: "Your firm’s administrator hires its AI colleagues and connects the models they think with.",
			};
		});
}

// The positions the firm ranks by (Organisation/Types/FirmOffice.hpp).
export const POSITIONS = ["Partner", "Senior Counsel", "Of Counsel", "Counsel", "Senior Associate", "Associate", "Paralegal"];
