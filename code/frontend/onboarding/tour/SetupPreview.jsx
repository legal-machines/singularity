import { useEffect, useLayoutEffect, useRef, useState } from "react";

import { Avatar } from "../../components/Avatar/Avatar.jsx";
import { FileKindIcon } from "../../components/FileKind/FileKind.jsx";
import { Icon } from "../../components/Icon/Icon.jsx";
import { OutlookIcon } from "./OutlookIcon.jsx";
import { platformOf } from "./pages.js";
import styles from "./SetupPreview.module.css";

// MOCK DATA / DESIGN PROTOTYPE: only the new chapter's feature, built with
// the application's faces, files and rows. These samples claim no real saved
// colleague, Office connection or mail access (owner, 2026-10-30). `asked`
// names the permissions the slide asks for, by key.
export function SetupPreview({ kind, draft, asked = null }) {
	return <aside className={styles.stage} aria-hidden="true" key={kind}>
		{kind === "preparation" ? <Preparation /> : kind === "protection" ? <Protection off={draft?.on === false} /> : kind === "certificates" || kind === "permissions" ? <SystemAsks kind={kind} asked={kind === "permissions" ? asked : null} /> : kind === "colleague" ? <Colleague draft={draft} /> : draft?.selected ? <Office app={draft.selected} /> : <AppsOverview />}
	</aside>;
}

// A part of what a slide pictures, set as the app sets its parts now: its
// name, then what it holds on the page's white, apart by space alone - no
// card, frame, small capitals or line between rows (the owner, 2026-11-05:
// onboarding's right side still showed the old design system).
function Part({ name, children }) {
	return <section className={styles.part}>
		{name != null ? <h3 className={styles.part_name}>{name}</h3> : null}
		{children}
	</section>;
}

function Lines({ children }) {
	return <ul className={styles.lines}>{children}</ul>;
}

// A line of a part: its mark, what it says, and the line under it.
function Line({ lead, title, line }) {
	return <li className={styles.line}>
		<span className={styles.line_lead}>{lead}</span>
		<span className={styles.line_text}>
			<span className={styles.line_title}>{title}</span>
			{line != null ? <span className={styles.line_words}>{line}</span> : null}
		</span>
	</li>;
}

// What the computer itself will show, in its own words, and what to choose,
// at the moment it asks (the certificates and permissions slides): set in the
// app's rows, never drawn as the system's own window.
const SYSTEM_ASKS = {
	certificates: {
		win32: { title: "A new version", line: "What Windows shows the first time it starts.", label: "Windows SmartScreen", asks: [
			{ icon: "shield", says: "Windows protected your PC", answer: "Choose More info." },
			{ icon: "check", says: "Publisher: Unknown publisher", answer: "Choose Run anyway." },
		] },
		darwin: { title: "A new version", line: "What macOS shows the first time it opens.", label: "Gatekeeper", asks: [
			{ icon: "shield", says: "“Singularity” Not Opened", answer: "Choose Done." },
			{ icon: "settings", says: "Privacy & Security", answer: "Choose Open Anyway, then confirm." },
		] },
	},
	// Each question the system asks as Allow access is pressed, by the
	// permission it is for, in the order they come (pages.js). Only those the
	// slide asks for are shown: one this computer allows already never comes
	// (the owner, 2026-11-05, D-249). Windows ticks the kind of network it is on,
	// public or private, so its firewall's answer is Allow access as it stands.
	permissions: {
		win32: { title: "This computer", line: "What Windows asks, one after another.", label: "Windows Security", asks: [
			{ key: "controlledFolders", icon: "matters", says: "Unauthorized changes blocked", answer: "Choose Open settings, then allow Singularity." },
			{ key: "officeCertificate", icon: "document", says: "Security Warning", answer: "It names localhost, this computer. Choose Yes." },
			{ key: "firewall", icon: "people", says: "Windows Security Alert", answer: "Choose Allow access." },
		] },
		darwin: { title: "This computer", line: "What macOS asks, one after another.", label: "Privacy & Security", asks: [
			{ key: "documents", icon: "matters", says: "“Singularity” would like to access files in your Documents folder.", answer: "Choose Allow." },
			{ key: "officeApps", icon: "document", says: "“Singularity” would like to access data from other apps.", answer: "Choose Allow, once for each app." },
			{ key: "localNetwork", icon: "people", says: "“Singularity” would like to find and connect to devices on your local network.", answer: "Choose Allow." },
		] },
	},
};

