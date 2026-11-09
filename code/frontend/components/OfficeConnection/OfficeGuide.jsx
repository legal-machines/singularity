import { useCallback, useEffect, useState, useSyncExternalStore } from "react";

import { Button } from "../Button/Button.jsx";
import { Dialog } from "../Dialog/Dialog.jsx";
import { Icon } from "../Icon/Icon.jsx";
import { nativeApp, openInWord, realDocuments } from "../../data/native.js";

import styles from "./OfficeGuide.module.css";

// Connecting Word, Excel, PowerPoint and Outlook to Singularity, as one of the
// window's dialogs over the white.
//
// It is one list of what is connected: this computer; Word, Excel and PowerPoint;
// Outlook; the panes allowed. Each says its state in a word, and the one at hand
// stands open: one line, the steps beside a sketch of the application's own screen
// with the places to press marked, and its one action. What is checked is checked
// as the person does it. A pane asking to connect is answered in its own
// application's part, its code beside the question, so it reads as Singularity's
// question about that pane. Weight is the hierarchy's: medium for the title, each
// part's name and the code alone; every other word regular.

let isGuideOpen = false;
const listeners = new Set();

function setGuideOpen(value) {
	isGuideOpen = value;
	for (const listener of listeners) listener();
}

// Whether the guide is open: the window's own question about a pane then
// stays quiet, as the guide asks it.
export function useOfficeGuideOpen() {
	return useSyncExternalStore(
		listener => {
			listeners.add(listener);
			return () => listeners.delete(listener);
		},
		() => isGuideOpen,
	);
}

// Each Office application a pane is in, by the client kind it pairs as.
export const APPLICATION = { word: "Word", excel: "Excel", powerpoint: "PowerPoint", outlook: "Outlook" };

// Office's own words where Office is in Russian, beside the English ones.
const isRussian = (navigator.language ?? "").toLowerCase().startsWith("ru");
const said = (english, russian) => (isRussian ? `${english} (${russian})` : english);

// On a Mac the steps are the Mac's own, as Office for Mac 16.114 shows them
// (checked 2026-10-25): macOS asks for the Mac's password to trust the
// certificate, Office finds a new add-in only as it starts, and Outlook keeps
// its add-ins behind the toolbar's button with three dots. The sketches of
// Windows' screens are not shown there.
const onMac = () => document.documentElement.dataset.nativePlatform == "darwin" || (navigator.platform ?? "").startsWith("Mac");
const system = () => (onMac() ? "macOS" : "Windows");

const PARTS = [
	{ id: "computer", name: "This computer" },
	{ id: "word", name: "Word, Excel and PowerPoint" },
	{ id: "outlook", name: "Outlook" },
	{ id: "panes", name: "Allowed panes" },
];

// The part a pane's request is answered in.
const partOf = client => (client == "outlook" ? "outlook" : "word");

