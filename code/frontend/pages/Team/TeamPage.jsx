import { useEffect, useState } from "react";
import { DEMO_MEMBER, isEmulation } from "../../emulation/emulation.js";

import { Avatar } from "../../components/Avatar/Avatar.jsx";
import { Button, IconButton } from "../../components/Button/Button.jsx";
import { Dialog } from "../../components/Dialog/Dialog.jsx";
import { toast } from "../../components/Toast/toast.js";
import { useNavigation } from "../../components/Navigation/navigation.js";
import { Page, PageHead } from "../../components/Page/Page.jsx";
import { SheetBar } from "../../components/SheetBar/SheetBar.jsx";
import { Views } from "../../components/Views/Views.jsx";
import { ProfileFields, emptyProfile } from "../../components/ProfileFields/ProfileFields.jsx";
import { hasNative, nativeApp, refreshFirm, teamMembers } from "../../data/native.js";
import { emulationOf } from "../../onboarding/entry/account.js";
import { organisation, viewerMay } from "../../data/workspace.js";
import { colleagueState, memberAway } from "../threadState.js";
import { byFirmOrder, nameAsSeen, officeLine, positionLevels, positionOf } from "../words.js";

import styles from "./TeamPage.module.css";

// The team: every member of the firm, people and AI colleagues in one list,
// in the firm's order, by position, then by surname (D-059). An AI colleague
// stands with the position its role holds, so Kitsune stands with the
// partners. Its face lives, and its line says what it is doing. The list
// stands on the page itself, in groups by position, each named in plain
// words, with no frame, no labels in capitals and no lines between its rows
// (the owner, 2026-11-02: "kickers and borders are AI slop; the division by
// categories is needed, and filters - in our style, in Twist's spirit"); over
// it, as over Inbox, which members it shows. Each row says its member's
// office in one phrase, as the conversation's head does.
export function TeamPage({ state }) {
	const { open } = useNavigation();
	const [isAdding, setAdding] = useState(false);
	// Who sits in each seat is read again as the team opens: a colleague's
	// profile may have come from the firm's record since (D-209).
	useEffect(() => {
		if (hasNative() && isEmulation == false) refreshFirm();
	}, []);
	// The firm's own members in the app; the prototype's in a plain browser.
	// The team at work in its order; the archived apart at its foot, the
	// removed not at all (D-214).
	const everyone = teamMembers().filter(member => !isEmulation || ["m_themis", "m_gabriel", "m_evelyn", DEMO_MEMBER].includes(member.id))
		.filter(member => member.isRemoved != true);
	const archived = everyone.filter(member => member.isArchived == true).sort(byFirmOrder);
	const members = everyone.filter(member => member.isArchived != true);
	const ordered = [...members].sort(byFirmOrder);
	const people = members.filter(member => member.kind != "ai_colleague").length;
	const colleagues = members.length - people;
	const openMember = memberId => open({ kind: "profile", memberId });
	const [view, setView] = useState("everyone");
	const views = [
		{ id: "everyone", label: "Everyone" },
		{ id: "people", label: "People" },
		{ id: "colleagues", label: "AI colleagues" },
		...(archived.length > 0 ? [{ id: "archived", label: "Archived" }] : []),
	];
	const shownView = views.some(item => item.id == view) ? view : "everyone";
	const groups = shownView == "archived" ? [{ title: "Archived", members: archived }]
		: groupsOf(ordered.filter(member => shownView == "everyone" || (shownView == "people") == (member.kind != "ai_colleague")));

	return (
		<Page asInbox bar={<SheetBar trail={[{ label: organisation.name }, { label: "Team" }]} />}>
			<PageHead
				asInbox
				title="Team"
				line={isEmulation ? undefined : teamLine(people, colleagues, organisation.shortName)}
				actions={isEmulation ? null : (
					<>
						{hasNative() && viewerMay.manageSeats ? <Button icon="plus" onClick={() => setAdding(true)}>Add a person</Button> : null}
						{viewerMay.hire ? <Button variant="primary" icon="plus" onClick={() => open({ kind: "hire" })}>Add an AI colleague</Button> : null}
					</>
				)}
			/>
			{isAdding ? <AddColleagueDialog onClose={() => setAdding(false)} /> : null}
			{isEmulation ? null : <div className={styles.toolbar}><Views label="Which members" views={views} view={shownView} onChange={setView} controls="team-members" /></div>}
			<div className={styles.groups} id="team-members" role={isEmulation ? undefined : "tabpanel"}>
				{groups.map(group => (
					<section key={group.title} className={styles.group} aria-label={group.title}>
						<h2 className={styles.group_title}>{group.title}<span className={styles.group_count}>{group.members.length}</span></h2>
						<ul className={styles.list}>
							{group.members.map(member => (
								<MemberRow key={member.id} member={member} state={state} isArchived={shownView == "archived"} onOpen={openMember} />
							))}
						</ul>
					</section>
				))}
				{groups.length == 0 ? <p className={styles.none}>{shownView == "colleagues" ? "No AI colleague at the firm yet." : "No one here yet."}</p> : null}
			</div>
		</Page>
	);
}

