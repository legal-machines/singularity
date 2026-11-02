import { useEffect, useRef, useState } from "react";
import { Avatar } from "../../components/Avatar/Avatar.jsx";
import { characters } from "../../components/Avatar/characters.js";
import { FileKindIcon } from "../../components/FileKind/FileKind.jsx";
import { POSITIONS } from "./pages.js";
import { OutlookIcon } from "./OutlookIcon.jsx";
import entry from "../entry/EntryForm.module.css";
import tour from "./Tour.module.css";
import styles from "./SetupDesign.module.css";

// The first AI colleague is hired for the whole firm through the bridge
// (agent.hire, D-157): no Matter is asked for or bound. Until it is created
// its face, name and place are a draft of this onboarding session. The app
// connections below follow Office's verified status and pairing (D-156).
const FAMILY = Object.keys(characters);
const EMPTY_COLLEAGUE = { name: "", species: "kitsune", role: "", position: "", area: "", step: 0 };

export function ColleagueAsk({ active, draft, onChange, onDone, setup, busy = false, setBusy }) {
	const [value, setValue] = useState(() => ({ ...EMPTY_COLLEAGUE, ...draft }));
	const [error, setError] = useState("");
	const firstField = useRef(null);
	const creating = useRef(false);
	if (!active) return null;
	const change = patch => {
		const next = { ...value, ...patch };
		setValue(next); onChange(next);
		setError("");
	};
	const identity = value.step === 0;
	const valid = identity ? Boolean(value.name.trim()) : Boolean(value.role.trim() && value.position && value.area.trim());
	const submit = async event => {
		event.preventDefault();
		if (!valid || busy || creating.current) return;
		if (identity) { change({ name: value.name.trim(), step: 1 }); requestAnimationFrame(() => firstField.current?.focus()); return; }
		// Made once: a second press while it is being made makes no second one.
		if (value.createdId) return onDone();
		creating.current = true;
		setBusy?.(true);
		try {
			const made = await setup.hireColleague(value);
			change({ createdId: made?.id ?? "created" });
			onDone();
		} catch (failure) {
			setError(failure?.detail || failure?.message || `${value.name.trim()} couldn’t be hired. Try again.`);
		} finally {
			creating.current = false;
			setBusy?.(false);
		}
	};
	return <form className={tour.ask} onSubmit={submit} aria-label="Your first AI colleague">
		{identity ? <>
			<fieldset className={styles.family}>
				<legend className={styles.label}>Choose a face</legend>
				<div className={styles.faces}>{FAMILY.map(species => <label key={species} className={styles.face} data-selected={value.species === species || undefined}>
					<input type="radio" name="colleague-face" aria-label={species[0].toUpperCase() + species.slice(1)} checked={value.species === species} onChange={() => change({ species })} />
					<Avatar member={{ id: `onboarding-${species}`, kind: "ai_colleague", name: species, species }} size={40} />
				</label>)}</div>
			</fieldset>
			<div className={entry.field}><label htmlFor="colleague-name">Name</label><input id="colleague-name" autoComplete="off" maxLength={80} placeholder="For example, Kitsune" value={value.name} onChange={event => change({ name: event.target.value })} /></div>
			<p className={tour.help}>The name everyone in the firm will know.</p>
		</> : <>
			<div className={styles.formHeading}><span>Their place in the firm</span><button type="button" className={tour.inline} onClick={() => change({ step: 0 })}>Change name and face</button></div>
			<div className={entry.fields}>
				<div className={entry.field}><label htmlFor="colleague-role">Role</label><input ref={firstField} id="colleague-role" value={value.role} maxLength={80} placeholder="For example, Head of Commercial" onChange={event => change({ role: event.target.value })} /></div>
				<div className={entry.field}><label htmlFor="colleague-position">Ranks with</label><select id="colleague-position" className={tour.select} value={value.position} onChange={event => change({ position: event.target.value })}><option value="" disabled>Choose a rank</option>{POSITIONS.map(position => <option key={position}>{position}</option>)}</select></div>
				<div className={entry.field}><label htmlFor="colleague-area">Area</label><input id="colleague-area" value={value.area} maxLength={240} placeholder="For example, Commercial contracts" onChange={event => change({ area: event.target.value })} /></div>
			</div>
		</>}
		{identity ? null : <p className={tour.error} role="status">{error}</p>}
		<button className={entry.primary} type="submit" disabled={!valid || busy}><span className={entry.buttonLabel}>{identity ? "Continue" : busy ? "Hiring…" : `Hire ${value.name.trim()}`}</span></button>
	</form>;
}

