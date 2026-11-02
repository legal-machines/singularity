import { useEffect, useRef, useState } from "react";
import { createPortal } from "react-dom";

import { Avatar } from "../../components/Avatar/Avatar.jsx";
import { characters } from "../../components/Avatar/characters.js";
import { hasNative, shownMembers } from "../../data/native.js";
import { normalizePractice } from "../../data/practices.js";
import { BackButton } from "../../onboarding/BackButton.jsx";
import { Dots } from "../../onboarding/Dots.jsx";
import entry from "../../onboarding/entry/EntryForm.module.css";
import onboarding from "../../onboarding/Onboarding.module.css";
import { POSITIONS } from "../../onboarding/tour/pages.js";
import faces from "../../onboarding/tour/SetupDesign.module.css";
import preview from "../../onboarding/tour/SetupPreview.module.css";
import tour from "../../onboarding/tour/Tour.module.css";

import styles from "./HirePage.module.css";

// Hiring an AI colleague (D-071), as onboarding hires the first (the owner,
// 2026-11-02: "rebuild creating an AI agent in onboarding's style, filling
// the window"): the whole window, its words on white on the left and the
// colleague taking shape on the right, one step a slide, Back and the steps'
// dots at its foot. It asks what a colleague's profile holds (D-219): its
// face and name; its role on the firm's scale, its own words for its
// position and its practice; its area, what it does and how it works. It
// joins the whole firm, never one Matter (D-157), and is set over no one's
// shoulder: no member it "answers to" (the owner, 2026-11-02: "writing that
// there is Luddism"; PHIL-16).
const STEPS = [
	{ title: "Who they are.", line: "Start with a name and a face. They join the whole firm, not one Matter." },
	{ title: "Their place in the firm.", line: "The role they hold on the firm's scale, their position and their practice." },
	{ title: "An area to lead.", line: "The work they own, what they do without being asked and how they work with everyone." },
];
const FAMILY = Object.keys(characters);