function SystemAsks({ kind, asked }) {
	const shown = SYSTEM_ASKS[kind]?.[platformOf()];
	if (shown == null) return null;
	const asks = asked == null ? shown.asks : shown.asks.filter(ask => ask.key == null || asked.includes(ask.key));
	return <div className={styles.profile}>
		<header className={styles.overviewHead}><h2>{shown.title}</h2><p>{shown.line}</p></header>
		<Part name={shown.label}>
			<Lines>{asks.map(ask => <Line key={ask.says} lead={<Icon name={ask.icon} size={18} />} title={ask.says} line={ask.answer} />)}</Lines>
		</Part>
	</div>;
}

// One clause of the deal in Word after an AI colleague's change, and what was
// done to it on this computer on the way (D-195): read as Word numbers it, its
// names and amount masked for the model, the change returned as a tracked
// change read back, its price checked against the schedule. Fictional, a US
// stock purchase (AG-12): the buyer pays for the shares it buys.
function Preparation() {
	return <div className={styles.picture}>
		<section className={styles.document}>
			<div className={styles.appName}><Icon name="document" size={16} /><span>Stock purchase agreement.docx</span></div>
			<p className={styles.clauseNumber}>4.2 Purchase price</p>
			<p className={styles.passage}>Halvane Robotics will pay $48,000,000 for 80% of Sablewood at Closing, <del>in cash</del> <ins>by wire transfer</ins>.</p>
		</section>
		<Part name="Checked on this computer">
			<Lines>
				<Line lead={<Icon name="document" size={18} />} title="Read as written" line="Clause 4.2, numbered as Word numbers it" />
				<Line lead={<Icon name="check" size={18} />} title="Change checked" line="One tracked change, read back from a copy" />
				<Line lead={<Icon name="matters" size={18} />} title="Figures match" line="The price matches Schedule 2" />
				<Line lead={<Icon name="shield" size={18} />} title="Client data, masked" line="The model read [[Org1]], [[Org2]] and [[Amount1]]" />
			</Lines>
		</Part>
	</div>;
}

// When each step of the protection picture comes, in milliseconds from the
// slide's opening: the sentence copied for sending, its three names masked one
// by one, its leaving, the answer, and its two symbols restored one by one.
// Unhurried, so each change can be read as it happens (the owner, 2026-11-01:
// "slower"): one name a step, each finished before the next begins.
const MASKING_STEPS = [900, 1900, 2600, 3300, 4300, 5500, 6500, 7200];

function reducedMotion() {
	return typeof window != "undefined" && window.matchMedia?.("(prefers-reduced-motion: reduce)").matches === true;
}

