import { useEffect, useRef, useState } from "react";

import entry from "./entry/EntryForm.module.css";
import tour from "./tour/Tour.module.css";
import styles from "./FirmWait.module.css";

// A colleague's new computer waits for the firm here, at onboarding's end and
// before the app. Its seat's profile, its colleagues and the firm's AI
// colleagues come only from the firm's own computers, sealed, once one of
// them has checked this one: so this page begins the check with the
// administrator's computer by itself, shows the eight digits both computers
// show, and opens the app once the seat's profile has come. Nothing opens
// before the firm has come.
//
// Once checked, the page says where each step stands, as it stands: the
// check, the computers' encrypted connection, the firm's record and what of
// it came. A Matter a colleague shared waits here to be added, and a firm
// whose record came without this seat's profile lets the member go on.

const EVERY_MS = 2000;
// How long the computers may take to connect before this page says what to
// keep open.
const SLOW_MS = 45000;
// How long a record that came may take to bring this seat's profile before
// the member is offered to go on without it.
const PROFILE_GRACE_MS = 10000;

// How far the firm has come to this computer, from what the native side says
// (FirmWait reads it every step): the computers a lawyer checked, whether one
// of them holds a live encrypted session with this one, whether the firm's
// record was heard from them, and what of it came.
export function firmProgress({ computers = [], profiles = [], status = null, team = null, seatId = "" } = {}) {
	const checked = computers.filter(computer => computer.state == "checked");
	const admin = checked.find(computer => computer.role == "administrator") ?? checked[0] ?? null;
	// The firm's record channel as coordination.status says it: null while
	// this computer knows none of the firm's other computers. Its "heard" is
	// filled only by what opened under the record's key from another of the
	// firm's computers; its state turns "carried" on this computer before
	// anything came, so it is never read as the record having come.
	const firm = status?.firm ?? null;
	const connected = new Set([...(firm?.connected ?? []), ...(status?.matters ?? []).flatMap(matter => matter.connected ?? [])]);
	const named = profiles.filter(profile => (profile.name ?? "").trim() != "" && (profile.state ?? "") == "");
	const isHeard = (firm?.heard ?? []).length > 0 || named.length > 0;
	const receiving = (firm?.receiving ?? []).filter(transfer => checked.some(computer => computer.deviceId == transfer.deviceId));
	return {
		isChecked: checked.length > 0,
		// What came over it proves the connection, as a live session does.
		isConnected: checked.some(computer => connected.has(computer.deviceId)) || isHeard,
		isHeard,
		files: receiving.reduce((sum, transfer) => sum + (transfer.files ?? 0), 0),
		filesHere: receiving.reduce((sum, transfer) => sum + (transfer.filesHere ?? 0), 0),
		hasOwnProfile: named.some(profile => profile.seatId == seatId),
		colleagues: named.filter(profile => profile.seatId != seatId).length,
		aiColleagues: (team?.agents ?? []).length,
		adminName: named.find(profile => profile.seatId == admin?.seatId)?.name.trim() ?? null,
	};
}

const counted = (count, one, many) => `${count} ${count == 1 ? one : many}`;

// "Your profile, 2 colleagues and 1 AI colleague."
function cameWords(progress) {
	const parts = [
		progress.hasOwnProfile ? "Your profile" : null,
		progress.colleagues > 0 ? counted(progress.colleagues, "colleague", "colleagues") : null,
		progress.aiColleagues > 0 ? counted(progress.aiColleagues, "AI colleague", "AI colleagues") : null,
	].filter(Boolean);
	if (parts.length == 0) return "The firm’s record. It holds no profile yet.";
	const last = parts.pop();
	return `${parts.length > 0 ? `${parts.join(", ")} and ${last}` : last}.`;
}