export function HirePage({ onHire, onCancel }) {
	const [step, setStep] = useState(0);
	const [isHiring, setHiring] = useState(false);
	const [refusal, setRefusal] = useState("");
	const [ready, setReady] = useState(false);
	const heading = useRef(null);
	const group = useRef(null);
	const [draft, setDraft] = useState(() => ({
		name: "",
		// A face no colleague of the firm has yet, where one is left.
		species: FAMILY.find(name => shownMembers().every(member => member.species != name)) ?? FAMILY[0],
		ranksWith: "",
		title: "",
		practice: "",
		area: "",
		goal: "",
		manner: "",
	}));
	const change = patch => {
		setDraft(previous => ({ ...previous, ...patch }));
		setRefusal("");
	};
	const name = draft.name.trim();
	const isValid = [name != "", draft.ranksWith != "", draft.area.trim() != ""][step];
	const isLast = step == STEPS.length - 1;

	// The slide's words appear once onboarding's type has loaded, and each new
	// step comes in as one composition, its title first to be read.
	useEffect(() => {
		let live = true;
		Promise.all([document.fonts?.load('500 40px "Sora Entry"'), document.fonts?.ready]).then(() => live && setReady(true), () => live && setReady(true));
		return () => {
			live = false;
		};
	}, []);
	useEffect(() => {
		if (!ready) return undefined;
		heading.current?.focus({ preventScroll: true });
		group.current.scrollTop = 0;
		const reduced = window.matchMedia?.("(prefers-reduced-motion: reduce)").matches;
		const motion = group.current.animate([{ opacity: 0 }, { opacity: 1 }], { duration: reduced ? 0 : 220, easing: "ease-out" });
		return () => motion.cancel();
	}, [ready, step]);
	// Escape leaves the hiring, as Back does from its first step.
	useEffect(() => {
		const leave = event => {
			if (event.key == "Escape" && !isHiring) onCancel();
		};
		window.addEventListener("keydown", leave);
		return () => window.removeEventListener("keydown", leave);
	}, [isHiring, onCancel]);

	async function submit(event) {
		event.preventDefault();
		if (!isValid || isHiring) return;
		if (!isLast) {
			setStep(step + 1);
			return;
		}
		setHiring(true);
		try {
			await hire();
		} catch (reason) {
			setRefusal(reason?.detail ?? reason?.message ?? `${name} couldn’t be hired. Try again.`);
			setHiring(false);
		}
	}

	function hire() {
		const slug = name.toLowerCase().replace(/[^a-z0-9]+/g, "-").replace(/^-|-$/g, "") || "colleague";
		return onHire(
			{
				id: `m_${slug}_${Date.now().toString(36)}`,
				kind: "ai_colleague",
				name,
				species: draft.species,
				ranksWith: draft.ranksWith,
				// Its own words for its position; the role on the scale where it gave none.
				role: draft.title.trim() || draft.ranksWith,
				title: draft.title.trim(),
				practice: normalizePractice(draft.practice),
				area: sentence(draft.area),
				goal: draft.goal.trim(),
				manner: draft.manner.trim().slice(0, 400),
				duties: [],
				remit: [],
				record: "No results yet.",
			},
			{
				joined: new Intl.DateTimeFormat("en-US", { month: "long", year: "numeric" }).format(new Date()),
				probation: "Under way: its work in its area is reviewed in full until its record speaks",
				reads: "The firm's Matters, as each Matter's own rules allow any member",
				follows: [],
				can: [],
				runs: hasNative() ? "On this computer, while Singularity is open" : "Always on, on the firm's server",
				attention: "No results yet",
				spend: "$0.00 this month",
			},
			[],
		);
	}

	const back = () => (step == 0 ? onCancel() : setStep(step - 1));

	// Over the whole window, the sidebar too, as onboarding fills it.
	return createPortal(
		<main className={`${onboarding.page} ${styles.flow}`} data-tour="" aria-label="Hire an AI colleague">
			<section className={onboarding.entry} aria-labelledby="hire-title">
				<div className={`${entry.entry} ${tour.tour}`}>
					<BackButton className={tour.back} disabled={isHiring} onClick={back} />
					<div ref={group} className={tour.group} data-ready={ready || undefined} data-mode="setup">
						<header className={tour.header}>
							<h1 ref={heading} id="hire-title" tabIndex={-1} className={tour.title}>{STEPS[step].title}</h1>
							<p className={tour.line}>{STEPS[step].line}</p>
						</header>
						<div className={tour.body}>
							<form className={tour.ask} onSubmit={submit} aria-label={STEPS[step].title}>
								{step == 0 ? <WhoTheyAre draft={draft} change={change} /> : null}
								{step == 1 ? <TheirPlace draft={draft} change={change} /> : null}
								{step == 2 ? <TheirArea draft={draft} change={change} /> : null}
								{isLast ? <p className={tour.error} role="status">{refusal}</p> : null}
								<button className={entry.primary} type="submit" disabled={!isValid || isHiring}>
									<span className={entry.buttonLabel}>{isLast ? (isHiring ? "Hiring…" : `Hire ${name}`) : "Continue"}</span>
								</button>
							</form>
						</div>
					</div>
					<Dots className={tour.progress} steps={STEPS.map(item => item.title)} current={step} label="Hiring steps" canGo={at => !isHiring && at < step} onGo={setStep} />
				</div>
			</section>
			<div className={onboarding.art}>
				<Colleague draft={draft} />
			</div>
		</main>,
		document.body,
	);
}

function WhoTheyAre({ draft, change }) {
	return (
		<>
			<fieldset className={faces.family}>
				<legend className={faces.label}>Choose a face</legend>
				<div className={faces.faces}>
					{FAMILY.map(species => {
						const held = shownMembers().some(member => member.species == species);
						const label = `${species[0].toUpperCase()}${species.slice(1)}${held ? ", in the firm" : ""}`;
						return (
							<label key={species} className={faces.face} data-selected={draft.species == species || undefined} data-tooltip={label}>
								<input type="radio" name="hire-face" aria-label={label} checked={draft.species == species} onChange={() => change({ species })} />
								<Avatar member={{ id: `hire-${species}`, kind: "ai_colleague", name: species, species }} size={40} />
							</label>
						);
					})}
				</div>
			</fieldset>
			<div className={entry.field}>
				<label htmlFor="hire-name">Name</label>
				<input id="hire-name" autoComplete="off" maxLength={80} autoFocus placeholder={`For example, ${draft.species[0].toUpperCase()}${draft.species.slice(1)}`} value={draft.name} onChange={event => change({ name: event.target.value })} />
			</div>
			<p className={tour.help}>The name everyone in the firm will know.</p>
		</>
	);
}

