import { ensureDefaultModels } from "../../data/models.js";
import { emulatedFirm } from "../entry/account.js";

// What onboarding's pages set up in the app (D-134), through the bridge: the
// member's name and own office, the model accounts the AI colleagues think
// with, and the protection model's state. In a browser preview a stand-in
// answers, and nothing is kept.
export function setupOf(nativeApp) {
	if (typeof nativeApp?.setOwnOffice == "function") {
		return {
			preview: false,
			viewer: async () => (await nativeApp.getOrganisation())?.viewer ?? null,
			// The name first: the office is written into the member's record of
			// every Matter here, and the record carries the name the member gives.
			// A photo they chose or took away goes the same way (D-163); one they
			// left as it was is not written again.
			// The firm's name is kept beside the member's: the one this computer
			// keeps for an installation nobody signed in to stays as it was (a
			// whole-product test's firm among them, account.js).
			saveRole: async (name, position, practice, photo) => {
				const known = typeof nativeApp.getOrganisation == "function" ? await nativeApp.getOrganisation().catch(() => null) : null;
				const firm = known?.viewer?.organisationName || emulatedFirm();
				if (firm) await nativeApp.savePerson(name, firm);
				else await nativeApp.savePerson(name);
				const viewer = await nativeApp.setOwnOffice({ position, practice });
				if (photo === undefined || typeof nativeApp.setOwnPhoto != "function") return viewer;
				return nativeApp.setOwnPhoto(photo);
			},
			// The picture the member chooses on this computer, as it is: the window
			// cuts their photo from it (data/photos.js). Null when they cancel.
			choosePhoto: async () => (typeof nativeApp.chooseOwnPhoto == "function" ? (await nativeApp.chooseOwnPhoto())?.picture ?? null : null),
			// The model accounts the backend keeps (providers.list): the AI setup's
			// capabilities name roles, not accounts (2026-10-29: a connected
			// account read as not connected after a reload).
			providers: async () => {
				const listed = await nativeApp.listProviders();
				return Array.isArray(listed) ? listed : listed?.providers ?? [];
			},
			// A connected account gives the firm's colleagues their default models
			// for the roles not set yet (data/models.js, D-138).
			connect: async (provider, key) => {
				const connected = await nativeApp.configureProvider(provider.id, key, provider.baseUrl, provider.displayName);
				await ensureDefaultModels(nativeApp).catch(() => []);
				return connected;
			},
			protection: () => nativeApp.getPapinianModelStatus(),
			// The protection model's download, started only by the member: its
			// button in onboarding, or Download and Try again where its state
			// shows (D-198). It answers once the model runs, or fails.
			downloadProtection: () => nativeApp.syncLocalModels("papinian-ner"),
			// Whether this computer masks what leaves it (protectedApi.getPolicy):
			// "off" sends requests as written; "standard" and "custom" mask.
			protectionOn: async () => ((await nativeApp.getProtectedApiPolicy?.())?.mode ?? "standard") != "off",
			// On keeps the categories this computer chose; Off turns masking off
			// on this computer only (protectedApi.savePolicy).
			setProtectionOn: async on => {
				const policy = (await nativeApp.getProtectedApiPolicy?.()) ?? {};
				const mode = on ? (policy.mode == "custom" ? "custom" : "standard") : "off";
				return nativeApp.saveProtectedApiPolicy({
					mode,
					enabledCategories: (policy.categories ?? []).filter(category => category.enabled).map(category => category.id),
					customTerms: policy.customTerms ?? [],
				});
			},
			// The folder the firm's computers share, where their Matters travel
			// sealed between them (D-123): the one set, and choosing one.
			firmFolder: async () => (await nativeApp.getCoordination?.())?.firmFolder ?? null,
			chooseFirmFolder: async () => {
				const folder = await nativeApp.chooseFolder();
				if (folder == null || folder == "") return null;
				await nativeApp.useFirmFolder(folder);
				return folder;
			},
			// Word, Excel, PowerPoint and Outlook as Office itself reports them
			// (office.getStatus, office.listClients, D-105): this computer's add-in,
			// the panes allowed, and the requests waiting with their codes.
			officeStatus: () => nativeApp.getOfficeStatus(),
			officeClients: () => nativeApp.listOfficeClients(),
			connectOffice: () => nativeApp.connectOffice(),
			// The application being connected, opened by Singularity itself: Word,
			// Excel and PowerPoint with a file of its own whose pane opens and asks
			// to connect by itself (office.openApp, D-191). A bridge without it
			// opens nothing, and says no problem.
			openOfficeApp: app => (typeof nativeApp.openOfficeApp == "function"
				? nativeApp.openOfficeApp(app)
				: Promise.resolve({ app, opened: false, problem: "" })),
			allowPane: pairingId => nativeApp.decideOfficePairing(pairingId, true),
			onPairing: listener => (typeof nativeApp.on == "function" ? nativeApp.on("office.pairingRequested", listener) : null) ?? (() => {}),
			// The first AI colleague, the whole firm's, never one Matter's
			// (agent.hire, D-157): its face, name, role, position and field.
			hireColleague: ({ name, species, role, position, area }) => nativeApp.hireColleague({
				name: name.trim(),
				character: species,
				title: role.trim(),
				ranksWith: position,
				area: area.trim(),
			}),
		};
	}
	const connected = [];
	const pause = () => new Promise((resolve) => setTimeout(resolve, 650));
	// The protection model as a preview pretends it: not here until asked for,
	// then coming over a few seconds, then running.
	let protection = { status: "available", recovery: "retry_download" };
	return {
		preview: true,
		viewer: async () => null,
		saveRole: async () => {
			await pause();
			return {};
		},
		// A browser has its own file chooser.
		choosePhoto: () => new Promise((resolve) => {
			const input = document.createElement("input");
			input.type = "file";
			input.accept = "image/jpeg,image/png,image/gif,image/webp";
			input.onchange = () => {
				const file = input.files?.[0];
				if (file == null) return resolve(null);
				const reader = new FileReader();
				reader.onload = () => resolve(String(reader.result));
				reader.onerror = () => resolve(null);
				reader.readAsDataURL(file);
			};
			input.click();
		}),
		providers: async () => [...connected],
		connect: async (provider, key) => {
			await pause();
			if (key.trim().length < 12) throw Object.assign(new Error("refused"), { detail: "Could not verify this API key." });
			connected.push({ id: provider.id, baseUrl: provider.baseUrl ?? null });
			return [...connected];
		},
		protection: async () => ({ ...protection }),
		downloadProtection: async () => {
			for (let percent = 0; percent < 100; percent += 3) {
				protection = { status: "downloading", progressPercent: percent };
				await new Promise((resolve) => setTimeout(resolve, 120));
			}
			protection = { status: "verifying" };
			await pause();
			protection = { status: "ready" };
			return { ...protection };
		},
		protectionOn: async () => true,
		setProtectionOn: async () => ({}),
		firmFolder: async () => null,
		chooseFirmFolder: async () => {
			await pause();
			return "/Users/you/Dropbox/Firm";
		},
		hireColleague: async ({ name }) => {
			await pause();
			return { id: "agent_preview", name: name.trim() };
		},
		// A browser preview has no Office: a specimen code stands in, and nothing
		// connects.
		officeStatus: async () => ({ supported: true, connected: true, preview: true }),
		officeClients: async () => ({ clients: [], pending: [] }),
		connectOffice: async () => ({}),
		openOfficeApp: async app => ({ app, opened: true, problem: "" }),
		allowPane: async () => ({}),
		onPairing: () => () => {},
	};
}