// The members by their position, in the firm's order: the partners, then the
// counsel, and so on; one joining the firm, and one with no position, apart.
function groupsOf(members) {
	const groups = [];
	for (const member of members) {
		const level = member.position ?? member.ranksWith;
		const title = level == "Partner" ? "Partners" : level == "Senior Counsel" ? "Senior counsel" : level == "Of Counsel" ? "Of counsel"
			: level == "Counsel" ? "Counsel" : level == "Senior Associate" ? "Senior associates" : level == "Associate" ? "Associates"
			: level == "Paralegal" ? "Paralegals" : member.joining == true ? "New to the firm" : "Others";
		const group = groups.find(item => item.title == title);
		if (group == null) groups.push({ title, order: positionLevels.indexOf(level), members: [member] });
		else group.members.push(member);
	}
	return groups;
}

// "3 people and 2 AI colleagues at Whitfield Crane, in the firm's order." A
// count of none is not said: "1 person at Whitfield Crane, …".
export function teamLine(people, colleagues, firm) {
	const parts = [
		people > 0 ? `${people} ${people == 1 ? "person" : "people"}` : null,
		colleagues > 0 ? `${colleagues} AI ${colleagues == 1 ? "colleague" : "colleagues"}` : null,
	].filter(Boolean);
	return `${parts.length > 0 ? parts.join(" and ") : "No one yet"} at ${firm}, in the firm's order.`;
}

// One member: the face, the name, and under it their office in one phrase -
// or, for one archived, that they are; at the right what an AI colleague is
// doing, or when a person is away. The whole row opens their page.
function MemberRow({ member, state, isArchived = false, onOpen }) {
	const [isPointedAt, setPointedAt] = useState(false);
	const now = member.kind == "ai_colleague" && isArchived == false ? colleagueState(member.id, state) : null;
	const away = isArchived ? null : memberAway(member, state);
	const line = isArchived ? "Archived" : isEmulation ? positionOf(member) : officeLine(member) || positionOf(member);

	return (
		<li className={styles.member} data-joining={isEmulation && member.id == DEMO_MEMBER || undefined} data-archived={isArchived || undefined}>
			<button
				className={styles.member_open}
				type="button"
				onClick={() => onOpen(member.id)}
				onMouseEnter={() => setPointedAt(true)}
				onMouseLeave={() => setPointedAt(false)}
			>
				<Avatar member={member} size={40} presence={now?.presence} pointedAt={isPointedAt} />
				<span className={styles.member_text}>
					<span className={styles.member_name}>{nameAsSeen(member)}</span>
					{line != "" ? <span className={styles.member_line}>{line}</span> : null}
				</span>
				{/* What it does now, or when they are back, as a word in its own
				    colour, never in a frame (the owner's rule: status as words). */}
				{isEmulation ? null : now != null ? <span className={styles.member_now} data-tone={now.kind == "yours" ? "yours" : now.kind == "working" ? "work" : "plain"}>{now.text}</span>
					: away != null ? <span className={styles.member_now} data-tone="time">{away}</span> : null}
			</button>
		</li>
	);
}

// The administrator adds a colleague (D-204), on a roomy page: the three
// things the colleague signs in with - the firm's email, the seat's login and
// its password - shown before the seat is made. The login is drawn here from
// the set Legal Machines draws from, never typed, so no one puts a person's
// name in it; the password is drawn too, strong, and either may be drawn
// again (the owner, 2026-11-02: no password of one's own). Once Legal Machines
// makes the seat, the same page hands them over; a press on the login or the
// password copies both. Who the colleague is stays in the firm.
const JURISTS = [
	"ulpian", "gaius", "paulus", "papinian", "modestinus", "justinian", "tribonian", "scaevola", "labeo", "proculus",
	"sabinus", "celsus", "julianus", "marcellus", "florentinus", "hermogenian", "africanus", "pomponius", "callistratus", "marcianus",
	"venuleius", "terentius", "javolenus", "aquilius", "trebatius", "capito", "cassius", "neratius", "valens", "paconius",
	"aristo", "atilicinus", "octavenus", "priscus", "quintus", "rufus", "severus", "theophilus", "vindianus", "volusius",
];
// None of 0, o, 1, l or i, which are taken for one another on a note.
const SEAT_LETTERS = "abcdefghjkmnpqrstuvwxyz23456789";

function randomIndexes(count, below) {
	const values = new Uint32Array(count);
	crypto.getRandomValues(values);
	return [...values].map(value => value % below);
}

