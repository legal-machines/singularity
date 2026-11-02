import { Fragment, useEffect, useLayoutEffect, useRef, useState } from "react";

import { Avatar } from "../../components/Avatar/Avatar.jsx";
import { readPicture, squarePhoto } from "../../data/photos.js";
import { normalizePractice } from "../../data/practices.js";
import { BackButton } from "../BackButton.jsx";
import { Dots } from "../Dots.jsx";
import entry from "../entry/EntryForm.module.css";
import { LEGAL_LINKS, LEGAL_TITLES } from "../legal.js";
import { PAGES, POSITIONS, platformOf } from "./pages.js";
import { PROVIDERS, isConnected, reasonOf } from "./providers.js";
import { downloadWords, protectionDetail, protectionFill, protectionMoving, protectionRuns, protectionWords, setupOf } from "./setup.js";
import { APPLICATIONS, ColleagueAsk, ConnectionAsk } from "./SetupDesign.jsx";
import styles from "./Tour.module.css";

// A chapter first shows the app at work. Its related setup opens on request,
// without discarding what was entered when the member goes back to the story.
export function Tour(props) {
	const [setup] = useState(() => setupOf(window.nativeApp));
	const [drafts, setDrafts] = useState({});
	return <Chapter key={props.index} {...props} drafts={props.drafts ?? drafts} onDraftChange={props.onDraftChange ?? setDrafts} setup={setup} />;
}

function Chapter({ index, pages = PAGES, setup, onNext, onBack, onComplete, onSetupChange, drafts = {}, onDraftChange }) {
	const page = pages[index];
	const definingRole = page.ask === "colleague" && drafts.colleague?.step === 1;
	const connecting = page.ask === "apps" ? APPLICATIONS.find(app => app.key === drafts.apps?.selected) : null;
	const [editing, setEditing] = useState(false);
	const [busy, setBusy] = useState(false);
	const [ready, setReady] = useState(false);
	const heading = useRef(null);
	const group = useRef(null);
	const footnote = useRef(null);
	const lift = useFootnoteLift(footnote, page.disclaimer);
	useEffect(() => {
		let live = true;
		Promise.all([document.fonts?.load('500 40px "Sora Entry"'), document.fonts?.ready]).then(() => live && setReady(true), () => live && setReady(true));
		return () => { live = false; };
	}, []);
	useEffect(() => {
		if (!ready) return undefined;
		heading.current?.focus({ preventScroll: true });
		group.current.scrollTop = 0;
		const reduced = window.matchMedia?.("(prefers-reduced-motion: reduce)").matches;
		const motion = group.current.animate([{ opacity: 0 }, { opacity: 1 }], { duration: reduced ? 0 : 220, easing: "ease-out" });
		return () => motion.cancel();
	}, [ready, editing]);
	useEffect(() => {
		if (!ready) return;
		heading.current?.focus({ preventScroll: true });
		group.current.scrollTop = 0;
	}, [ready, editing, definingRole, connecting?.key, drafts.apps?.stage]);
	useEffect(() => {
		onSetupChange?.(editing);
		return () => onSetupChange?.(false);
	}, [editing, onSetupChange]);
	const back = () => {
		if (editing && connecting) onDraftChange(previous => ({ ...previous, apps: { ...previous.apps, selected: null, stage: 0 } }));
		else if (editing) setEditing(false);
		else onBack(index - 1);
	};
	const formProps = { setup, active: editing, busy, setBusy, onDone: onNext };
	const designProps = { active: editing, draft: drafts[page.ask], onChange: value => onDraftChange?.(previous => ({ ...previous, [page.ask]: value })), onDone: onNext };
	return (
		<div className={`${entry.entry} ${styles.tour}`} style={lift > 0 ? { "--footnote-lift": `${lift}px` } : undefined}>
			{index > 0 || editing ? <BackButton className={styles.back} disabled={busy} onClick={back} /> : null}
			<div ref={group} className={styles.group} data-ready={ready || undefined} data-mode={editing ? "setup" : "story"}>
				<header className={styles.header}>
					<h1 ref={heading} id="onboarding-title" tabIndex={-1} className={styles.title}>{editing ? connecting ? `Connect ${connecting.name}.` : definingRole ? "An area to lead." : page.setupTitle : page.title}</h1>
					<p className={styles.line}>{editing ? connecting ? connecting.line : definingRole ? "Choose their role, the rank they hold and their area." : page.setupLine : page.line}</p>
					{!editing && page.note ? <p className={styles.note}>{page.note}</p> : null}
				</header>
				{!editing ? <StoryList page={page} /> : null}
				<div className={styles.body} hidden={editing}>
					<button className={entry.primary} type="button" onClick={page.ask ? () => setEditing(true) : page.next ? onNext : onComplete}>
						<span className={entry.buttonLabel}>{page.action}</span>
					</button>
					{page.skip ? <button className={styles.later} type="button" onClick={onNext}>{page.skip}</button> : null}
				</div>
				{page.ask ? <div className={styles.body} hidden={!editing}>
					{page.ask === "role" ? <RoleAsk {...formProps} draft={drafts.role} onChange={value => onDraftChange?.(previous => ({ ...previous, role: value }))} /> : null}
					{page.ask === "models" ? <ModelsAsk {...formProps} /> : null}
					{page.ask === "protection" ? <ProtectionAsk {...formProps} onChange={value => onDraftChange?.(previous => ({ ...previous, protection: value }))} /> : null}
					{page.ask === "folder" ? <FolderAsk {...formProps} /> : null}
					{page.ask === "colleague" ? <ColleagueAsk {...designProps} setup={setup} busy={busy} setBusy={setBusy} /> : null}
					{page.ask === "apps" ? <ConnectionAsk {...designProps} setup={setup} busy={busy} setBusy={setBusy} /> : null}
				</div> : null}
			</div>
			<Dots className={styles.progress} steps={pages.map(item => item.title)} current={index} label="Onboarding steps" canGo={at => !busy && at < index} onGo={onBack} />
			{page.disclaimer ? <Disclaimer page={page} footnoteRef={footnote} /> : null}
		</div>
	);
}