export function OfficeGuide({ onClose }) {
	const [status, setStatus] = useState(null);
	const [clients, setClients] = useState({ pending: [], clients: [] });
	const [chosen, setChosen] = useState(null);
	const [busy, setBusy] = useState(false);
	const [problem, setProblem] = useState(null);

	useEffect(() => {
		setGuideOpen(true);
		return () => setGuideOpen(false);
	}, []);

	const read = useCallback(async () => {
		try {
			const [now, paired] = await Promise.all([nativeApp().getOfficeStatus(), nativeApp().listOfficeClients()]);
			setStatus(now);
			setClients(paired);
		} catch (error) {
			setProblem(error?.message ?? "Singularity could not read the connection.");
		}
	}, []);

	// Word opened for the first time while the guide waited for it: Connect
	// finishes by itself, with nothing more to press.
	const firstOpening = status?.firstOpening ?? "none";
	const [isFinishing, setFinishing] = useState(false);
	useEffect(() => {
		if (firstOpening != "opened" || isFinishing) return;
		setFinishing(true);
		nativeApp().connectOffice().catch(() => {}).finally(async () => {
			await read();
			setFinishing(false);
		});
	}, [firstOpening, isFinishing, read]);

	// What was done is checked as the person does it.
	useEffect(() => {
		read();
		const timer = setInterval(read, 2000);
		const off = nativeApp().on?.("office.pairingRequested", read);
		return () => {
			clearInterval(timer);
			if (typeof off == "function") off();
		};
	}, [read]);

	const act = async work => {
		setBusy(true);
		setProblem(null);
		try {
			await work();
		} catch (error) {
			setProblem(error?.message ?? "It did not work.");
		}
		await read();
		setBusy(false);
	};

	const has = kind => clients.clients.some(client => client.client == kind);
	const done = {
		computer: status?.connected == true && status?.listening == true && status?.certificate == "trusted",
		word: has("word") || has("excel") || has("powerpoint"),
		outlook: has("outlook"),
	};
	const pending = clients.pending ?? [];
	const next = ["computer", "word", "outlook"].find(id => !done[id]) ?? "panes";
	const allDone = next == "panes";
	// A pane asking to connect opens its part: its question waits there.
	const asking = pending.length > 0 ? partOf(pending[0].client) : null;
	const open = asking ?? chosen ?? next;
	const answer = request => allow => act(() => nativeApp().decideOfficePairing(request.pairingId, allow));
	const requestsOf = id => pending.filter(request => partOf(request.client) == id);

	const stateOf = id => {
		if (requestsOf(id).length > 0) return { words: "Asks to connect", tone: "asking" };
		if (id == "panes") return { words: clients.clients.length == 0 ? "None yet" : `${clients.clients.length} allowed` };
		if (done[id]) return { words: "Connected", tone: "settled" };
		if (id == "computer") return { words: "Not connected" };
		return { words: done.computer ? "Not yet" : "After this computer" };
	};

	const body = status == null ? <p className={styles.line}>Reading the connection…</p>
		: status.supported == false ? <p className={styles.line}>Word, Excel, PowerPoint and Outlook do not run on this system.</p>
		: (
			<div className={styles.parts}>
				{PARTS.map(part => (
					<Part key={part.id} name={part.name} state={stateOf(part.id)} isOpen={open == part.id} onOpen={() => setChosen(part.id)}>
						{part.id == "computer" ? <ComputerPart status={status} done={done.computer} busy={busy} act={act} /> : null}
						{part.id == "word" ? (
							<WordPart status={status} done={done.word} computerDone={done.computer} requests={requestsOf("word")} answer={answer} busy={busy} />
						) : null}
						{part.id == "outlook" ? (
							<OutlookPart status={status} done={done.outlook} computerDone={done.computer} requests={requestsOf("outlook")} answer={answer} busy={busy} />
						) : null}
						{part.id == "panes" ? (
							<PanesPart clients={clients.clients} busy={busy} act={act} status={status}
								onRemove={() => act(() => nativeApp().disconnectOffice()).then(() => setChosen("computer"))} />
						) : null}
					</Part>
				))}
			</div>
		);

	return (
		<Dialog
			layout="spacious"
			size="wide"
			title="Connect Office"
			description="Singularity in a pane beside your documents and mail."
			onClose={onClose}
			// Finish later stands at the top left, the way out of a task left
			// for later; Done, at the foot, ends it.
			lead={allDone ? null : <Button variant="quiet" size="small" onClick={onClose}>Finish later</Button>}
			actions={allDone ? <Button variant="primary" onClick={onClose}>Done</Button> : null}
		>
			{body}
			{problem != null ? <p className={styles.problem}>{problem}</p> : null}
		</Dialog>
	);
}

// One part of what is connected: its name and its state in a word; open, what
// to do in it. A closed one opens at a press.
function Part({ name, state, isOpen, onOpen, children }) {
	const head = (
		<>
			<span className={styles.part_name}>{name}</span>
			<span className={styles.part_state} data-tone={state.tone ?? undefined}>{state.words}</span>
		</>
	);
	return (
		<section className={styles.part} data-open={isOpen ? "" : undefined}>
			{isOpen
				? <div className={styles.part_head}>{head}</div>
				: <button type="button" className={`${styles.part_head} ${styles.part_button}`} aria-expanded="false" onClick={onOpen}>{head}</button>}
			{isOpen ? <div className={styles.part_body}>{children}</div> : null}
		</section>
	);
}

// ---- The parts ----------------------------------------------------------------------------