// One sentence of the deal on its way (D-195): written on this computer with
// its client's names and figures, masked here before it leaves for the AI
// model, and the answer restored here. As the slide opens it plays once, in
// the order it happens (the owner, 2026-11-01, D-198): the sentence is copied
// for sending, its names become symbols while it is still on this computer's
// white, it leaves on its green ground, and the answer's symbols become names
// again here. Under reduced motion it shows the end at once. With protection
// off, what leaves stands as written on a warning ground, and nothing is
// masked or restored. Every row keeps the height of its fullest state, so
// nothing below it moves while it plays or as the member switches. Fictional.
function Protection({ off = false }) {
	const [step, setStep] = useState(() => (reducedMotion() ? MASKING_STEPS.length : 0));
	useEffect(() => {
		if (reducedMotion()) return undefined;
		const timers = MASKING_STEPS.map((at, index) => setTimeout(() => setStep(shown => Math.max(shown, index + 1)), at));
		return () => timers.forEach(clearTimeout);
	}, []);
	const at = off ? MASKING_STEPS.length : step;
	return <div className={styles.journey}>
		<div className={styles.step}>
			<span className={styles.stepName}>On this computer</span>
			<div>
				<p className={styles.sentence}><b>Halvane Robotics</b> will pay <b>$48,000,000</b> for 80% of <b>Sablewood</b> at Closing.</p>
				<p className={styles.legend} data-hidden={off || at < 2 || undefined}>Masked on this computer before it leaves.</p>
			</div>
		</div>
		<div className={styles.step} data-waiting={at < 1 || undefined} data-away={!off && at >= 5 ? "" : undefined} data-exposed={off ? "" : undefined}>
			<span className={styles.stepName}>Sent out</span>
			<div className={styles.stack}>
				<div data-shown={off ? undefined : ""}>
					<p className={styles.sentence}><Swap word="Halvane Robotics" symbol="[[Org1]]" masked={at >= 2} /> will pay <Swap word="$48,000,000" symbol="[[Amount1]]" masked={at >= 3} /> for 80% of <Swap word="Sablewood" symbol="[[Org2]]" masked={at >= 4} /> at Closing.</p>
				</div>
				<div data-shown={off ? undefined : ""}>
					<p className={`${styles.sentence} ${styles.sizer}`}><mark>[[Org1]]</mark> will pay <mark>[[Amount1]]</mark> for 80% of <mark>[[Org2]]</mark> at Closing.</p>
					<p className={styles.legend} data-hidden={at < 5 || undefined}>[[Amount1]] is an amount in US dollars.</p>
				</div>
				<div data-shown={off ? "" : undefined}>
					<p className={styles.sentence}><b>Halvane Robotics</b> will pay <b>$48,000,000</b> for 80% of <b>Sablewood</b> at Closing.</p>
				</div>
			</div>
		</div>
		<div className={styles.step} data-waiting={at < 6 || undefined}>
			<span className={styles.stepName}>Back here</span>
			<div className={styles.stack}>
				<div data-shown="">
					<p className={styles.sentence}><Swap word="Halvane Robotics" symbol="[[Org1]]" masked={!off && at < 7} /> wires <Swap word="$48,000,000" symbol="[[Amount1]]" masked={!off && at < 8} /> to the sellers at Closing.</p>
				</div>
				<div data-shown="">
					<p className={`${styles.sentence} ${styles.sizer}`}><b>Halvane Robotics</b> wires <b>$48,000,000</b> to the sellers at Closing.</p>
					<p className={styles.legend} data-hidden={off || at < 8 || undefined}>Restored on this computer.</p>
				</div>
			</div>
		</div>
	</div>;
}

// A client's word and the symbol it leaves as, in one place: the place takes
// the width of the one shown and they cross-fade, so the sentence closes up
// around the change. Both widths are measured once the fonts are in.
function Swap({ word, symbol, masked }) {
	const wordRef = useRef(null);
	const symbolRef = useRef(null);
	const [widths, setWidths] = useState(null);
	useLayoutEffect(() => {
		let live = true;
		const measure = () => {
			if (!live || wordRef.current == null || symbolRef.current == null) return;
			setWidths({ word: Math.ceil(wordRef.current.getBoundingClientRect().width), symbol: Math.ceil(symbolRef.current.getBoundingClientRect().width) });
		};
		measure();
		document.fonts?.ready.then(measure, () => {});
		return () => { live = false; };
	}, []);
	return <span className={styles.swap} data-masked={masked || undefined} style={widths ? { width: `${masked ? widths.symbol : widths.word}px` } : undefined}>
		<b ref={wordRef}>{word}</b>
		<mark ref={symbolRef}>{symbol}</mark>
	</span>;
}