// A slide's footnote: what its picture is, then the pages it rests on, each a
// link, joined by commas (the owner, 2026-11-01, D-198). A link is a legal
// document's key (legal.js) or a published page of its own; a slide that names
// none points to the Terms of Use.
function Disclaimer({ page, footnoteRef }) {
	const links = (page.disclaimerLinks ?? ["terms"]).map(link => typeof link == "string" ? { text: LEGAL_TITLES[link], href: LEGAL_LINKS[link] } : link);
	return <p ref={footnoteRef} className={styles.disclaimer}>
		{page.disclaimer} See the {links.map((link, index) => <Fragment key={link.href}>{index > 0 ? ", " : null}<a href={link.href} target="_blank" rel="noreferrer">{link.text}</a></Fragment>)}.
	</p>;
}

// The height a slide's footnote takes beyond its first line, measured as it
// lays out and again whenever it reflows: Back and the step dots rise by it
// (Tour.module.css), so Back's hit area never reaches the footnote's words
// (the owner, 2026-11-01: a two-line footnote stood cut under Back).
const FOOTNOTE_LINE = 16;
function useFootnoteLift(footnote, text) {
	const [lift, setLift] = useState(0);
	useLayoutEffect(() => {
		const node = footnote.current;
		if (node == null) { setLift(0); return undefined; }
		const measure = () => setLift(Math.max(0, Math.round(node.getBoundingClientRect().height) - FOOTNOTE_LINE));
		measure();
		const observer = typeof ResizeObserver == "function" ? new ResizeObserver(measure) : null;
		observer?.observe(node);
		return () => observer?.disconnect();
	}, [footnote, text]);
	return lift;
}

// The lock also covers two submits in the same render; a completed native
// request cannot navigate a chapter that has since been unmounted.
function useTask(setBusy) {
	const locked = useRef(false);
	const live = useRef(true);
	useEffect(() => { live.current = true; return () => { live.current = false; }; }, []);
	return async (work) => {
		if (locked.current) return null;
		locked.current = true;
		setBusy(true);
		try { const value = await work(); return live.current ? { value } : null; }
		catch (error) { return live.current ? { error } : null; }
		finally { locked.current = false; if (live.current) setBusy(false); }
	};
}

