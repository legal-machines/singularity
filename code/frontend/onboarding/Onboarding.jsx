import { useEffect, useRef, useState } from "react";

import { WindowControls } from "../components/WindowControls/WindowControls.jsx";
import { Painting } from "./painting/Painting.jsx";
import { EntryForm } from "./entry/EntryForm.jsx";
import { FirmWait } from "./FirmWait.jsx";
import { Lens } from "./tour/Lens.jsx";
import { PAGES, pagesFor, permissionKeys, platformOf } from "./tour/pages.js";
import { Tour } from "./tour/Tour.jsx";
import { SetupPreview } from "./tour/SetupPreview.jsx";
import { Welcome } from "./Welcome.jsx";
import styles from "./Onboarding.module.css";

// How long the page takes to dissolve once onboarding completes.
const LEAVE_MS = 180;
// The app's emulation starts loading behind the painting once the first page
// has settled, so it is ready by the time sign-in is done.
const WARM_AFTER_MS = 2500;
// The painting stays until the app has fully taken its place, then lets go
// of its GPU work.
const PAINTING_LEAVES_MS = 700;
// A development build opens on a page after sign-in with
// `?onboarding=<page id>`, so that page can be checked in the real app
// without an account; a release build has no such door.
const PREVIEW = import.meta.env.DEV ? new URLSearchParams(window.location.search).get("onboarding") : null;
const START = PAGES.findIndex((page) => page.id === PREVIEW || ["office", "outlook"].includes(PREVIEW) && page.id === "apps");
// Browser-only design previews: no account is created or marked as signed in.
const WELCOME_PREVIEW = import.meta.env.DEV && !window.nativeApp && ["welcome", "welcome-sign-in"].includes(PREVIEW);