// Each step as it stands now: its name, its state in a word, and one line.
export function stepsOf(progress, { isSlow = false } = {}) {
	const who = progress.adminName != null ? `${progress.adminName}’s computer` : "your administrator’s computer";
	const Who = who.charAt(0).toUpperCase() + who.slice(1);
	return [
		{ key: "checked", name: "Check", state: "done", word: "Done", line: `This computer and ${who} showed the same number.` },
		progress.isConnected
			? { key: "link", name: "Connection", state: "done", word: "Connected", line: "End‑to‑end encrypted. Only your firm’s computers can read what comes." }
			: isSlow
				? { key: "link", name: "Connection", state: "problem", word: "Not connected yet", line: `${Who} hasn’t answered yet. Keep Singularity open and online on both computers. This page goes on by itself.` }
				: { key: "link", name: "Connection", state: "busy", word: "Connecting…", line: `Looking for ${who} on the office network and the internet.` },
		progress.isHeard
			? { key: "firm", name: "Your firm", state: progress.hasOwnProfile ? "done" : "problem", word: progress.hasOwnProfile ? "Received" : "No profile yet", line: progress.hasOwnProfile ? cameWords(progress) : "Your administrator hasn’t filled in your profile. You can go on: it comes here when they do." }
			: progress.files > 0
				? { key: "firm", name: "Your firm", state: "busy", word: "Receiving…", line: `Your profile, your colleagues and the firm’s AI colleagues: ${progress.filesHere} of ${counted(progress.files, "file", "files")} here.` }
				: { key: "firm", name: "Your firm", state: "waiting", word: "Waiting", line: "Your profile, your colleagues and the firm’s AI colleagues come once your computers connect." },
	];
}

const STATE_OF_ROW = { done: "allowed", problem: "denied", busy: "asking", waiting: "unknown" };