// The member in the firm (D-162, D-163, D-166): their photo, name, position
// and practice. What they type shows in the demonstration beside them at
// once; the practice takes its official name when they leave the field, and
// the photo is cut to the firm's square here, on their computer.
function RoleAsk({ setup, busy, setBusy, onDone, draft, onChange }) {
	const [name, setName] = useState(draft?.name ?? "");
	const [position, setPosition] = useState(draft?.position ?? "");
	const [practice, setPractice] = useState(draft?.practice ?? "");
	const [photo, setPhoto] = useState(draft?.photo ?? null);
	const [photoChanged, setPhotoChanged] = useState(draft?.photoChanged ?? false);
	const [error, setError] = useState("");
	// The fields the member has edited, kept with the draft: a late read of
	// what is saved never overwrites them.
	const touched = useRef(new Set(draft?.touched ?? []));
	const run = useTask(setBusy);
	useEffect(() => {
		let live = true;
		setup.viewer().then(viewer => {
			if (!live || !viewer) return;
			if (!touched.current.has("name")) setName(viewer.name || "");
			if (!touched.current.has("position")) setPosition(viewer.position || "");
			if (!touched.current.has("practice")) setPractice(viewer.practice || "");
			if (!touched.current.has("photo")) setPhoto(viewer.photo || null);
		}, () => {});
		return () => { live = false; };
	}, [setup]);
	useEffect(() => { onChange?.({ name, position, practice, photo, photoChanged, touched: [...touched.current] }); }, [name, position, practice, photo, photoChanged]);
	const change = (field, save) => event => { touched.current.add(field); save(event.target.value); setError(""); };
	const takePicture = async picture => {
		if (picture == null) return;
		try {
			const square = await squarePhoto(picture);
			touched.current.add("photo");
			setPhoto(square);
			setPhotoChanged(true);
			setError("");
		} catch {
			setError("This picture can’t be used. Choose a JPEG, PNG, GIF or WebP picture.");
		}
	};
	const choosePhoto = async () => {
		setError("");
		try { await takePicture(await setup.choosePhoto()); }
		catch (reason) { setError(reason?.detail ?? reason?.message ?? "The picture couldn’t be opened. Choose another."); }
	};
	const dropPhoto = async event => {
		event.preventDefault();
		if (busy) return;
		try { await takePicture(await readPicture(event.dataTransfer?.files?.[0])); }
		catch (reason) { setError(reason.message); }
	};
	const removePhoto = () => { touched.current.add("photo"); setPhoto(null); setPhotoChanged(true); };
	const submit = async event => {
		event.preventDefault();
		if (!name.trim() || !position) return setError(!name.trim() ? "Enter your name." : "Choose your position.");
		const named = normalizePractice(practice).trim();
		setPractice(named);
		setError("");
		const result = await run(() => setup.saveRole(name.trim(), position, named, photoChanged ? photo : undefined));
		if (!result) return;
		if (result.error) setError("Your profile wasn’t saved. Try again.");
		else { setPhotoChanged(false); onDone(); }
	};
	const face = { id: "onboarding-you", kind: "person", name: name.trim(), color: "#5b6170", photo };
	return <form className={styles.ask} noValidate onSubmit={submit} aria-busy={busy} aria-label="You in the firm">
		<div className={styles.photo} onDragOver={event => event.preventDefault()} onDrop={dropPhoto}>
			<button className={styles.face} type="button" disabled={busy} aria-label={photo ? "Change your photo" : "Add your photo"} onClick={choosePhoto}>
				<Avatar member={face} size={56} />
			</button>
			<span className={styles.photoText}>
				<span className={styles.photoLabel}>Your photo <span className={styles.optional}>(optional)</span></span>
				<span className={styles.photoActions}>
					<button className={styles.inline} type="button" disabled={busy} onClick={choosePhoto}>{photo ? "Change" : "Choose a photo"}</button>
					{photo ? <button className={styles.inline} type="button" disabled={busy} onClick={removePhoto}>Remove</button> : null}
				</span>
			</span>
		</div>
		<div className={entry.fields}>
			<div className={entry.field}>
				<label htmlFor="tour-name">Your name</label>
				<input id="tour-name" autoComplete="name" maxLength={120} value={name} readOnly={busy} aria-invalid={Boolean(error) && !name.trim()} onChange={change("name", setName)} />
			</div>
			<div className={entry.field}>
				<label htmlFor="tour-position">Your position</label>
				<select id="tour-position" className={styles.select} value={position} disabled={busy} onChange={change("position", setPosition)}>
					<option value="" disabled>Choose your position</option>
					{POSITIONS.map(item => <option key={item}>{item}</option>)}
				</select>
			</div>
			<div className={entry.field}>
				<label htmlFor="tour-practice">Practice <span className={styles.optional}>(optional)</span></label>
				<input id="tour-practice" value={practice} maxLength={160} readOnly={busy} placeholder="Mergers & Acquisitions" onChange={change("practice", setPractice)} onBlur={() => setPractice(value => normalizePractice(value))} />
			</div>
		</div>
		<p className={styles.error} role="status">{error}</p>
		<button className={entry.primary} type="submit" disabled={busy || !name.trim() || !position}><span className={entry.buttonLabel}>{busy ? "Saving…" : "That’s me"}</span></button>
	</form>;
}

