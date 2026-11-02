import { useCallback, useEffect, useState } from "react";

import { describeFile, isConnected } from "./data.js";
import { ExcelPane } from "./ExcelPane.jsx";
import { fileLocation } from "./host.js";
import { OutlookPane } from "./OutlookPane.jsx";
import { endingWords, pairByItself } from "./pairing.js";
import { PowerPointPane } from "./PowerPointPane.jsx";
import { WordPane } from "./WordPane.jsx";
import { appName } from "./words.js";

import styles from "./pane.module.css";

// The pane in Word, Excel or Outlook (D-105). It is Singularity's only when
// the person has paired it in Singularity's window; then it finds the Matter
// of the file Office has open and shows it. `start` says whether Office
// started the pane: "no_office" when Office's script did not come from
// Microsoft, "late" when Office did not start it in time.
export function OfficePane({ host, start = "ready" }) {
	const [isPaired, setPaired] = useState(isConnected());

	useEffect(() => {
		const unpaired = () => setPaired(false);
		window.addEventListener("lm-office-unpaired", unpaired);
		return () => window.removeEventListener("lm-office-unpaired", unpaired);
	}, []);

	const application = host == "none" ? "Office" : appName(host);
	if (start == "no_office") {
		return <Retry words={`This pane needs the internet to open: ${application} loads part of it from Microsoft. Check this computer's internet connection, then try again.`} />;
	}
	if (start == "late") return <Retry words={`${application} did not start Singularity's pane. Try again.`} />;
	if (host == "none") {
		return <p className={styles.quiet}>This page is Singularity's pane in Word, Excel, PowerPoint and Outlook. Open it from there.</p>;
	}
	if (!isPaired) return <Pairing host={host} onPaired={() => setPaired(true)} />;
	if (host == "outlook") return <OutlookPane />;
	return <FilePane host={host} />;
}

// What stopped the pane from starting, and the one way on: the pane loaded again.
function Retry({ words }) {
	return (
		<div className={styles.pairing}>
			<img className={styles.pairing_icon} src="assets/singularity-64.png" width="40" height="40" alt="" />
			<p className={styles.problem}>{words}</p>
			<button className={styles.primary} type="button" onClick={() => window.location.reload()}>Try again</button>
		</div>
	);
}

// Pairing: the pane asks by itself as it opens (D-191), and the person allows
// it in Singularity's window, after checking that both show the same code.
function Pairing({ host, onPaired }) {
	const [stage, setStage] = useState({ kind: "asking" });
	// Each round of asking: a press of the button after asking ended begins the next.
	const [round, setRound] = useState(0);

	useEffect(() => {
		let isStopped = false;
		const transport = window.__lmOffice;
		const asking = transport == null
			? Promise.resolve("unreachable")
			: pairByItself({
				transport,
				client: host,
				label: `${appName(host)} on this computer`,
				onStage: next => { if (!isStopped) setStage(next); },
				isStopped: () => isStopped,
			});
		asking.then(ending => {
			if (isStopped) return;
			if (ending == "allowed") onPaired();
			else setStage({ kind: "ended", ending });
		});
		return () => { isStopped = true; };
		// Asked again only in a new round, never as the page draws again.
	}, [host, round]);

	const ended = stage.kind == "ended" ? endingWords(stage.ending, appName(host)) : null;
	return (
		<div className={styles.pairing}>
			<img className={styles.pairing_icon} src="assets/singularity-64.png" width="40" height="40" alt="" />
			<p className={styles.pairing_title}>Connect {appName(host)} to Singularity</p>
			<p className={styles.pairing_text}>
				Then this pane shows the Matter of the file you are in, and you work with its team from here. Singularity asks you to allow it first, so no other program can.
			</p>
			{stage.kind == "waiting" ? (
				<>
					<p className={styles.pairing_text}>Allow it in Singularity. It shows the same code:</p>
					<p className={styles.code}>{stage.code.slice(0, 3)} {stage.code.slice(3)}</p>
				</>
			) : null}
			{stage.kind == "asking" ? <p className={styles.pairing_text} role="status">Asking Singularity…</p> : null}
			{stage.kind == "busy" ? <p className={styles.pairing_text} role="status">Singularity is answering another pane first. This one asks next.</p> : null}
			{stage.kind == "unreachable" ? (
				<p className={styles.pairing_text} role="status">Singularity on this computer does not answer. Open Singularity, and this pane asks by itself.</p>
			) : null}
			{ended != null ? (
				<>
					<p className={styles.problem}>{ended.words}</p>
					<button className={styles.primary} type="button" onClick={() => { setStage({ kind: "asking" }); setRound(value => value + 1); }}>{ended.action}</button>
				</>
			) : null}
		</div>
	);
}

// The file Office has open, and its Matter.
function FilePane({ host }) {
	const [described, setDescribed] = useState(null);
	const [problem, setProblem] = useState(null);

	const read = useCallback(async () => {
		try {
			setDescribed(await describeFile(fileLocation()));
			setProblem(null);
		} catch (error) {
			setProblem(error?.userMessage || error?.message || "Singularity did not answer.");
		}
	}, []);

	useEffect(() => {
		read();
		// A document saved into a Matter's folder becomes its document.
		const off = window.nativeApp?.on?.("matter.changed", () => read());
		return () => { if (typeof off == "function") off(); };
	}, [read]);

	if (problem != null) return <p className={styles.problem}>{problem}</p>;
	if (described == null) return <p className={styles.quiet}>Opening…</p>;
	const kind = host == "excel" ? "workbook" : host == "powerpoint" ? "presentation" : "document";
	// The file Singularity opened the application with, to show this pane (D-191).
	if (described.kind == "starter") {
		return <p className={styles.quiet}>{appName(host)} is connected to Singularity. Open a {kind} from one of your Matters, and its Matter opens here.</p>;
	}
	if (described.kind == "unsaved") {
		return <p className={styles.quiet}>Save this {kind} in a Matter's folder, and its Matter opens here.</p>;
	}
	if (described.kind == "online") {
		return <p className={styles.quiet}>This file is open from the web. Singularity works with the files in your Matters' folders on this computer.</p>;
	}
	if (described.kind == "outside" || described.place == null) {
		return <p className={styles.quiet}>{described.name || "This file"} is not in a Matter. Save it in a Matter's folder, and its Matter opens here.</p>;
	}
	if (described.place.documentId == "") return <p className={styles.quiet}>Singularity is reading {described.name}…</p>;
	if (host == "word") return <WordPane place={described.place} reveal={described.reveal ?? null} />;
	if (host == "powerpoint") return <PowerPointPane place={described.place} reveal={described.reveal ?? null} />;
	return <ExcelPane place={described.place} reveal={described.reveal ?? null} />;
}