export function FirmWait({ onDone }) {
	const api = window.nativeApp;
	// "looking" before the first reading (nothing shows), then "waiting" for
	// the administrator's computer, "checking" while the check is under way,
	// "ready" with the number, "failed", or "coming" once checked.
	const [phase, setPhase] = useState("looking");
	const [check, setCheck] = useState(null);
	const [isDeciding, setDeciding] = useState(false);
	const [problem, setProblem] = useState("");
	const [isSlow, setSlow] = useState(false);
	const [progress, setProgress] = useState(() => firmProgress());
	const [firmName, setFirmName] = useState("");
	const [offers, setOffers] = useState([]);
	const [names, setNames] = useState({});
	const [adding, setAdding] = useState({});
	// When the firm's record came without this seat's profile: after a grace,
	// the member may go on.
	const [heardAt, setHeardAt] = useState(null);
	const [mayGoOn, setMayGoOn] = useState(false);
	const begun = useRef(null);
	const isDone = useRef(false);
	const heading = useRef(null);

	const finish = () => {
		if (isDone.current) return;
		isDone.current = true;
		onDone?.();
	};

	useEffect(() => {
		let live = true;
		let timer = null;
		const step = async () => {
			try {
				const [account, profiles, listed, status, waiting] = await Promise.all([
					api.getAccountState?.().catch(() => null),
					api.listSeatProfiles?.().catch(() => null),
					api.getFirmComputers?.().catch(() => null),
					api.getCoordination?.().catch(() => null),
					api.getMatterOffers?.().catch(() => null),
				]);
				const seat = (account?.seatId ?? "").trim();
				const all = profiles?.profiles ?? [];
				const computers = listed?.computers ?? [];
				const asking = listed?.asking ?? [];
				const team = await api.getFirmTeam?.().catch(() => null);
				if (live == false) return;
				const now = firmProgress({ computers, profiles: all, status, team, seatId: seat });
				setProgress(now);
				setFirmName((account?.organisationName ?? "").trim());
				// A Matter a colleague shared comes by itself.
				for (const offer of waiting?.offers ?? []) add(offer);
				setOffers(previous => [...previous.filter(kept => !(waiting?.offers ?? []).some(offer => offer.matterId == kept.matterId && offer.fromDeviceId == kept.fromDeviceId)), ...(waiting?.offers ?? [])]);
				setNames(Object.fromEntries(all.map(profile => [`seat:${profile.seatId}`, (profile.name ?? "").trim()])));
				// The seat's profile came: the app opens as the member.
				if (now.isChecked && now.hasOwnProfile) {
					await api.getFirmTeam?.().catch(() => null);
					if (live) finish();
					return;
				}
				if (now.isChecked == true) {
					setPhase("coming");
					if (now.isHeard) setHeardAt(previous => previous ?? Date.now());
				} else {
					// The computer that asks first; else the administrator's; else any
					// of the firm's not checked yet.
					const target = computers.find(computer => computer.state == "new" && asking.includes(computer.deviceId))
						?? computers.find(computer => computer.state == "new" && computer.role == "administrator")
						?? computers.find(computer => computer.state == "new")
						?? null;
					if (target == null) {
						setPhase("waiting");
					} else {
						const next = begun.current == target.deviceId
							? await api.getComputerCheck(target.deviceId)
							: await api.checkFirmComputer(target.deviceId);
						begun.current = target.deviceId;
						if (live == false) return;
						setCheck(next);
						setPhase(next?.state == "ready" ? "ready" : next?.state == "failed" ? "failed" : "checking");
					}
				}
			} catch {
				// Read again at the next step.
			}
			if (live) timer = setTimeout(step, EVERY_MS);
		};
		step();
		return () => {
			live = false;
			clearTimeout(timer);
		};
	}, []);

	useEffect(() => {
		if (phase != "looking") heading.current?.focus({ preventScroll: true });
	}, [phase]);

	// Checked, the firm comes over the computers' own link, or through the
	// relay where a network lets no direct way through: computers that take
	// long to connect are told so, with what keeps it going.
	useEffect(() => {
		if (phase != "coming" || progress.isConnected) {
			setSlow(false);
			return undefined;
		}
		const timer = setTimeout(() => setSlow(true), SLOW_MS);
		return () => clearTimeout(timer);
	}, [phase, progress.isConnected]);

	useEffect(() => {
		if (heardAt == null || progress.hasOwnProfile) return undefined;
		const timer = setTimeout(() => setMayGoOn(true), Math.max(0, heardAt + PROFILE_GRACE_MS - Date.now()));
		return () => clearTimeout(timer);
	}, [heardAt, progress.hasOwnProfile]);

	async function decide(isSame) {
		if (check == null || isDeciding) return;
		setDeciding(true);
		setProblem("");
		try {
			if (isSame) {
				await api.keepFirmComputer(check.deviceId);
				setPhase("coming");
			} else {
				await api.refuseFirmComputer(check.deviceId);
				begun.current = null;
				setCheck(null);
				setPhase("waiting");
				setProblem("That computer stays out of the firm here. Ask your administrator to check this computer with you again.");
			}
		} catch (error) {
			setProblem(error?.message ?? "It could not be decided. Try again.");
		}
		setDeciding(false);
	}

	function again() {
		begun.current = null;
		setCheck(null);
		setProblem("");
		setPhase("checking");
	}

	// Each shared Matter is taken once; one that could not be is tried again at
	// the next reading.
	const taking = useRef(new Set());
	async function add(offer) {
		const key = `${offer.matterId}/${offer.fromDeviceId}`;
		if (taking.current.has(key)) return;
		taking.current.add(key);
		setAdding(previous => ({ ...previous, [key]: "adding" }));
		try {
			await api.acceptMatterOffer(offer.matterId, offer.fromDeviceId);
			setAdding(previous => ({ ...previous, [key]: "added" }));
		} catch {
			taking.current.delete(key);
			setAdding(previous => ({ ...previous, [key]: undefined }));
		}
	}

	if (phase == "looking") return null;

	const words = {
		waiting: ["Waiting for your firm", "Your administrator’s computer checks this one once. Then your profile, your colleagues and the firm’s AI colleagues come here, sealed.",
			"Your administrator’s computer is not open now. When it is, this page goes on by itself."],
		checking: ["Checking with your administrator", "Your administrator is asked to check this computer. The same number then shows here and on their screen.", null],
		ready: ["Compare the number", "Your administrator sees the same eight digits only when nothing stands between your two computers.", null],
		failed: ["The check stopped", check?.problem || "The check could not finish.", null],
		coming: [firmName != "" ? `Joining ${firmName}` : "Joining your firm", "Your firm comes to this computer from your colleagues’ computers, end‑to‑end encrypted.", null],
	}[phase];
	const steps = phase == "coming" ? stepsOf(progress, { isSlow }) : [];
	// A Matter added here is no longer an offer once the native side took it.
	const shown = offers.map(offer => ({ ...offer, key: `${offer.matterId}/${offer.fromDeviceId}` }));

	return (
		<div className={`${entry.entry} ${tour.tour}`}>
			<div className={tour.group} data-ready="">
				<header className={tour.header}>
					<h1 ref={heading} id="onboarding-title" tabIndex={-1} className={tour.title}>{words[0]}</h1>
					<p className={tour.line}>{words[1]}</p>
					{words[2] ? <p className={tour.note}>{words[2]}</p> : null}
				</header>
				<div className={tour.body}>
					{phase == "ready" ? (
						<div className={styles.check}>
							<p className={styles.number} aria-label={(check?.number ?? "").split("").join(" ")}>{check?.number}</p>
							<p className={styles.question}>Does your administrator see the same number?</p>
							<button className={entry.primary} type="button" disabled={isDeciding} data-busy={isDeciding || undefined} onClick={() => decide(true)}>
								<span className={entry.buttonLabel}>They match</span>
							</button>
							<button className={tour.later} type="button" disabled={isDeciding} onClick={() => decide(false)}>They don’t match</button>
						</div>
					) : phase == "checking" ? (
						<div className={styles.check} aria-hidden="true">
							<p className={styles.number} data-waiting="">•••• ••••</p>
						</div>
					) : phase == "failed" ? (
						<button className={entry.primary} type="button" onClick={again}><span className={entry.buttonLabel}>Try again</span></button>
					) : phase == "coming" ? (
						<>
							<ul className={tour.features} aria-live="polite">
								{steps.map(row => (
									<li key={row.key} data-step={row.key} data-state={row.state}>
										<span className={tour.featureName}>{row.name}<span className={tour.permissionState} data-state={STATE_OF_ROW[row.state]}>{row.word}</span></span>
										<span className={tour.featureLine}>{row.line}</span>
									</li>
								))}
							</ul>
							{shown.length > 0 ? (
								<ul className={`${tour.features} ${styles.offers}`} aria-label="Matters shared with you">
									{shown.map(offer => {
										const state = adding[offer.key];
										const from = names[offer.fromActorId] || "your administrator";
										return (
											<li key={offer.key} data-offer={offer.matterId} data-state={state == "added" ? "done" : "busy"}>
												<span className={tour.featureName}>{offer.name}<span className={tour.permissionState} data-state={state == "added" ? "allowed" : "asking"}>{state == "added" ? "Added" : "Coming…"}</span></span>
												<span className={tour.featureLine}>{`Shared with you by ${from}, ${counted(offer.files, "file", "files")}.`}</span>
											</li>
										);
									})}
								</ul>
							) : null}
							{mayGoOn && progress.isHeard && !progress.hasOwnProfile ? (
								<button className={entry.primary} type="button" onClick={finish}><span className={entry.buttonLabel}>Open Singularity</span></button>
							) : null}
						</>
					) : null}
					{problem != "" ? <p className={tour.error}>{problem}</p> : null}
				</div>
			</div>
		</div>
	);
}