// The protection model's state, read while its page shows: the backend sends
// no event for it (papinian.getModelStatus). Every two seconds, and twice a
// second while it comes, so its button fills as the bytes arrive.
function useProtection(setup, active = true, often = false) {
	const [protection, setProtection] = useState(null);
	useEffect(() => {
		if (!active) return undefined;
		let live = true;
		let timer;
		const read = () => setup.protection().then(status => live && setProtection(status), () => {}).finally(() => { if (live) timer = setTimeout(read, often ? 500 : 2000); });
		read();
		return () => { live = false; clearTimeout(timer); };
	}, [setup, active, often]);
	return [protection, setProtection];
}

// A story's list under its words: the facts it rests on (D-195), or what this
// computer may ask and why.
function StoryList({ page }) {
	const items = page.permissions?.[platformOf()] ?? null;
	if (items) {
		return <ul className={styles.features}>
			{items.map(item => <li key={item.key ?? item.name}><span className={styles.featureName}>{item.name}</span><span className={styles.featureLine}>{item.line}</span></li>)}
		</ul>;
	}
	if (page.facts) return <ul className={styles.facts}>{page.facts.map(fact => <li key={fact}>{fact}</li>)}</ul>;
	return null;
}

// How long the button stays full, saying the model runs, before onboarding
// goes on by itself.
const ARRIVED_MS = 700;