function TheirPlace({ draft, change }) {
	return (
		<div className={entry.fields}>
			<div className={entry.field}>
				<label htmlFor="hire-role">Role</label>
				<select id="hire-role" className={tour.select} value={draft.ranksWith} autoFocus onChange={event => change({ ranksWith: event.target.value })}>
					<option value="" disabled>Choose a role</option>
					{POSITIONS.map(position => <option key={position}>{position}</option>)}
				</select>
			</div>
			<div className={entry.field}>
				<label htmlFor="hire-position">Position <span className={tour.optional}>(optional)</span></label>
				<input id="hire-position" autoComplete="off" maxLength={80} placeholder="For example, Head of Commercial" value={draft.title} onChange={event => change({ title: event.target.value })} />
			</div>
			<div className={entry.field}>
				<label htmlFor="hire-practice">Practice <span className={tour.optional}>(optional)</span></label>
				<input id="hire-practice" autoComplete="off" maxLength={80} placeholder="For example, Mergers & Acquisitions" value={draft.practice} onChange={event => change({ practice: event.target.value })} onBlur={() => change({ practice: normalizePractice(draft.practice) })} />
			</div>
		</div>
	);
}

function TheirArea({ draft, change }) {
	return (
		<div className={entry.fields}>
			<div className={entry.field}>
				<label htmlFor="hire-area">Area</label>
				<input id="hire-area" autoComplete="off" maxLength={240} autoFocus placeholder="For example, Commercial contracts" value={draft.area} onChange={event => change({ area: event.target.value })} />
			</div>
			<div className={`${entry.field} ${styles.field}`}>
				<label htmlFor="hire-goal">What they do <span className={tour.optional}>(optional)</span></label>
				<textarea id="hire-goal" rows={3} maxLength={2000} placeholder="For example, Reads every incoming contract against the Playbook and marks it up." value={draft.goal} onChange={event => change({ goal: event.target.value })} />
			</div>
			<div className={`${entry.field} ${styles.field}`}>
				<label htmlFor="hire-manner">How they work <span className={tour.optional}>(optional)</span></label>
				<textarea id="hire-manner" rows={3} maxLength={400} placeholder="For example, Brief and exact; asks before anything leaves the firm." value={draft.manner} onChange={event => change({ manner: event.target.value })} />
			</div>
		</div>
	);
}

// The colleague as the firm will meet it, taking shape as the slides are
// filled: its face, name and position, then its area once it is written -
// never service words, nor what it does and how it works (the owner,
// 2026-11-02).
function Colleague({ draft }) {
	const name = draft.name.trim() || "Your colleague";
	const member = { id: "hire-colleague", kind: "ai_colleague", name, species: draft.species };
	const position = draft.title.trim() || draft.ranksWith || "An AI colleague";
	const role = draft.title.trim() != "" && draft.ranksWith != "" ? draft.ranksWith : "";
	const practice = normalizePractice(draft.practice);
	return (
		<aside className={preview.stage} aria-hidden="true">
			<div className={preview.profile}>
				<header className={preview.identity}>
					<Avatar member={member} size={80} />
					<div>
						<h2>{name}</h2>
						<p>{position}</p>
						{role != "" || practice != "" ? <span>{[role, practice].filter(Boolean).join(" in ")}</span> : null}
					</div>
				</header>
				{draft.area.trim() != "" ? <div className={styles.about}><p>{draft.area.trim()}</p></div> : null}
			</div>
		</aside>
	);
}

function sentence(text) {
	const trimmed = text.trim();
	return /[.!?]$/.test(trimmed) ? trimmed : `${trimmed}.`;
}