// The protection model's state in a few words (UI-28), from the backend's own
// status (papinian.getModelStatus), with the thing it speaks of as `subject`.
export function protectionWords(status, subject = "Client data protection") {
	switch (status?.status) {
		case "ready":
		case "loading":
		case "unloaded":
			return `${subject} is ready.`;
		case "downloading":
			return status.progressPercent != null
				? `${subject} is downloading, ${Math.floor(status.progressPercent)}%.`
				: `${subject} is downloading.`;
		case "verifying":
		case "installing":
			return `${subject} is installing.`;
		case "checking_server":
			return `${subject} is on its way.`;
		// Nothing downloads until the member asks (D-198).
		case "available":
			return `${subject} is not on this computer yet.`;
		case "authorization_required":
			return `${subject} waits for sign-in.`;
		case "error":
			// A model on this computer that would not start is not one that did
			// not arrive (2026-10-29: a Release build's failed start read "did not
			// download" over a model fully installed).
			return status.recovery == "retry_load"
				? `${subject} did not start on this computer.`
				: `${subject} did not download.`;
		default:
			return "";
	}
}

// What went wrong, in the backend's own words, when protection failed: shown
// under its state, so a failure says what happened.
export function protectionDetail(status) {
	if (status?.status != "error") return "";
	return (status.message ?? "").trim();
}

// The protection model is on this computer and runs, or is starting to.
export function protectionRuns(status) {
	return status?.status == "ready" || status?.status == "loading";
}

// The protection model is on its way: asked for, coming or being checked.
export function protectionMoving(status) {
	return ["checking_server", "downloading", "verifying", "installing"].includes(status?.status);
}

// How much of its button the protection model's download fills, in percent
// (D-198): the bytes while they come, all of it once they are being checked,
// installed and started.
export function protectionFill(status) {
	switch (status?.status) {
		case "downloading":
			return Math.max(0, Math.min(100, status.progressPercent ?? 0));
		case "verifying":
		case "installing":
		case "loading":
		case "ready":
		case "unloaded":
			return 100;
		default:
			return 0;
	}
}

// The button's words while the protection model comes: one line (UI-28).
export function downloadWords(status) {
	switch (status?.status) {
		case "downloading":
			return status.progressPercent != null ? `Downloading ${Math.floor(status.progressPercent)}%` : "Downloading…";
		case "verifying":
			return "Checking the download…";
		case "installing":
			return "Installing…";
		// Starting, it says how far by the start's own expected time.
		case "loading":
			return status.elapsedMs != null && status.expectedMs > 0
				? `Starting ${Math.min(99, Math.floor((status.elapsedMs * 100) / status.expectedMs))}%`
				: "Starting…";
		case "ready":
		case "unloaded":
			return "Starting…";
		default:
			return "Connecting to Legal Machines…";
	}
}