// Client data protection on this computer (D-195): masking on or off. Its
// model comes from Legal Machines under the Software License Agreement the
// firm accepted before anything downloaded (its section 4). It downloads only
// when the member chooses protection and presses the button, which fills from
// the left as the model comes (the owner, 2026-11-01, D-198); once the model
// runs, onboarding goes on by itself. A failure says what went wrong and the
// same button tries again; Off goes on without the model.
function ProtectionAsk({ setup, active, busy, setBusy, onDone, onChange }) {
	const [on, setOn] = useState(true);
	const [error, setError] = useState("");
	const [fetching, setFetching] = useState(false);
	const [arrived, setArrived] = useState(false);
	const [protection, setProtection] = useProtection(setup, active, fetching);
	const run = useTask(setBusy);
	const leaving = useRef(null);
	useEffect(() => () => clearTimeout(leaving.current), []);
	useEffect(() => {
		let live = true;
		setup.protectionOn?.().then(value => live && setOn(value), () => {});
		return () => { live = false; };
	}, [setup]);
	// The picture beside the form shows what leaves with the choice made.
	useEffect(() => { onChange?.({ on }); }, [on]);
	const runs = protectionRuns(protection);
	const failed = protection?.status === "error";
	// A sign-in Legal Machines no longer accepts: said in its own words.
	const waiting = protection?.status === "authorization_required";
	// A download already under way when the page opens is followed, not started
	// again.
	const moving = !fetching && protectionMoving(protection);
	const submit = async event => {
		event.preventDefault();
		if (busy || fetching || arrived || moving) return;
		setError("");
		const fetch = on && !runs;
		const result = await run(async () => {
			try { await setup.setProtectionOn(on); }
			catch { return { saved: false }; }
			if (!fetch) return { saved: true };
			setFetching(true);
			return { saved: true, status: await setup.downloadProtection() };
		});
		if (!result) return;
		if (result.value?.saved === false) return setError("Singularity could not change protection on this computer. Try again.");
		if (!fetch) return onDone();
		const status = result.value?.status?.status ? result.value.status : await setup.protection().catch(() => null);
		if (status) setProtection(status);
		if (result.error == null && protectionRuns(status)) {
			setArrived(true);
			setFetching(false);
			leaving.current = setTimeout(onDone, ARRIVED_MS);
			return;
		}
		setFetching(false);
		if (status?.status !== "error" && status?.status !== "authorization_required") setError("The protection model did not download. Try again.");
	};
	const filling = on && (fetching || arrived || moving);
	const label = arrived ? "Ready"
		: filling ? downloadWords(protection)
		: busy ? "Saving…"
		: !on || runs ? "Continue"
		: failed || waiting ? "Try again"
		: "Download and continue";
	// One line, in every state but a failure, so nothing moves as the member
	// switches or the download starts (the owner, 2026-11-01).
	const state = !on ? (runs ? "" : "Nothing downloads.")
		: protection == null ? ""
		: filling ? "From Legal Machines, to run only on this computer."
		: failed || runs || waiting ? protectionWords(protection, "The protection model")
		: "The model, about 630 MB, downloads when you continue.";
	const detail = !on || filling ? "" : waiting ? (protection.message ?? "").trim() : protectionDetail(protection);
	const ways = [
		{ key: true, name: "Protect what leaves", line: "Names and figures masked, then restored.", tag: "Recommended" },
		{ key: false, name: "Off on this computer", line: "Work leaves as written, names included." },
	];
	return <form className={styles.ask} noValidate onSubmit={submit} aria-busy={busy || moving} aria-label="Protection on this computer">
		<fieldset className={styles.ways} disabled={busy || arrived || moving}>
			<legend className={styles.srOnly}>Protection on this computer</legend>
			{ways.map(way => <label key={String(way.key)} className={styles.way} data-selected={on === way.key || undefined}>
				<input type="radio" name="protection" checked={on === way.key} onChange={() => { setOn(way.key); setError(""); }} />
				<span><span className={styles.wayName}>{way.name}</span><span className={`${styles.wayLine} ${styles.wayLineFull}`}>{way.line}</span></span>
				{way.tag ? <span className={styles.recommended}>{way.tag}</span> : null}
			</label>)}
		</fieldset>
		<p className={styles.protection} role="status">{state || " "}</p>
		{filling ? <p className={styles.srOnly} role="status">{protectionWords(protection, "The protection model")}</p> : null}
		{detail ? <p className={styles.help}>{detail}</p> : null}
		<p className={styles.error} role="status">{error}</p>
		<button className={`${entry.primary} ${styles.filling}`} type="submit" disabled={busy || arrived || moving} data-filling={filling || undefined} style={filling ? { "--fill": `${arrived ? 100 : protectionFill(protection)}%` } : undefined}>
			<span className={entry.buttonLabel}>{label}</span>
			{filling ? <span className={styles.fill} aria-hidden="true"><span className={entry.buttonLabel}>{label}</span></span> : null}
		</button>
	</form>;
}