export const APPLICATIONS = [
	{ key: "word", name: "Word", color: "var(--kind-word)", file: "Agreement.docx", line: "Your Matter beside the document you’re working on." },
	{ key: "excel", name: "Excel", color: "var(--kind-sheet)", file: "Workplan.xlsx", line: "Your team and the context behind every workbook." },
	{ key: "powerpoint", name: "PowerPoint", color: "var(--kind-slides)", file: "Briefing.pptx", line: "Your presentation, connected to the work behind it." },
	{ key: "outlook", name: "Outlook", color: "var(--kind-mail)", line: "Your correspondence and attachments, with their Matter." },
];

// On a Mac the system's own question and Office's own ways are the Mac's.
const onMac = () => document.documentElement.dataset.nativePlatform == "darwin" || (navigator.platform ?? "").startsWith("Mac");

// What the system asks as Singularity's certificate goes onto this computer,
// and the answer to give: said before the person presses, and while the
// system waits for them (D-191).
function trustWords(mac) {
	return mac
		? "macOS asks for your Mac’s password to trust Singularity’s certificate. Enter it."
		: "Windows asks to install a certificate for “localhost”: it is Singularity’s own, on this computer. Choose Yes.";
}

// A name Outlook shows, kept whole on its line: its spaces and hyphens never
// break, so "My add-ins" never ends a line at "add-".
const keep = name => name.replace(/ /g, " ").replace(/-/g, "‑");
// A way through Outlook's menus, each name whole, each arrow ending its line.
const menus = (...names) => names.map(keep).join(" → ");

// Outlook takes an add-in only from the person, in its own Add-ins window:
// Microsoft lets no program add one on a Mac, and on Windows only through the
// mailbox (docs/OFFICE.md). So the steps name Outlook's own way on each
// system, and the add-in's file is pasted where Outlook asks for it, not read
// out: its place goes onto the clipboard ("copy").
function instructions(app, status, { needsTrust, mac }) {
	const trust = needsTrust ? [["Allow it on this computer", trustWords(mac)]] : [];
	if (app.key === "outlook") return [
		...trust,
		mac
			? ["Open Outlook’s add-ins", `In your mailbox, choose ${menus("Get Add-ins", "My add-ins")}.`]
			: ["Open Outlook’s add-ins", `In an Outlook.com or Microsoft 365 mailbox, choose ${menus("More apps", "Add apps", "My add-ins")}.`],
		mac
			? ["Add Singularity", `Choose ${menus("Add a custom add-in", "Add from File")}. Press ${keep("Shift-Command-G")}, paste, press Return, then Open and Install.`, "copy"]
			: ["Add Singularity", `Choose ${menus("Add a custom add-in", "Add from file")}. Paste into ${keep("File name")}, then Open and Install.`, "copy"],
		["Open it in an email", `Choose Singularity in an email. Check that both show the same code, then press ${keep("Allow Outlook")}.`],
	];
	// On a Mac, Office finds a new add-in only as it starts (docs/OFFICE.md).
	const opens = mac && status?.documentsRegistered !== true
		? `Singularity opens ${app.name} with its pane. Quit ${app.name} first if it is open: it finds the pane only as it starts.`
		: `Singularity opens ${app.name} with its pane on the right.`;
	return [
		...trust,
		[`${app.name} opens with Singularity`, opens],
		["Match the code", `The pane asks to connect by itself. Check that both show the same code, then press Allow ${app.name}.`],
	];
}

// How long the window waits for the pane to ask before it says what to check.
const WAIT_HINT_MS = 45000;