function ComputerPart({ status, done, busy, act }) {
	const mac = onMac();
	const notTrusted = status.connected == true && status.certificate != "trusted";
	// Word never opened on this Mac: a step of its own, never a failure.
	const waitsForWord = (status.firstOpening ?? "none") != "none";
	if (done) return <p className={styles.line}>{system()} trusts Singularity's certificate, for "localhost" only.</p>;
	return (
		<>
			<p className={styles.line}>{system()} trusts one certificate, for "localhost" only.</p>
			<div className={mac ? `${styles.sheet} ${styles.sheet_plain}` : styles.sheet}>
				{mac ? null : <WindowsTrustSketch />}
				<ol className={styles.steps}>
					<Step number={1}>Press Connect.</Step>
					{mac
						? <Step number={2}>Enter your Mac's password when macOS asks.</Step>
						: <Step number={2}>Press {said("Yes", "\u0414\u0430")} when Windows asks to install the certificate.</Step>}
					{waitsForWord ? <Step number={3}>Open Word once. Singularity finishes by itself as soon as Word has opened.</Step> : null}
				</ol>
			</div>
			{notTrusted ? <p className={styles.problem}>Not trusted yet. Press Connect again.</p> : null}
			{status.problem && !waitsForWord ? <p className={styles.problem}>{status.problem}</p> : null}
			{waitsForWord ? null : <Button variant="primary" busy={busy} onClick={() => act(() => nativeApp().connectOffice())}>Connect</Button>}
		</>
	);
}

function WordPart({ status, done, computerDone, requests, answer, busy }) {
	const document = realDocuments().find(entry => /\.docx$/i.test(entry.name ?? "") && entry.processingState != "unavailable") ?? null;
	// A pane asks only once its steps are done: its question alone stands then.
	if (requests.length > 0) return requests.map(request => <PairingRequest key={request.pairingId} request={request} busy={busy} onAnswer={answer(request)} />);
	if (done) return <p className={styles.line}>Word is connected. Excel and PowerPoint connect the same way.</p>;
	return (
		<>
			<p className={styles.line}>
				{!computerDone || !status.documentsRegistered ? "Connect this computer first."
					: onMac() ? "Quit and reopen Word first: Office finds the add-in as it starts." : "The add-in is already in Word, Excel and PowerPoint."}
			</p>
			<div className={styles.sheet}>
				<WordSketch />
				<ol className={styles.steps}>
					<Step number={1}>In Word, on {said("Home", "\u0413\u043b\u0430\u0432\u043d\u0430\u044f")}, press {said("Add-ins", "\u041d\u0430\u0434\u0441\u0442\u0440\u043e\u0439\u043a\u0438")}.</Step>
					<Step number={2}>Under {said("Developer Add-ins", "\u041d\u0430\u0434\u0441\u0442\u0440\u043e\u0439\u043a\u0438 \u0440\u0430\u0437\u0440\u0430\u0431\u043e\u0442\u0447\u0438\u043a\u0430")}, press Singularity.</Step>
					<Step number={3}>Press Matter, now on Home. The pane opens.</Step>
					<Step number={4}>The pane shows a code. Allow it here.</Step>
				</ol>
			</div>
			{document != null && computerDone ? (
				<Button onClick={() => openInWord(document.id).catch(() => {})}>Open {(document.displayName ?? document.name).replace(/\.[^.]+$/, "")} in Word</Button>
			) : null}
		</>
	);
}

