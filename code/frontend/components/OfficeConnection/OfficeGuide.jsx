import { useCallback, useEffect, useState, useSyncExternalStore } from "react";
import { createPortal } from "react-dom";

import { Button } from "../Button/Button.jsx";
import { Icon } from "../Icon/Icon.jsx";
import { nativeApp, openInWord, realDocuments } from "../../data/native.js";

import styles from "./OfficeGuide.module.css";

// Connecting Word, Excel, PowerPoint and Outlook to Singularity (D-105), as one guide over
// the whole window (the owner, 2026-10-24: "make the onboarding as clear as
// possible inside our app; diagrams by stage; always check that the person did
// everything right; it may cover the whole interface"). Each stage shows the
// place to press as a sketch of the application's own screen, says the steps
// in order, and checks itself what was done - the certificate trusted, the
// add-in known to Word, each pane connected - before it moves on. A pane's
// request to connect is answered here, with its code.

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

	// What was done is checked as the person does it.
	useEffect(() => {
		read();
		const timer = setInterval(read, 2000);
		const off = nativeApp().on?.("office.pairingRequested", read);
		const onKey = event => {
			if (event.key == "Escape") onClose();
		};
		window.addEventListener("keydown", onKey);
		return () => {
			clearInterval(timer);
			if (typeof off == "function") off();
			window.removeEventListener("keydown", onKey);
		};
	}, [read, onClose]);

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
	const stages = [
		{ key: "computer", label: "This computer", line: "The connection and its certificate" },
		{ key: "word", label: "Word, Excel and PowerPoint", line: "The Matter button" },
		{ key: "outlook", label: "Outlook", line: "The add-in, added once" },
	];
	const next = stages.find(stage => !done[stage.key])?.key ?? "finished";
	const current = chosen ?? next;
	const allDone = next == "finished";
	const pending = clients.pending ?? [];
	const go = key => setChosen(key);

	return createPortal(
		<div className={styles.guide} role="dialog" aria-modal="true" aria-label="Connect Word, Excel, PowerPoint and Outlook">
			<header className={styles.top}>
				<img className={styles.top_icon} src="app-icon.png" width="28" height="28" alt="" />
				<p className={styles.top_title}>Singularity in Word, Excel, PowerPoint and Outlook</p>
				<Button variant={allDone ? "primary" : "quiet"} onClick={onClose}>{allDone ? "Done" : "Finish later"}</Button>
			</header>
			<div className={styles.body}>
				<nav className={styles.stages} aria-label="Stages">
					{stages.map((stage, index) => (
						<button
							key={stage.key}
							type="button"
							className={current == stage.key ? `${styles.stage_button} ${styles.stage_on}` : styles.stage_button}
							aria-current={current == stage.key ? "step" : undefined}
							onClick={() => go(stage.key)}
						>
							<span className={done[stage.key] ? `${styles.marker} ${styles.marker_done}` : styles.marker}>
								{done[stage.key] ? <Icon name="check" size={14} /> : index + 1}
							</span>
							<span className={styles.stage_text}>
								<span className={styles.stage_label}>{stage.label}</span>
								<span className={styles.stage_line}>{done[stage.key] ? "Done" : stage.line}</span>
							</span>
						</button>
					))}
					<button
						type="button"
						className={current == "finished" ? `${styles.stage_button} ${styles.stage_on}` : styles.stage_button}
						onClick={() => go("finished")}
					>
						<span className={allDone ? `${styles.marker} ${styles.marker_done}` : styles.marker}>{allDone ? <Icon name="check" size={14} /> : 4}</span>
						<span className={styles.stage_text}>
							<span className={styles.stage_label}>Connected panes</span>
							<span className={styles.stage_line}>{clients.clients.length == 0 ? "None yet" : `${clients.clients.length} allowed`}</span>
						</span>
					</button>
				</nav>
				<main className={styles.stage}>
					{pending.map(request => (
						<PairingRequest key={request.pairingId} request={request} busy={busy}
							onAnswer={allow => act(() => nativeApp().decideOfficePairing(request.pairingId, allow))} />
					))}
					{status == null ? <p className={styles.line}>Reading the connection…</p> : null}
					{status != null && current == "computer" ? (
						<ComputerStage status={status} done={done.computer} busy={busy} act={act} onNext={() => go("word")} />
					) : null}
					{status != null && current == "word" ? (
						<WordStage status={status} done={done.word} computerDone={done.computer} onBack={() => go("computer")} onNext={() => go("outlook")} />
					) : null}
					{status != null && current == "outlook" ? (
						<OutlookStage status={status} done={done.outlook} computerDone={done.computer} onBack={() => go("computer")} onNext={() => go("finished")} />
					) : null}
					{status != null && current == "finished" ? (
						<FinishedStage clients={clients.clients} busy={busy} act={act} done={done} status={status}
							onRemove={() => act(() => nativeApp().disconnectOffice()).then(() => go("computer"))} />
					) : null}
					{problem != null ? <p className={styles.problem}>{problem}</p> : null}
				</main>
			</div>
		</div>,
		document.body,
	);
}

