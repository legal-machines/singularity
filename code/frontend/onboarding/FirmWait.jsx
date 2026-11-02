import { useEffect, useRef, useState } from "react";

import entry from "./entry/EntryForm.module.css";
import tour from "./tour/Tour.module.css";
import styles from "./FirmWait.module.css";

// A colleague's new computer waits for the firm here, at onboarding's end and
// before the app (the owner, 2026-11-02: "hold us at the very end of
// onboarding, saying we wait for the sync", never a sample person's
// profile). Its seat's profile, its colleagues and the firm's AI colleagues
// come only from the firm's own computers, sealed, once one of them has
// checked this one (D-205, D-209): so this page begins the check with the
// administrator's computer by itself, shows the eight digits both computers
// show, and opens the app once the seat's profile has come. Nothing opens
// before the firm has come (the owner, 2026-11-02: "'Open Singularity now'
// cannot be there before the sync").

const EVERY_MS = 2000;

export function FirmWait({ onDone }) {
	const api = window.nativeApp;
	// "looking" before the first reading (nothing shows), then "waiting" for
	// the administrator's computer, "checking" while the check is under way,
	// "ready" with the number, "failed", or "coming" once checked.
	const [phase, setPhase] = useState("looking");
	const [check, setCheck] = useState(null);
	const [isDeciding, setDeciding] = useState(false);
	const [problem, setProblem] = useState("");
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
				const [account, profiles, listed] = await Promise.all([
					api.getAccountState?.().catch(() => null),
					api.listSeatProfiles?.().catch(() => null),
					api.getFirmComputers?.().catch(() => null),
				]);
				const seat = (account?.seatId ?? "").trim();
				const all = profiles?.profiles ?? [];
				const own = all.some(profile => profile.seatId == seat && (profile.name ?? "").trim() != "");
				const computers = listed?.computers ?? [];
				const asking = listed?.asking ?? [];
				const isChecked = computers.some(computer => computer.state == "checked");
				// The seat's profile came: the app opens as the member. Checked, with
				// the firm's record here and no profile in it for this seat, nothing
				// more is coming: the app opens too.
				if (own == true || (isChecked == true && all.length > 0)) {
					await api.getFirmTeam?.().catch(() => null);
					if (live) finish();
					return;
				}
				if (live == false) return;
				if (isChecked == true) {
					setPhase("coming");
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

	if (phase == "looking") return null;

	const words = {
		waiting: ["Waiting for your firm", "Your administrator’s computer checks this one once. Then your profile, your colleagues and the firm’s AI colleagues come here, sealed.",
			"Your administrator’s computer is not open now. When it is, this page goes on by itself."],
		checking: ["Checking with your administrator", "Your administrator is asked to check this computer. The same number then shows here and on their screen.", null],
		ready: ["Compare the number", "Your administrator sees the same eight digits only when nothing stands between your two computers.", null],
		failed: ["The check stopped", check?.problem || "The check could not finish.", null],
		coming: ["Your firm is on its way", "Your profile, your colleagues and the firm’s AI colleagues come here in a moment.", null],
	}[phase];

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
					) : null}
					{problem != "" ? <p className={tour.error}>{problem}</p> : null}
				</div>
			</div>
		</div>
	);
}