function OutlookPart({ status, done, computerDone, requests, answer, busy }) {
	const [copied, setCopied] = useState(false);
	const copy = async () => {
		try {
			await navigator.clipboard.writeText(status.outlookManifest);
			setCopied(true);
		} catch {
			setCopied(false);
		}
	};
	const mac = onMac();
	if (requests.length > 0) return requests.map(request => <PairingRequest key={request.pairingId} request={request} busy={busy} onAnswer={answer(request)} />);
	if (done) return <p className={styles.line}>Outlook is connected. A letter's Matter opens in its pane.</p>;
	return (
		<>
			<p className={styles.line}>{computerDone ? "Outlook keeps add-ins in your mailbox, so you add this one once." : "Connect this computer first."}</p>
			<div className={mac ? `${styles.sheet} ${styles.sheet_plain}` : styles.sheet}>
				{mac ? null : <OutlookSketch />}
				<ol className={styles.steps}>
					<Step number={1}>
						<span className={styles.step_body}>
							<span>Copy where the add-in's file is.</span>
							<span className={styles.copy_row}>
								<Button size="small" icon={copied ? "check" : "copy"} disabled={!status.outlookManifest} onClick={copy}>{copied ? "Copied" : "Copy"}</Button>
							</span>
						</span>
					</Step>
					{mac ? (
						<>
							<Step number={2}>In Outlook, in your Inbox, press the button with three dots at the right of the toolbar, then Get Add-ins.</Step>
							<Step number={3}>Choose My add-ins. Under Custom Addins, press Add a custom add-in, then Add from File.</Step>
							<Step number={4}>Press Shift-Command-G, paste, and press Return. Press Open, then Install.</Step>
							<Step number={5}>In a letter, press the three dots, then Singularity. The pane shows a code. Allow it here.</Step>
						</>
					) : (
						<>
							<Step number={2}>In Outlook, in the Inbox of a Microsoft 365 or Outlook.com mailbox, press More apps on Home, then Add apps.</Step>
							<Step number={3}>Choose {said("My add-ins", "\u041c\u043e\u0438 \u043d\u0430\u0434\u0441\u0442\u0440\u043e\u0439\u043a\u0438")}. Under Custom add-ins, press {said("Add a custom add-in", "\u0414\u043e\u0431\u0430\u0432\u0438\u0442\u044c \u043d\u0430\u0441\u0442\u0440\u0430\u0438\u0432\u0430\u0435\u043c\u0443\u044e \u043d\u0430\u0434\u0441\u0442\u0440\u043e\u0439\u043a\u0443")}, then {said("Add from file", "\u0414\u043e\u0431\u0430\u0432\u0438\u0442\u044c \u0438\u0437 \u0444\u0430\u0439\u043b\u0430")}.</Step>
							<Step number={4}>Paste into {said("File name", "\u0418\u043c\u044f \u0444\u0430\u0439\u043b\u0430")}. Press {said("Open", "\u041e\u0442\u043a\u0440\u044b\u0442\u044c")}, then {said("Install", "\u0423\u0441\u0442\u0430\u043d\u043e\u0432\u0438\u0442\u044c")}.</Step>
							<Step number={5}>In a letter, press More apps, then Singularity. The pane shows a code. Allow it here.</Step>
						</>
					)}
				</ol>
			</div>
		</>
	);
}

function PanesPart({ clients, busy, act, status, onRemove }) {
	return (
		<>
			{clients.length == 0 ? <p className={styles.line}>A pane is allowed once it asks, with its code.</p> : (
				<ul className={styles.clients}>
					{clients.map(client => (
						<li key={client.clientId} className={styles.client}>
							<span className={styles.client_text}>
								<span className={styles.client_name}>{client.label || APPLICATION[client.client]}</span>
								<span className={styles.client_line}>Allowed {day(client.pairedAt)}, last used {day(client.lastUsedAt)}</span>
							</span>
							<Button size="small" variant="quiet" disabled={busy} onClick={() => act(() => nativeApp().removeOfficeClient(client.clientId))}>Remove</Button>
						</li>
					))}
				</ul>
			)}
			{status.connected ? <Button disabled={busy} onClick={onRemove}>Remove Singularity from Office</Button> : null}
		</>
	);
}

function Step({ number, children }) {
	return (
		<li>
			<span className={styles.step_number}>{number}</span>
			<span className={styles.step_text}>{children}</span>
		</li>
	);
}

// A pane asking to connect, answered in its application's part: Singularity's
// question about that pane, with the code both show.
function PairingRequest({ request, busy, onAnswer }) {
	const application = APPLICATION[request.client] ?? "The program";
	return (
		<div className={styles.request} aria-live="polite">
			<p className={styles.request_question}>Does {application}'s pane show this code?</p>
			<div className={styles.request_row}>
				<p className={styles.code}>{request.code.slice(0, 3)} {request.code.slice(3)}</p>
				<span className={styles.request_actions}>
					<Button disabled={busy} onClick={() => onAnswer(false)}>No</Button>
					<Button variant="primary" disabled={busy} onClick={() => onAnswer(true)}>Yes, allow</Button>
				</span>
			</div>
		</div>
	);
}

// ---- The sketches: each application's screen, reduced to where to press ---------------------

function Mark({ number }) {
	return <span className={styles.mark} aria-hidden="true">{number}</span>;
}

function WindowsTrustSketch() {
	return (
		<figure className={styles.sketch} aria-label="Windows' security warning, with Yes marked">
			<div className={styles.win_dialog}>
				<p className={styles.win_title}>{said("Security Warning", "\u041f\u0440\u0435\u0434\u0443\u043f\u0440\u0435\u0436\u0434\u0435\u043d\u0438\u0435 \u0441\u0438\u0441\u0442\u0435\u043c\u044b \u0431\u0435\u0437\u043e\u043f\u0430\u0441\u043d\u043e\u0441\u0442\u0438")}</p>
				<div className={styles.win_body}>
					<span className={styles.win_shield}><Icon name="shield" size={22} /></span>
					<span className={styles.win_text}>You are about to install a certificate claiming to represent: <b>localhost</b>. Do you want to install it?</span>
				</div>
				<div className={styles.win_buttons}>
					<span className={`${styles.win_button} ${styles.target}`}>{said("Yes", "\u0414\u0430")}<Mark number={2} /></span>
					<span className={styles.win_button}>{said("No", "\u041d\u0435\u0442")}</span>
				</div>
			</div>
		</figure>
	);
}