// ---- The stages ---------------------------------------------------------------------------

function ComputerStage({ status, done, busy, act, onNext }) {
	const notTrusted = status.connected == true && status.certificate != "trusted";
	const mac = onMac();
	return (
		<section className={styles.panel}>
			<h2 className={styles.title}>Allow the connection on this computer</h2>
			<p className={styles.lead}>
				Singularity shows its pane inside Word, Excel, PowerPoint and Outlook from this computer only, and nothing leaves it. For that, {system()} needs
				to trust one certificate of Singularity's, for "localhost" alone. It is no authority: it cannot vouch for any other site,
				and it goes when you remove Singularity from Office.
			</p>
			<div className={mac ? `${styles.sheet} ${styles.sheet_plain}` : styles.sheet}>
				{mac ? null : <WindowsTrustSketch />}
				<ol className={styles.steps}>
					<li><span className={styles.step_number}>1</span><span>Press <b>Connect</b> below.</span></li>
					{mac ? (
						<li><span className={styles.step_number}>2</span><span>macOS asks for the password you sign in to this Mac with, to trust a certificate for "localhost". Enter it.</span></li>
					) : (
						<li><span className={styles.step_number}>2</span><span>Windows asks whether to install a certificate for "localhost". Press <b>{said("Yes", "Да")}</b>.</span></li>
					)}
				</ol>
			</div>
			<Check
				done={done}
				doneWords={`Connected. ${system()} trusts Singularity's certificate on this computer.`}
				waitingWords={notTrusted
					? mac
						? "macOS does not trust the certificate yet. Press Connect again, and enter your Mac's password when macOS asks."
						: "Windows does not trust the certificate yet. Press Connect again, and Yes in Windows' question."
					: "Not connected yet."}
			/>
			{status.supported == false ? <p className={styles.problem}>Word, Excel, PowerPoint and Outlook do not run on this system.</p> : null}
			{status.problem ? <p className={styles.problem}>{status.problem}</p> : null}
			<div className={styles.actions}>
				{done ? (
					<Button variant="primary" onClick={onNext}>Next: Word, Excel and PowerPoint</Button>
				) : (
					<Button variant="primary" busy={busy} disabled={status.supported == false} onClick={() => act(() => nativeApp().connectOffice())}>Connect</Button>
				)}
			</div>
		</section>
	);
}

function WordStage({ status, done, computerDone, onBack, onNext }) {
	const document = realDocuments().find(entry => /\.docx$/i.test(entry.name ?? "") && entry.processingState != "unavailable") ?? null;
	return (
		<section className={styles.panel}>
			<h2 className={styles.title}>Add the Matter button to Word</h2>
			<p className={styles.lead}>
				Singularity has given Word, Excel and PowerPoint its add-in.
				{onMac() ? " On a Mac, Office finds it only as it starts: quit Word, Excel and PowerPoint if they are open, and open them again." : ""}
				{" "}The first time in each, you start it from the Add-ins menu; after that its Matter button stays on Home.
			</p>
			<div className={styles.sheet}>
				<WordSketch />
				<ol className={styles.steps}>
					<li><span className={styles.step_number}>1</span><span>In Word, on <b>{said("Home", "Главная")}</b>, press <b>{said("Add-ins", "Надстройки")}</b>.</span></li>
					<li><span className={styles.step_number}>2</span><span>Under <b>{said("Developer Add-ins", "Надстройки разработчика")}</b>, press <b>Singularity</b>.</span></li>
					<li><span className={styles.step_number}>3</span><span>Press <b>Matter</b>, which now stands on Home. The pane opens on the right.</span></li>
					<li><span className={styles.step_number}>4</span><span>The pane asks to connect by itself. Allow it here: the code is the same in both.</span></li>
				</ol>
			</div>
			<Check
				done={done}
				doneWords="Word is connected. Excel and PowerPoint show the same Matter button: Home, Add-ins, Singularity."
				waitingWords={!computerDone ? "This computer is not connected yet: that comes first." : status.documentsRegistered ? "Waiting for Word's pane to ask to connect." : "Word does not know the add-in yet. Connect this computer first."}
			/>
			<div className={styles.actions}>
				{!computerDone ? <Button onClick={onBack}>Back to This computer</Button> : null}
				{document != null && !done ? (
					<Button onClick={() => openInWord(document.id).catch(() => {})}>Open {(document.displayName ?? document.name).replace(/\.[^.]+$/, "")} in Word</Button>
				) : null}
				{done ? <Button variant="primary" onClick={onNext}>Next: Outlook</Button> : null}
			</div>
		</section>
	);
}