// Onboarding (D-125, D-133, D-134). Its first page signs in, on white opposite
// the living painting. Then each page shows the app at work on the right, in
// the app's own emulation, and asks on the left for what the app needs here.
// When the last completes, the page dissolves, then onComplete hands the
// window to the app, so the window's move to its main size is never seen
// half-drawn. A colleague's new computer waits for the firm first, on a page
// of its own (FirmWait, D-205): at onboarding's end, and with `hold` before
// the app when it opens still new to the firm.
export function Onboarding({ onComplete, hold = false }) {
	const [page, setPage] = useState(START >= 0 ? START : null);
	// Signed in already - the app restarted after sign-in, before the slides
	// were done - onboarding goes on from the first slide, never asks for the
	// password and a new code again.
	const [resumed, setResumed] = useState(hold || START >= 0 || typeof window.nativeApp?.getAccountState != "function");
	const [holding, setHolding] = useState(hold);
	const [configuring, setConfiguring] = useState(false);
	const [drafts, setDrafts] = useState({});
	const [warm, setWarm] = useState(START >= 0);
	const [lensReady, setLensReady] = useState(false);
	const [painting, setPainting] = useState(START < 0 && !WELCOME_PREVIEW);
	const [leaving, setLeaving] = useState(false);
	const [joined, setJoined] = useState(false);
	// What the computer does not allow Singularity yet, by key: the slide that
	// asks names only those, and is left out when there are none (D-249). Read
	// once, before the slides.
	const [missingPermissions, setMissingPermissions] = useState(null);
	const pages = pagesFor({ isMember: joined, missingPermissions });
	// Signed in: the welcome with the organisation's name, before the slides
	// or, for a member who has been here, the app (the owner, 2026-10-29).
	const [welcome, setWelcome] = useState(WELCOME_PREVIEW ? { name: "Marlowe Finch LLP", returning: PREVIEW === "welcome-sign-in", celebrate: PREVIEW === "welcome" } : null);
	const timer = useRef(null);

	useEffect(() => {
		// A copy shortcut can target the document when no control has focus.
		const restrictCopy = (event) => {
			if (!(event.target instanceof HTMLInputElement)) event.preventDefault();
		};
		document.addEventListener("copy", restrictCopy, true);
		return () => {
			document.removeEventListener("copy", restrictCopy, true);
			clearTimeout(timer.current);
		};
	}, []);
	useEffect(() => {
		if (resumed) return undefined;
		let current = true;
		window.nativeApp.getAccountState().then(
			(state) => {
				if (!current) return;
				if (state?.signedIn) {
					// After a restart a colleague who joined is still shown a
					// member's slides: the backend says what the seat may do (D-184).
					window.nativeApp.getOrganisation?.().then(
						(organisation) => current && organisation?.viewer?.may?.connectModels === false && setJoined(true),
						() => {},
					);
					setWarm(true);
					setPage(0);
				}
				setResumed(true);
			},
			() => current && setResumed(true),
		);
		return () => {
			current = false;
		};
	}, [resumed]);
	useEffect(() => {
		const keys = permissionKeys();
		if (keys.length === 0 || typeof window.nativeApp?.getSystemPermissions != "function") return undefined;
		let current = true;
		window.nativeApp.getSystemPermissions().then(
			(answer) => {
				const states = Object.fromEntries((answer?.permissions ?? []).map((item) => [item.key, item.state]));
				if (current) setMissingPermissions(keys.filter((key) => !["allowed", "not_needed"].includes(states[key])));
			},
			() => {},
		);
		return () => {
			current = false;
		};
	}, []);
	useEffect(() => {
		if (warm) return undefined;
		const warming = setTimeout(() => setWarm(true), WARM_AFTER_MS);
		return () => clearTimeout(warming);
	}, [warm]);
	useEffect(() => {
		if (page == null || !lensReady || !painting) return undefined;
		const letting = setTimeout(() => setPainting(false), PAINTING_LEAVES_MS);
		return () => clearTimeout(letting);
	}, [page, lensReady, painting]);

	const go = (next) => {
		setConfiguring(false);
		setWarm(true);
		setPage(Math.max(0, Math.min(next, pages.length - 1)));
	};
	// Signed in, the organisation is welcomed by its name. Then a member who has
	// been here (their name and position are kept on this computer) goes
	// straight back to work; anyone else is shown the app and asked what it
	// needs, slide by slide (D-134, D-136).
	const signedIn = async ({ organisationName, registered, joined } = {}) => {
		// A colleague who joined the firm is not shown the administrator's
		// set-up: models and AI colleagues are the firm's (D-184).
		setJoined(Boolean(joined));
		let returning = false;
		try {
			const viewer = (await window.nativeApp?.getOrganisation?.())?.viewer;
			returning = Boolean(viewer?.name && viewer?.position);
			if (viewer?.may?.connectModels === false) setJoined(true);
		} catch {
			// The slides ask for what is missing.
		}
		// A seat that went through onboarding on this computer before, with its
		// data here, goes straight back to work: signing it in marks onboarding
		// complete here (the owner, 2026-11-05, D-250).
		try {
			if ((await window.nativeApp?.getOnboardingStatus?.())?.completed === true) returning = true;
		} catch {
			// The slides ask for what is missing.
		}
		try {
			organisationName = organisationName || (await window.nativeApp?.getAccountState?.())?.organisationName;
		} catch {
			// Welcomed without its name.
		}
		setWarm(true);
		setPainting(false);
		setWelcome({ name: organisationName ?? null, returning, celebrate: Boolean(registered) });
	};
	const welcomed = () => {
		const { returning } = welcome;
		if (returning) {
			complete();
			return;
		}
		setWelcome(null);
		go(0);
	};
	const leave = () => {
		if (leaving) return;
		setLeaving(true);
		const reduced = window.matchMedia("(prefers-reduced-motion: reduce)").matches;
		timer.current = setTimeout(() => onComplete?.(), reduced ? 0 : LEAVE_MS);
	};
	// A colleague who joined waits for the firm before the app opens; the page
	// goes on by itself once the firm's computers have come.
	const complete = () => {
		if (joined == true && holding == false) {
			setHolding(true);
			return;
		}
		leave();
	};

	// Where the page carries the window's caption (Windows, D-049), a strip along
	// its top drags the window and its buttons stand at the right, drawn light
	// over the painting and dark over the app (2026-10-29: the window could only
	// be closed with Alt+F4).
	const captionInPage = window.nativeApp?.windowFrame == "page" || document.documentElement.dataset.nativeWindowFrame == "page";
	const preview = page != null && !pages[page].scene ? pages[page].preview ?? pages[page].ask : null;
	// The system's questions the picture shows: those of the slide's own rows.
	const asked = page != null ? pages[page].permissions?.[platformOf()]?.map((item) => item.key) ?? null : null;

	return (
		<main className={styles.page} data-tour={!holding && page != null && !welcome || undefined} data-leaving={leaving || undefined} data-caption={captionInPage ? "page" : undefined} style={{ "--leave-ms": `${LEAVE_MS}ms` }}>
			{captionInPage ? <div className={styles.caption} aria-hidden="true" /> : null}
			<section className={styles.entry} aria-labelledby="onboarding-title">
				{holding ? (
					<FirmWait onDone={leave} />
				) : !resumed || welcome ? null : page == null ? (
					<EntryForm onComplete={signedIn} />
				) : (
					<Tour index={page} pages={pages} drafts={drafts} onDraftChange={setDrafts} onSetupChange={setConfiguring} onNext={() => page === pages.length - 1 ? complete() : go(page + 1)} onBack={(at) => go(at)} onComplete={complete} />
				)}
			</section>
			{/* While the computer waits for the firm, the painting alone, as at the
			    sign-in: none of the story's sample people or Matters stands beside
			    a real firm's wait (the PC's walk, 2026-11-02). */}
			<div className={styles.art}>
				{painting || holding ? <Painting /> : null}
				{warm && !holding ? <Lens paused={configuring} shown={page != null && !preview} scene={pages[page ?? 0].scene ?? "team"} profile={drafts.role ?? null} onReady={() => setLensReady(true)} /> : null}
				{preview && !holding ? <SetupPreview kind={preview} draft={drafts[preview]} asked={asked} /> : null}
			</div>
			{welcome ? <Welcome name={welcome.name} celebrate={welcome.celebrate} onContinue={welcomed} /> : null}
			<div className={styles.controls}>
				<WindowControls canMaximize={false} tone={!holding && (welcome || (page != null && lensReady)) ? null : "onDark"} />
			</div>
		</main>
	);
}