function WordSketch() {
	return (
		<figure className={styles.sketch} aria-label="Word's ribbon, with Add-ins, Singularity, Matter and the pane's code marked">
			<div className={styles.app}>
				<div className={`${styles.app_bar} ${styles.word_bar}`}>Word</div>
				<div className={styles.ribbon_tabs}>
					<span>File</span>
					<span className={styles.tab_on}>{isRussian ? "\u0413\u043b\u0430\u0432\u043d\u0430\u044f" : "Home"}</span>
					<span>Insert</span>
					<span>Layout</span>
					<span>Review</span>
				</div>
				<div className={styles.ribbon}>
					<span className={styles.ribbon_group} />
					<span className={styles.ribbon_group} />
					<span className={`${styles.ribbon_button} ${styles.target}`}><Icon name="plus" size={14} />{isRussian ? "\u041d\u0430\u0434\u0441\u0442\u0440\u043e\u0439\u043a\u0438" : "Add-ins"}<Mark number={1} /></span>
					<span className={`${styles.ribbon_button} ${styles.target}`}><img src="app-icon.png" width="14" height="14" alt="" />Matter<Mark number={3} /></span>
				</div>
				<div className={styles.app_page}>
					<div className={styles.menu}>
						<p className={styles.menu_heading}>{isRussian ? "\u041d\u0430\u0434\u0441\u0442\u0440\u043e\u0439\u043a\u0438 \u0440\u0430\u0437\u0440\u0430\u0431\u043e\u0442\u0447\u0438\u043a\u0430" : "Developer Add-ins"}</p>
						<span className={`${styles.menu_tile} ${styles.target}`}><img src="app-icon.png" width="22" height="22" alt="" />Singularity<Mark number={2} /></span>
					</div>
					<div className={styles.paper}>
						<span /><span /><span className={styles.paper_short} />
					</div>
					<div className={styles.side_pane}>
						<p className={styles.pane_head}>Singularity</p>
						<span className={`${styles.pane_button} ${styles.target}`}>384 219<Mark number={4} /></span>
					</div>
				</div>
			</div>
		</figure>
	);
}

function OutlookSketch() {
	return (
		<figure className={styles.sketch} aria-label="Outlook's ribbon and its add-ins, with More apps, My add-ins, Add from file and the file marked">
			<div className={styles.app}>
				<div className={`${styles.app_bar} ${styles.outlook_bar}`}>Outlook</div>
				<div className={styles.ribbon_tabs}>
					<span>File</span>
					<span className={styles.tab_on}>Home</span>
					<span>View</span>
					<span>Help</span>
				</div>
				<div className={styles.ribbon}>
					<span className={styles.ribbon_group} />
					<span className={styles.ribbon_group} />
					<span className={styles.ribbon_group} />
					<span className={`${styles.ribbon_button} ${styles.target}`}><Icon name="plus" size={14} />More apps<Mark number={2} /></span>
				</div>
				<div className={styles.store_page}>
					<div className={styles.store_card}>
						<p className={styles.store_title}>Add-ins for Outlook</p>
						<div className={styles.store_body}>
							<div className={styles.store_nav}>
								<span>All</span>
								<span className={`${styles.store_nav_on} ${styles.target}`}>My add-ins<Mark number={3} /></span>
							</div>
							<div className={styles.store_main}>
								<p className={styles.menu_heading}>Custom add-ins</p>
								<span className={`${styles.pane_button} ${styles.target}`}>Add from file<Mark number={3} /></span>
								<span className={styles.file_row}>
									<span className={`${styles.file_field} ${styles.target}`}>…\Office\singularity-outlook.xml<Mark number={4} /></span>
									<span className={styles.pane_button}>Install</span>
								</span>
							</div>
						</div>
					</div>
				</div>
			</div>
		</figure>
	);
}

function day(at) {
	const moment = new Date(at);
	return Number.isNaN(moment.getTime()) ? "" : new Intl.DateTimeFormat("en-US", { month: "short", day: "numeric" }).format(moment);
}