function OutlookStage({ status, done, computerDone, onBack, onNext }) {
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
	return (
		<section className={styles.panel}>
			<h2 className={styles.title}>Add Singularity to Outlook</h2>
			<p className={styles.lead}>
				Outlook keeps its add-ins in your mailbox, not on this computer, so you add this one yourself, once.
				{mac
					? " On a Mac it works with Microsoft 365, Outlook.com and Gmail mailboxes."
					: " It works with Microsoft 365 and Outlook.com mailboxes; Gmail and other IMAP mailboxes take no add-ins."}
			</p>
			<div className={mac ? `${styles.sheet} ${styles.sheet_plain}` : styles.sheet}>
				{mac ? null : <OutlookSketch />}
				<ol className={styles.steps}>
					<li>
						<span className={styles.step_number}>1</span>
						<span className={styles.step_body}>
							<span>Copy where the add-in's file is:</span>
							<span className={styles.copy_row}>
								<Button size="small" icon={copied ? "check" : "copy"} disabled={!status.outlookManifest} onClick={copy}>{copied ? "Copied" : "Copy the file's place"}</Button>
							</span>
						</span>
					</li>
					{mac ? (
						<>
							<li><span className={styles.step_number}>2</span><span>In Outlook, select a folder of your mailbox, such as its Inbox. Press the button with three dots at the right of the toolbar, then <b>Get Add-ins</b>.</span></li>
							<li><span className={styles.step_number}>3</span><span>Choose <b>My add-ins</b>. Under Custom Addins, press <b>Add a custom add-in</b>, then <b>Add from File…</b></span></li>
							<li><span className={styles.step_number}>4</span><span>In the window that opens, press Shift-Command-G, paste, and press Return. Press <b>Open</b>, then <b>Install</b>.</span></li>
							<li><span className={styles.step_number}>5</span><span>In a letter, press the button with three dots again, then <b>Singularity</b>. Its pane asks to connect by itself: allow it here.</span></li>
						</>
					) : (
						<>
							<li><span className={styles.step_number}>2</span><span>In Outlook, select a folder of your Microsoft 365 or Outlook.com mailbox, such as its Inbox: Outlook offers add-ins only there. Then, on <b>Home</b>, press <b>More apps</b> at the right, under Add-ins, then <b>Add apps</b>.</span></li>
							<li><span className={styles.step_number}>3</span><span>Choose <b>{said("My add-ins", "Мои надстройки")}</b>. At the bottom, under Custom add-ins, press <b>{said("Add a custom add-in", "Добавить настраиваемую надстройку")}</b>, then <b>{said("Add from file", "Добавить из файла")}</b>.</span></li>
							<li><span className={styles.step_number}>4</span><span>Paste into <b>{said("File name", "Имя файла")}</b>, press <b>{said("Open", "Открыть")}</b>, then <b>{said("Install", "Установить")}</b>.</span></li>
							<li><span className={styles.step_number}>5</span><span>Open a letter, press <b>More apps</b> again, then <b>Singularity</b>. Its pane asks to connect by itself: allow it here.</span></li>
						</>
					)}
				</ol>
			</div>
			<Check
				done={done}
				doneWords="Outlook is connected. Open a letter, and its Matter opens in the pane."
				waitingWords={!computerDone ? "This computer is not connected yet: that comes first." : "Waiting for Outlook's pane to ask to connect."}
			/>
			<div className={styles.actions}>
				{!computerDone ? <Button onClick={onBack}>Back to This computer</Button> : null}
				{done ? <Button variant="primary" onClick={onNext}>Next: Connected panes</Button> : null}
			</div>
		</section>
	);
}