export function drawSeatId() {
	const [name, ...letters] = [randomIndexes(1, JURISTS.length)[0], ...randomIndexes(3, SEAT_LETTERS.length)];
	return `${JURISTS[name]}-${letters.map(at => SEAT_LETTERS[at]).join("")}`;
}

export function suggestedPassword() {
	return randomIndexes(16, SEAT_LETTERS.length).map(at => SEAT_LETTERS[at]).join("").match(/.{4}/g).join("-");
}

function AddColleagueDialog({ onClose }) {
	// Who they are, then their sign-in, then the hand-over (D-209).
	const [step, setStep] = useState("who");
	const [profile, setProfile] = useState(emptyProfile);
	const [seatId, setSeatId] = useState(drawSeatId);
	const [password, setPassword] = useState(suggestedPassword);
	const [isMaking, setMaking] = useState(false);
	const [error, setError] = useState(null);
	const firmEmail = useFirmEmail();
	const isMade = step == "made";

	async function makeSeat() {
		if (isMaking) return;
		setMaking(true);
		setError(null);
		try {
			// A whole-product test stands in for Legal Machines (onboarding's
			// account.js): its seat is made nowhere.
			if (emulationOf(nativeApp()) == null) await nativeApp().createFirmSeat(seatId, password);
		} catch (reason) {
			setError(reason?.detail ?? reason?.message ?? "The seat could not be made. Try again.");
			setMaking(false);
			return;
		}
		// Who sits in it goes into the firm's own record, never to Legal
		// Machines; the seat stands even if this computer holds no record yet.
		try {
			if (emulationOf(nativeApp()) == null) {
				await nativeApp().saveSeatProfile({ seatId, ...profile, name: profile.name.trim(), photo: profile.photo ?? "" });
				refreshFirm();
			}
		} catch {
			toast("The seat is made. Their profile is kept once this computer holds the firm’s record.", { tone: "seal" });
		}
		setMaking(false);
		setStep("made");
	}

	async function copy(text, words) {
		try {
			await navigator.clipboard.writeText(text);
			toast(words);
		} catch {
			toast("It could not be copied. Select it and copy it by hand.", { tone: "seal" });
		}
	}

	// The login and password copy together, as the owner asked them written
	// (2026-11-02): "login: …" over "pass: …".
	const credentials = `login: ${seatId}\npass: ${password}`;
	const copyCredentials = () => copy(credentials, "The login and password are copied.");
	const firstName = profile.name.trim().split(/\s+/)[0] ?? "";
	if (step == "who") {
		return (
			<Dialog
				layout="spacious"
				size="small"
				title="Add a person"
				description="Who they are stays in your firm. Legal Machines never sees it."
				onClose={onClose}
				actions={(
					<>
						<Button onClick={onClose}>Cancel</Button>
						<Button variant="primary" disabled={profile.name.trim() == ""} onClick={() => setStep("signIn")}>Continue</Button>
					</>
				)}
			>
				<ProfileFields value={profile} onChange={setProfile} autoFocus />
			</Dialog>
		);
	}
	return (
		<Dialog
			layout="spacious"
			size="small"
			title={isMade ? (firstName ? `Ready to hand over to ${firstName}` : "Ready to hand over") : firstName ? `${firstName}’s sign-in` : "Their sign-in"}
			description={isMade
				? "Give these to them yourself. The password is not shown again."
				: "Singularity makes them a sign-in with no name in it."}
			initialFocus={isMade ? "button" : undefined}
			onClose={onClose}
			actions={isMade ? (
				<>
					<Button onClick={copyCredentials}>Copy login and password</Button>
					<Button variant="primary" onClick={onClose}>Done</Button>
				</>
			) : (
				<>
					<Button disabled={isMaking} onClick={() => setStep("who")}>Back</Button>
					<Button variant="primary" busy={isMaking} onClick={makeSeat}>Add {firstName || "colleague"}</Button>
				</>
			)}
		>
			<dl className={styles.signIn}>
				<SignInLine label="Email">
					{firmEmail == null ? (
						<span className={styles.signInHint}>Your firm’s email, the one you sign in with</span>
					) : isMade ? (
						<button type="button" className={styles.signInValue} onClick={() => copy(firmEmail, "The email is copied.")}>{firmEmail}</button>
					) : (
						<span className={styles.signInValue}>{firmEmail}</span>
					)}
				</SignInLine>
				<SignInLine label="Login" again={isMade || isMaking ? null : () => { setSeatId(drawSeatId()); setError(null); }} againLabel="Draw another login">
					<button type="button" className={styles.signInValue} data-code="" title="Copy the login and password" onClick={copyCredentials}>{seatId}</button>
				</SignInLine>
				<SignInLine label="Password" again={isMade || isMaking ? null : () => { setPassword(suggestedPassword()); setError(null); }} againLabel="Draw another password">
					<button type="button" className={styles.signInValue} data-code="" title="Copy the login and password" onClick={copyCredentials}>{password}</button>
				</SignInLine>
			</dl>
			{error != null ? <p className={styles.signInError} role="alert">{error}</p> : null}
		</Dialog>
	);
}