// An AI colleague's title is its role; the rank it holds is a level, never the
// title of Partner, which belongs to people (D-060).
function Colleague({ draft }) {
	const member = { id: "onboarding-colleague", kind: "ai_colleague", name: draft?.name.trim() || "Kitsune", species: draft?.species ?? "kitsune" };
	return <div className={styles.profile}>
		<header className={styles.identity}><Avatar member={member} size={80} /><div><h2>{member.name}</h2><p>{draft?.role.trim() || "Head of Commercial"}</p><span>{`${draft?.position || "Partner"} level`}</span></div></header>
		<Part name="In your firm"><div className={styles.about}><p>{draft?.area.trim() || "Commercial contracts, from the first discussion to the final agreement."}</p><span>An AI colleague, part of the team.</span></div></Part>
	</div>;
}

function Office({ app }) {
	const outlook = app === "outlook";
	const name = { word: "Word", excel: "Excel", powerpoint: "PowerPoint", outlook: "Outlook" }[app];
	return <div className={styles.picture}>
		<section className={styles.document}>
			<div className={styles.appName}><Icon name={outlook ? "mail" : "document"} size={16} /><span>{name}</span></div>
			{outlook ? <><h2>Documents for the transaction</h2><p className={styles.byline}>From Evelyn Hart</p><p>The employment pack and staff schedule are attached. I’ll speak to their HR lead tomorrow.</p><div className={styles.file}><FileKindIcon name="Staff schedule.xlsx" size="row" /><span>Staff schedule.xlsx</span></div></> : app === "excel" ? <><h2>DD workplan</h2><table className={styles.cells}><thead><tr><th>Workstream</th><th>Owner</th></tr></thead><tbody><tr><td>Legal DD</td><td>Kitsune</td></tr><tr><td>Employment</td><td>Evelyn</td></tr><tr><td>Contracts & IP</td><td>Phoenix</td></tr></tbody></table></> : app === "powerpoint" ? <div className={styles.slide}><h2>One transaction.<br />One team.</h2><span>Halvane and Sablewood, October 2026</span></div> : <><h2>Non-disclosure agreement</h2><p className={styles.clause}>2. Confidential information</p><p>Each party will use the confidential information solely to evaluate the proposed transaction.</p></>}
		</section>
		<section className={styles.pane}>
			<div className={styles.paneHead}><img src="app-icon.png" width="22" height="22" alt="" /><span>Singularity</span><span className={styles.matter}>Amsterdam</span></div>
			<Part name={outlook ? "Correspondence in the Matter" : "Working on this document"}>
				<Lines><Line lead={<Avatar member={{ id: "onboarding-kitsune", kind: "ai_colleague", name: "Kitsune", species: "kitsune" }} size={36} />} title="Kitsune" line={outlook ? "I’ve filed this to the employment DD workstream." : "I’m checking clause 2 against the NDA Playbook."} /></Lines>
			</Part>
		</section>
	</div>;
}

function AppsOverview() {
	return <div className={styles.profile}>
		<header className={styles.overviewHead}><h2>Amsterdam</h2><p>Its documents and mail, in the apps you use.</p></header>
		<Part name="One Matter">
			<Lines>
				<Line lead={<FileKindIcon name="NDA.docx" size="row" />} title="Non-disclosure agreement" line="Word" />
				<Line lead={<FileKindIcon name="DD workplan.xlsx" size="row" />} title="DD workplan" line="Excel" />
				<Line lead={<FileKindIcon name="Transaction briefing.pptx" size="row" />} title="Transaction briefing" line="PowerPoint" />
				<Line lead={<OutlookIcon />} title="Documents for the transaction" line="Outlook" />
			</Lines>
		</Part>
	</div>;
}