function ModelsAsk({ setup, active, busy, setBusy, onDone }) {
	const [connected, setConnected] = useState([]);
	const [loading, setLoading] = useState(true);
	const [providerKey, setProviderKey] = useState(PROVIDERS[0].key);
	const [key, setKey] = useState("");
	const [error, setError] = useState("");
	const keyRef = useRef(null);
	const run = useTask(setBusy);
	const provider = PROVIDERS.find(item => item.key === providerKey);
	const isOn = isConnected(provider, connected);
	useEffect(() => {
		let live = true;
		setup.providers().then(list => {
			if (!live) return;
			setConnected(list ?? []);
			const existing = PROVIDERS.find(item => isConnected(item, list ?? []));
			if (existing) setProviderKey(existing.key);
		}, () => {}).finally(() => live && setLoading(false));
		return () => { live = false; };
	}, [setup]);
	useEffect(() => {
		if (!active) { setKey(""); setError(""); }
	}, [active]);
	const submit = async event => {
		event.preventDefault();
		if (loading || busy) return;
		if (isOn) return onDone();
		if (!key.trim()) return setError("Paste the API key.");
		setError("");
		const result = await run(() => setup.connect(provider, key.trim()));
		if (!result) return;
		if (result.error) { setError(reasonOf(result.error)); keyRef.current?.focus(); }
		else { setKey(""); onDone(); }
	};
	return <form className={styles.ask} noValidate onSubmit={submit} aria-busy={busy} aria-label="Connect a model account">
		<div className={entry.fields}>
			<div className={entry.field}>
				<label htmlFor="tour-provider">Model account</label>
				<select id="tour-provider" className={styles.select} disabled={busy || loading} value={providerKey} onChange={event => { setProviderKey(event.target.value); setKey(""); setError(""); }}>
					{PROVIDERS.map(item => <option key={item.key} value={item.key}>{item.name}</option>)}
				</select>
				<p className={styles.help}>{loading ? "Checking your accounts…" : isOn ? "Connected on this computer." : provider.line}</p>
			</div>
			{!isOn ? <div className={entry.field}>
				<label htmlFor="tour-key">API key</label>
				<input ref={keyRef} id="tour-key" type="password" autoComplete="off" spellCheck={false} value={key} readOnly={busy || loading} placeholder="Paste your API key" aria-invalid={Boolean(error)} aria-describedby={error ? "tour-model-error" : undefined} onChange={event => { setKey(event.target.value); setError(""); }} />
			</div> : null}
		</div>
		<p id="tour-model-error" className={styles.error} role="status">{error}</p>
		<button className={entry.primary} type="submit" disabled={busy || loading || !isOn && !key.trim()}><span className={entry.buttonLabel}>{busy ? "Connecting…" : isOn ? "Continue" : "Connect and continue"}</span></button>
		<button className={styles.later} type="button" disabled={busy} onClick={onDone}>Set up later</button>
	</form>;
}

const WAYS = [
	{ key: "direct", name: "Direct sharing", line: "End-to-end encrypted, between computers.", tag: "Recommended" },
	{ key: "folder", name: "A folder your firm shares", line: "OneDrive, Dropbox or a shared drive." },
	{ key: "local", name: "Start on this computer", line: "Share individual Matters when you’re ready." },
];

function FolderAsk({ setup, busy, setBusy, onDone }) {
	const [way, setWay] = useState("direct");
	const [folder, setFolder] = useState(null);
	const [error, setError] = useState("");
	const run = useTask(setBusy);
	const touched = useRef(false);
	useEffect(() => {
		let live = true;
		setup.firmFolder().then(found => { if (live && found && !touched.current) { setFolder(found); setWay("folder"); } }, () => {});
		return () => { live = false; };
	}, [setup]);
	const choose = async () => {
		setError("");
		const result = await run(() => setup.chooseFirmFolder());
		if (!result) return;
		if (result.error) setError("The folder wasn’t selected. Try again.");
		else if (result.value) setFolder(result.value);
	};
	const name = folder?.split(/[\\/]/).filter(Boolean).pop();
	const needsFolder = way === "folder" && !folder;
	return <div className={styles.ask}>
		<fieldset className={styles.ways} disabled={busy}>
			<legend className={styles.srOnly}>How your Matters travel</legend>
			{WAYS.map(option => <label className={styles.way} key={option.key} data-selected={way === option.key || undefined}>
				<input type="radio" name="tour-way" value={option.key} checked={way === option.key} onChange={() => { touched.current = true; setWay(option.key); setError(""); }} />
				<span><span className={styles.wayName}>{option.name}</span><span className={styles.wayLine}>{option.line}</span>{option.tag ? <span className={styles.recommended}>{option.tag}</span> : null}</span>
			</label>)}
		</fieldset>
		{way === "folder" && folder ? <div className={styles.folder}><span title={folder}>{name}</span><button type="button" className={styles.inline} disabled={busy} onClick={choose}>Change folder</button></div> : null}
		<p className={styles.help}>{way === "local" ? "Inviting a colleague to a Matter enables sharing for it." : "Only your firm can open shared Matters."}</p>
		<p className={styles.error} role="status">{error}</p>
		<button className={entry.primary} type="button" disabled={busy} onClick={needsFolder ? choose : onDone}><span className={entry.buttonLabel}>{busy ? "Choosing…" : needsFolder ? "Choose a folder" : "Use this setup"}</span></button>
	</div>;
}