function FinishedStage({ clients, busy, act, done, status, onRemove }) {
	return (
		<section className={styles.panel}>
			<h2 className={styles.title}>{done.computer && done.word && done.outlook ? "Word, Excel, PowerPoint and Outlook work with Singularity" : "The panes allowed on this computer"}</h2>
			<p className={styles.lead}>Each pane asked once, with its code, and reads only the Matters open in Singularity here. Remove one, and it asks again the next time.</p>
			{clients.length == 0 ? <p className={styles.line}>No pane is allowed yet.</p> : null}
			<ul className={styles.clients}>
				{clients.map(client => (
					<li key={client.clientId} className={styles.client}>
						<span className={styles.client_text}>
							<span className={styles.client_name}>{client.label || APPLICATION[client.client]}</span>
							<span className={styles.line}>Allowed {day(client.pairedAt)}, last used {day(client.lastUsedAt)}</span>
						</span>
						<Button size="small" variant="quiet" disabled={busy} onClick={() => act(() => nativeApp().removeOfficeClient(client.clientId))}>Remove</Button>
					</li>
				))}
			</ul>
			{status.connected ? (
				<div className={styles.actions}>
					<Button variant="quiet" disabled={busy} onClick={onRemove}>Remove Singularity from Office</Button>
				</div>
			) : null}
		</section>
	);
}

// A pane's request to connect, answered here: the code is the one the pane shows.
function PairingRequest({ request, busy, onAnswer }) {
	const application = APPLICATION[request.client] ?? "A program";
	return (
		<section className={styles.request} aria-live="polite">
			<div className={styles.request_text}>
				<p className={styles.request_title}>{application} asks to connect</p>
				<p className={styles.line}>Allow it only if its pane shows this code. It may then read your Matters here and give work to your colleagues.</p>
			</div>
			<p className={styles.code}>{request.code.slice(0, 3)} {request.code.slice(3)}</p>
			<div className={styles.request_actions}>
				<Button variant="quiet" disabled={busy} onClick={() => onAnswer(false)}>Don't allow</Button>
				<Button variant="primary" disabled={busy} onClick={() => onAnswer(true)}>Allow</Button>
			</div>
		</section>
	);
}

// What the guide sees now, said in a line.
function Check({ done, doneWords, waitingWords }) {
	return (
		<p className={done ? `${styles.check} ${styles.check_done}` : styles.check} aria-live="polite">
			<span className={styles.check_icon}>{done ? <Icon name="check" size={16} /> : <span className={styles.watching} aria-hidden="true" />}</span>
			<span>{done ? doneWords : waitingWords}</span>
		</p>
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
				<p className={styles.win_title}>{said("Security Warning", "Предупреждение системы безопасности")}</p>
				<div className={styles.win_body}>
					<span className={styles.win_shield}><Icon name="shield" size={22} /></span>
					<span className={styles.win_text}>You are about to install a certificate claiming to represent: <b>localhost</b>. Do you want to install it?</span>
				</div>
				<div className={styles.win_buttons}>
					<span className={`${styles.win_button} ${styles.target}`}>{said("Yes", "Да")}<Mark number={2} /></span>
					<span className={styles.win_button}>{said("No", "Нет")}</span>
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
					<span className={styles.tab_on}>{isRussian ? "Главная" : "Home"}</span>
					<span>Insert</span>
					<span>Layout</span>
					<span>Review</span>
				</div>
				<div className={styles.ribbon}>
					<span className={styles.ribbon_group} />
					<span className={styles.ribbon_group} />
					<span className={`${styles.ribbon_button} ${styles.target}`}><Icon name="plus" size={14} />{isRussian ? "Надстройки" : "Add-ins"}<Mark number={1} /></span>
					<span className={`${styles.ribbon_button} ${styles.target}`}><img src="app-icon.png" width="14" height="14" alt="" />Matter<Mark number={3} /></span>
				</div>
				<div className={styles.app_page}>
					<div className={styles.menu}>
						<p className={styles.menu_heading}>{isRussian ? "Надстройки разработчика" : "Developer Add-ins"}</p>
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