// A colleague's sign-in given again (D-214): a new password for their seat,
// drawn as one is when a colleague is added, or their archived seat made again
// with one. Shown before it is set, then handed over as Add a colleague does;
// the password is never shown again.
export function SeatPasswordDialog({ member, seatId, isRestoring = false, onSet, onClose }) {
	const [password, setPassword] = useState(suggestedPassword);
	const [isSetting, setSetting] = useState(false);
	const [isSet, setSet] = useState(false);
	const [error, setError] = useState(null);
	const firmEmail = useFirmEmail();
	const first = (member?.name ?? "").trim().split(/\s+/)[0] || "your colleague";

	async function set() {
		if (isSetting) return;
		setSetting(true);
		setError(null);
		try {
			await onSet(password);
			setSet(true);
		} catch (reason) {
			setError(reason?.detail ?? reason?.message ?? "It could not be set. Try again.");
		}
		setSetting(false);
	}

	async function copy(text, words) {
		try {
			await navigator.clipboard.writeText(text);
			toast(words);
		} catch {
			toast("It could not be copied. Select it and copy it by hand.", { tone: "seal" });
		}
	}

	const credentials = `login: ${seatId}\npass: ${password}`;
	const copyCredentials = () => copy(credentials, "The login and password are copied.");
	return (
		<Dialog
			layout="spacious"
			size="small"
			title={isSet ? `Ready to hand over to ${first}` : isRestoring ? `Restore ${first}` : `A new password for ${first}`}
			description={isSet
				? "Give these to them yourself. The password is not shown again."
				: isRestoring
					? `${first} signs in again with this password. Their history in the firm is as they left it.`
					: `${first}'s old password stops working once this is set.`}
			initialFocus={isSet ? "button" : undefined}
			onClose={onClose}
			actions={isSet ? (
				<>
					<Button onClick={copyCredentials}>Copy login and password</Button>
					<Button variant="primary" onClick={onClose}>Done</Button>
				</>
			) : (
				<>
					<Button disabled={isSetting} onClick={onClose}>Cancel</Button>
					<Button variant="primary" busy={isSetting} onClick={set}>{isRestoring ? `Restore ${first}` : "Set the password"}</Button>
				</>
			)}
		>
			<dl className={styles.signIn}>
				<SignInLine label="Email">
					{firmEmail == null
						? <span className={styles.signInHint}>Your firm’s email, the one you sign in with</span>
						: <span className={styles.signInValue}>{firmEmail}</span>}
				</SignInLine>
				<SignInLine label="Login">
					<button type="button" className={styles.signInValue} data-code="" title="Copy the login and password" onClick={copyCredentials}>{seatId}</button>
				</SignInLine>
				<SignInLine label="Password" again={isSet || isSetting ? null : () => { setPassword(suggestedPassword()); setError(null); }} againLabel="Draw another password">
					<button type="button" className={styles.signInValue} data-code="" title="Copy the login and password" onClick={copyCredentials}>{password}</button>
				</SignInLine>
			</dl>
			{error != null ? <p className={styles.signInError} role="alert">{error}</p> : null}
		</Dialog>
	);
}

// One thing the colleague signs in with: what it is, small, over the thing
// itself, and just after it, where it may be drawn again, the button for that.
function SignInLine({ label, again, againLabel, children }) {
	return (
		<div className={styles.signInLine}>
			<dt>{label}</dt>
			<dd>
				<span className={styles.signInSlot}>{children}</span>
				{again != null ? <IconButton icon="repeat" size="small" label={againLabel} className={styles.again} onClick={again} /> : null}
			</dd>
		</div>
	);
}

// The firm's email as this computer keeps it, from the account signed in
// here; one signed in before it was kept asks Legal Machines, which keeps it
// here from then on (D-204).
function useFirmEmail() {
	const [email, setEmail] = useState(null);
	useEffect(() => {
		if (hasNative() == false || typeof nativeApp().getAccountState != "function") return;
		let isCurrent = true;
		nativeApp().getAccountState().then(async state => {
			if (state?.organisationEmail) {
				if (isCurrent) setEmail(state.organisationEmail);
				return;
			}
			if (typeof nativeApp().listFirmSeats != "function") return;
			const listed = await nativeApp().listFirmSeats();
			if (isCurrent && listed?.organisationEmail) setEmail(listed.organisationEmail);
		}).catch(() => {});
		return () => {
			isCurrent = false;
		};
	}, []);
	return email;
}