// This computer is connected for the app: its certificate trusted, its panes
// served, and for Word, Excel and PowerPoint the add-in known.
function isReady(status, app) {
	return status?.connected === true && status?.listening === true && status?.certificate === "trusted" && !status?.problem
		&& (app.key === "outlook" || status?.documentsRegistered === true);
}

// Each app connects on this computer as Office reports it (D-105, D-156,
// D-191): this computer first, where the system asks the person; then
// Singularity opens the app itself, Word, Excel and PowerPoint with a file of
// its own whose pane opens and asks to connect; the window shows the pane's
// code and one button. Only a pane whose code the member sees here is
// allowed. Every wait says what it waits for, and ends in words and a button.
// Connected is what Office's allowed panes say, never a stage passed.
export function ConnectionAsk({ draft, onChange, onDone, setup, busy = false, setBusy }) {
	const [local, setLocal] = useState({ selected: null, stage: 0, connected: {} });
	const [office, setOffice] = useState({ status: null, clients: [], pending: [] });
	const [error, setError] = useState("");
	// What the window waits for while it works: the system's question, or the app opening.
	const [phase, setPhase] = useState(null);
	const [failed, setFailed] = useState(false);
	const [waitingSince, setWaitingSince] = useState(() => Date.now());
	const [late, setLate] = useState(false);
	const [offline, setOffline] = useState(() => navigator.onLine === false);
	// The add-in's place on the clipboard, said for a moment after it is copied.
	const [copied, setCopied] = useState(false);
	const value = draft ?? local;
	const change = patch => { const next = { ...value, ...patch }; setLocal(next); onChange(next); };
	const app = APPLICATIONS.find(item => item.key === value.selected);
	const isPreview = office.status?.preview === true;
	useEffect(() => {
		if (!setup?.officeStatus) return undefined;
		let live = true;
		const read = () => Promise.all([setup.officeStatus(), setup.officeClients()]).then(([status, found]) => {
			if (live) setOffice({ status, clients: found?.clients ?? [], pending: found?.pending ?? [] });
		}, () => {});
		read();
		const off = setup.onPairing(read);
		// While a pane is being connected its request is looked for often.
		const timer = setInterval(read, value.selected ? 2500 : 10000);
		return () => { live = false; off(); clearInterval(timer); };
	}, [setup, value.selected]);
	useEffect(() => {
		const update = () => setOffline(navigator.onLine === false);
		window.addEventListener("online", update);
		window.addEventListener("offline", update);
		return () => { window.removeEventListener("online", update); window.removeEventListener("offline", update); };
	}, []);
	const isConnected = key => isPreview ? value.connected?.[key] === true : office.clients.some(client => client.client === key);
	const request = app ? office.pending.find(entry => entry.client === app.key) : null;
	// Waiting for the pane to ask: after a while the window says what to check.
	// A code that goes, as when its request ended and the pane asks anew,
	// starts the wait again.
	const isWaiting = app != null && value.stage === 1 && !isPreview && request == null;
	useEffect(() => {
		if (request == null) return undefined;
		return () => setWaitingSince(Date.now());
	}, [request?.pairingId]);
	useEffect(() => {
		setLate(false);
		if (!isWaiting) return undefined;
		const timer = setTimeout(() => setLate(true), Math.max(0, WAIT_HINT_MS - (Date.now() - waitingSince)));
		return () => clearTimeout(timer);
	}, [isWaiting, waitingSince]);
	// A pane of this app allowed already, as when its earlier connection stands.
	const isAllowedNow = app != null && !isPreview && isConnected(app.key);
	useEffect(() => {
		if (isWaiting && isAllowedNow) change({ selected: null, stage: 0, justConnected: app.key });
	}, [isWaiting, isAllowedNow]);
	useEffect(() => {
		if (!copied) return undefined;
		const timer = setTimeout(() => setCopied(false), 3000);
		return () => clearTimeout(timer);
	}, [copied]);
	// The place of Outlook's add-in file, put on the clipboard: at the press
	// that opens Outlook, so the person can paste it there at once, and again
	// at "Copy the add-in’s place". Copied is said only once it is.
	const copyManifest = () => {
		const place = office.status?.outlookManifest;
		if (!place || typeof navigator.clipboard?.writeText != "function") return;
		navigator.clipboard.writeText(place).then(() => setCopied(true), () => setCopied(false));
	};
	const run = async work => {
		setError("");
		setBusy?.(true);
		try { return await work(); }
		catch (failure) { setError(failure?.message || failure?.detail || "It didn’t connect. Try again."); return null; }
		finally { setBusy?.(false); }
	};
	// Opens the app with its pane; says what stood in the way when it did not open.
	const open = async () => {
		setPhase("opening");
		const opened = await setup.openOfficeApp(app.key);
		if (opened?.opened !== true && opened?.problem) {
			setError(opened.problem);
			setFailed(true);
			return false;
		}
		setWaitingSince(Date.now());
		return true;
	};
	// This computer first, once, for every app: the certificate the system asks
	// about, and the panes served here; then the app itself.
	const begin = async () => {
		if (app.key === "outlook") copyManifest();
		if (isPreview) return change({ stage: 1 });
		setFailed(false);
		await run(async () => {
			try {
				if (!isReady(office.status, app)) {
					setPhase(office.status?.certificate === "trusted" ? "opening" : "trusting");
					const status = await setup.connectOffice();
					if (status != null) setOffice(previous => ({ ...previous, status }));
					if (status?.connected !== true || status?.problem) {
						setError(status?.problem || "Singularity did not connect on this computer. Try again.");
						setFailed(true);
						return;
					}
				}
				if (await open()) change({ stage: 1 });
			} catch (failure) {
				setFailed(true);
				throw failure;
			} finally {
				setPhase(null);
			}
		});
	};
	const openAgain = () => run(async () => {
		try { await open(); } finally { setPhase(null); }
	});
	const allow = async () => {
		if (isPreview) return change({ selected: null, stage: 0, justConnected: app.key, connected: { ...value.connected, [app.key]: true } });
		if (request == null) return;
		// The pane takes its key on its next ask, a second or so after the
		// press: Office's panes are read again until they name it, for a few
		// seconds at most, so the app reads Connected once it is (2026-11-01:
		// Outlook stood Not connected for ten seconds after Allow).
		const found = await run(async () => {
			await setup.allowPane(request.pairingId);
			let listed = null;
			for (let attempt = 0; attempt < 8; attempt += 1) {
				listed = await setup.officeClients().catch(() => null);
				if (listed?.clients?.some(client => client.client === app.key)) break;
				await new Promise(resolve => setTimeout(resolve, 750));
			}
			return listed ?? {};
		});
		if (found == null) return;
		if (found.clients != null) setOffice(previous => ({ ...previous, clients: found.clients ?? [], pending: found.pending ?? [] }));
		change({ selected: null, stage: 0, justConnected: app.key });
	};
	if (!app) return <div className={tour.ask}>
		<div className={styles.apps} aria-label="Applications">{APPLICATIONS.map(item => {
			const connected = isConnected(item.key);
			return <button className={styles.app} key={item.key} type="button" data-connected={connected || undefined} data-celebrate={value.justConnected === item.key || undefined} style={{ "--app-color": item.color }} aria-label={`${item.name}: ${connected ? "Connected" : "Not connected"}`} onClick={() => { setError(""); change({ selected: item.key, stage: 0, justConnected: null }); }}>
				<AppIdentity app={item} connected={connected} />
				{connected ? <span className={styles.appFilled} aria-hidden="true"><AppIdentity app={item} connected /></span> : null}
			</button>;
		})}</div>
		<p className={tour.help}>{office.status?.problem || (office.status?.supported === false ? "Word, Excel, PowerPoint and Outlook do not run on this system." : "Each app connects separately on this computer.")}</p>
		<button className={entry.primary} type="button" onClick={onDone}><span className={entry.buttonLabel}>Continue</span></button>
	</div>;
	const code = isPreview ? "384219" : request?.code ?? "";
	const mac = onMac();
	const isOutlook = app.key === "outlook";
	const steps = instructions(app, office.status, { needsTrust: !isPreview && office.status?.certificate !== "trusted", mac });
	// What the window waits for while it works, said as it waits.
	const doing = phase === "trusting"
		? mac ? "macOS asks for your Mac’s password. Enter it to trust Singularity’s certificate." : "Windows asks to install a certificate for “localhost”: it is Singularity’s own, on this computer. Choose Yes in its window."
		: phase === "opening" ? `Singularity opens ${app.name}.` : "";
	// Waiting for the pane to ask: what it waits for, and after a while what to check.
	const waiting = offline
		? `This computer is offline. ${app.name} needs the internet to open Singularity’s pane. Connect to the internet, then press Open ${app.name} again.`
		: late
			? isOutlook
				? "Outlook’s pane has not asked to connect yet. Add Singularity as above, then choose it in an email."
				: `${app.name}’s pane has not asked to connect yet. If it shows a message, follow it, then press Open ${app.name} again.${mac ? ` If ${app.name} was open before, quit it first.` : ""}`
			// Outlook's steps stay in view and already say that its pane asks.
			: isOutlook ? "" : `${app.name} opens with Singularity’s pane on the right. The pane asks to connect by itself.`;
	const canOpenAgain = late || offline;
	const label = phase === "trusting" ? `Waiting for ${mac ? "macOS" : "Windows"}…`
		: phase === "opening" ? `Opening ${app.name}…`
		: value.stage === 0 ? (failed ? "Try again" : `Connect ${app.name}`)
		: code ? `Allow ${app.name}` : canOpenAgain ? `Open ${app.name} again` : `Waiting for ${app.name}…`;
	const place = office.status?.outlookManifest;
	const list = <ol className={styles.steps}>{steps.map(([title, line, action], index) => <li key={title}><span className={styles.number}>{index + 1}</span><span><strong>{title}</strong><p>{line}</p>
		{action === "copy" && place && value.stage === 1 ? <button type="button" className={`${tour.inline} ${styles.copy}`} title={place} onClick={copyManifest}>{copied ? <><Tick />Copied</> : "Copy the add-in’s place"}</button> : null}
	</span></li>)}</ol>;
	return <div className={tour.ask}>
		{value.stage === 0 ? list : code ? <div className={styles.pairing}>
			<p className={styles.pairTitle}>Does {app.name} show this code?</p>
			<div className={styles.code} aria-label={`Pairing code ${code.slice(0, 3)} ${code.slice(3)}`}><span>{code.slice(0, 3)}</span><span>{code.slice(3)}</span></div>
			<p className={tour.help}>Allow only the pane you opened. It will work with your Matters and their team.</p>
		</div> : isOutlook ? list : <div className={styles.pairing}>
			<p className={styles.pairTitle}>Waiting for {app.name}</p>
			<p className={tour.help} role="status">{waiting}</p>
		</div>}
		{value.stage === 0 && doing ? <p className={tour.help} role="status">{doing}</p> : null}
		{value.stage === 1 && !code && isOutlook && waiting ? <p className={tour.help} role="status">{waiting}</p> : null}
		<p className={tour.error} role="status">{error}</p>
		<button className={`${entry.primary} ${styles.held}`} type="button" disabled={busy || phase != null || (value.stage === 1 && !code && !canOpenAgain)} onClick={value.stage === 0 ? begin : code ? allow : openAgain}><span className={entry.buttonLabel}>{label}</span></button>
	</div>;
}

function Tick() {
	return <svg className={styles.tick} width="13" height="13" viewBox="0 0 16 16" fill="none" aria-hidden="true"><path d="m3 8 3.25 3.25L13 4.5" pathLength="1" stroke="currentColor" strokeWidth="1.6" strokeLinecap="round" strokeLinejoin="round" /></svg>;
}

function AppIdentity({ app, connected }) {
	return <><span className={styles.appIcon}>{app.file ? <FileKindIcon name={app.file} size="row" /> : <OutlookIcon />}</span><span className={styles.appText}><strong>{app.name}</strong><span className={styles.appStatus}>{connected ? <Tick /> : null}{connected ? "Connected" : "Not connected"}</span></span></>;
}
