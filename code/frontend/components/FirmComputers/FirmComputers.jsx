import { useEffect, useRef, useState } from "react";

import { Avatar } from "../Avatar/Avatar.jsx";
import { Button } from "../Button/Button.jsx";
import { Chip } from "../Chip/Chip.jsx";
import { Dialog } from "../Dialog/Dialog.jsx";
import { Field, Input } from "../Field/Field.jsx";
import { Icon } from "../Icon/Icon.jsx";
import { toast } from "../Toast/toast.js";
import { Row, Rows, Tray } from "../Tray/Tray.jsx";
import {
	acceptGivenMatter, beginComputerCheck, chooseFirmFolder, closeFirmDialog, computerOwner, declineGivenMatter, endComputerCheck, giveMatter,
	invitationFor, isArriving, joinWithInvitation, keepComputer, letComputerIn, openFirmDialog, readComputerCheck, refuseComputer,
	refuseFirmComputer, takeMatter, useFirmComputers,
} from "../../data/coordination.js";
import { nativeApp, teamMembers } from "../../data/native.js";
import { sizeWords } from "../../data/transfers.js";
import { viewerId } from "../../data/workspace.js";
import { formatWhen } from "../../pages/words.js";
import { travelComputers, travelWords } from "./travel.js";

import styles from "./FirmComputers.module.css";

// How a Matter reaches a colleague's computer: the lawyer who has it sends an
// invitation, which names where this computer's direct link may be reached;
// the colleague's computer calls it there and asks to join; each lawyer
// compares the other computer's code before anything passes. A folder the firm
// keeps in step is a second way, for computers that cannot reach each other:
// offered in the dialogs, never required.

// Whether a Matter travels between the firm's computers, said in words
// under its name (travel.js).
export function MatterTravel({ matterId, className }) {
	const travel = useMatterTravel(matterId);
	if (travel == null) return null;
	return <p className={className ?? styles.travel}>{travelWords(travel.link, travel.devices, travel.owners)}</p>;
}

// The Matter's other computers and its link to them, read when the Matter
// opens and again, at most every ten seconds, until they are read and while a
// computer it hears from is still unnamed: a member's record can come after
// their first envelope does. Null until the firm's computers are read.
function useMatterTravel(matterId) {
	const firm = useFirmComputers();
	const link = firm.matters.find(matter => matter.matterId == matterId) ?? null;
	const [read, setRead] = useState({ matterId: null, members: null });
	const members = read.matterId == matterId ? read.members : null;
	const { devices, owners } = travelComputers(members, link);
	const unknown = devices.filter(device => owners[device] == null).join(",");
	const lastRead = useRef({ matterId: null, at: 0 });
	const isShown = useRef(true);
	useEffect(() => {
		isShown.current = true;
		return () => {
			isShown.current = false;
		};
	}, []);
	useEffect(() => {
		if (matterId == null) return;
		const isSameMatter = lastRead.current.matterId == matterId;
		if (isSameMatter && ((members != null && unknown == "") || Date.now() - lastRead.current.at < 10_000)) return;
		lastRead.current = { matterId, at: Date.now() };
		nativeApp().getSharedActivity(matterId).then(activity => {
			if (isShown.current == false) return;
			setRead({ matterId, members: activity?.members ?? [] });
		}, () => {});
	}, [matterId, members, unknown, firm.version]);
	if (firm.isRead != true || matterId == null) return null;
	return { link, devices, owners };
}

// The firm's folder, chosen on this computer; back to the dialog that asked.
function FolderStep({ matterName, onCancel, onChosen }) {
	const [isChoosing, setChoosing] = useState(false);
	const [error, setError] = useState(null);

	async function choose() {
		setChoosing(true);
		setError(null);
		try {
			if ((await chooseFirmFolder()) == true) onChosen?.();
		} catch (reason) {
			setError(reason?.message ?? "That folder cannot be used. Choose one of the firm's storage that this computer can change.");
		}
		setChoosing(false);
	}

	return (
		<Dialog
			title="Choose your firm's folder"
			description={`Your firm's computers exchange ${matterName ?? "their Matters"} through a folder your firm keeps in step on each of them: in OneDrive or SharePoint, Dropbox, Google Drive, iCloud Drive or a shared drive.`}
			onClose={onCancel}
			actions={(
				<>
					<Button onClick={onCancel}>Cancel</Button>
					<Button variant="primary" busy={isChoosing} onClick={choose}>Choose the folder</Button>
				</>
			)}
		>
			<p className={styles.note}>All this computer puts there is sealed: only the firm's computers that share a Matter can open it, and none of it passes through Legal Machines.</p>
			{error != null ? <p className={styles.error}>{error}</p> : null}
		</Dialog>
	);
}

// Invites a colleague to a Matter open here: the line they paste on their own
// computer. Nothing in it is secret; their computer comes in only by the
// codes the two lawyers compare.
export function InviteDialog({ projectId, matterName, onClose }) {
	const firm = useFirmComputers();
	const [invitation, setInvitation] = useState("");
	const [error, setError] = useState(null);
	const [isChoosingFolder, setChoosingFolder] = useState(false);

	useEffect(() => {
		if (invitation != "" || error != null) return;
		invitationFor(projectId)
			.then(setInvitation)
			.catch(reason => setError(reason?.message ?? "The invitation could not be made."));
	}, [projectId]);

	if (isChoosingFolder) return <FolderStep matterName={matterName} onCancel={() => setChoosingFolder(false)} onChosen={() => setChoosingFolder(false)} />;

	async function copy() {
		try {
			await navigator.clipboard.writeText(invitation);
			toast("The invitation is copied. Send it to your colleague.");
		} catch {
			toast("It could not be copied. Select it and copy it yourself.", { tone: "seal" });
		}
	}

	return (
		<Dialog
			title={`Invite a colleague to ${matterName}`}
			description="Send your colleague this invitation. On their computer, they open Matters, choose Join with an invitation and paste it."
			onClose={onClose}
			actions={(
				<>
					<Button onClick={onClose}>Close</Button>
					<Button variant="primary" icon="copy" disabled={invitation == ""} onClick={copy}>Copy the invitation</Button>
				</>
			)}
		>
			<Field label="Invitation" error={error ?? undefined}>
				<Input readOnly value={invitation} placeholder={error == null ? "Making the invitation" : ""} spellCheck={false} onFocus={event => event.target.select()} />
			</Field>
			<p className={styles.note}>When their computer asks to join, {matterName} shows it on its page with its code. Let it in only if your colleague reads you the same code from their screen.</p>
			{firm.deviceCode != "" ? <p className={styles.note}>This computer's code is <strong className={styles.code}>{firm.deviceCode}</strong>. Your colleague checks it before they take {matterName}.</p> : null}
			{firm.isRead == true && firm.firmFolder == "" ? (
				<SecondWay words={`Their computer calls this one directly, over the network. Where it cannot reach it, a folder your firm keeps in step can carry ${matterName} between them.`} onChoose={() => setChoosingFolder(true)} />
			) : null}
		</Dialog>
	);
}

// The firm's folder offered as the second way, beside the direct link.
function SecondWay({ words, onChoose }) {
	return (
		<div className={styles.second_way}>
			<p className={styles.note}>{words}</p>
			<Button size="small" onClick={onChoose}>Choose the firm's folder</Button>
		</div>
	);
}

// Asks, by a colleague's invitation, to bring their Matter to this computer.
export function JoinDialog({ onClose }) {
	const firm = useFirmComputers();
	const [text, setText] = useState("");
	const [error, setError] = useState(null);
	const [isAsking, setAsking] = useState(false);
	const [joined, setJoined] = useState(null);
	const [isChoosingFolder, setChoosingFolder] = useState(false);

	if (isChoosingFolder) return <FolderStep onCancel={() => setChoosingFolder(false)} onChosen={() => setChoosingFolder(false)} />;

	async function ask() {
		if (text.trim() == "" || isAsking) return;
		setAsking(true);
		setError(null);
		const before = new Set(firm.matters.map(matter => matter.matterId));
		try {
			const status = await joinWithInvitation(text);
			const matters = status?.matters ?? [];
			const added = matters.find(matter => before.has(matter.matterId) == false) ?? matters.find(matter => matter.state == "waiting");
			setJoined(added?.name ?? "the Matter");
		} catch (reason) {
			setError(reason?.message ?? "The invitation could not be read. Copy it whole and paste it again.");
		}
		setAsking(false);
	}

	if (joined != null) {
		return (
			<Dialog
				title={`Asked to join ${joined}`}
				description="Your colleague lets this computer in once they have compared its code with you."
				onClose={onClose}
				actions={<Button variant="primary" onClick={onClose}>Done</Button>}
			>
				<p className={styles.note}>Read them this computer's code: <strong className={styles.code}>{firm.deviceCode}</strong>.</p>
				<p className={styles.note}>Once they let it in, {joined} shows in Matters with their computer's code. Take it only if it is the code they read you.</p>
			</Dialog>
		);
	}

	return (
		<Dialog
			title="Join a Matter with an invitation"
			description="Paste the invitation your colleague sent you."
			initialFocus="input"
			onClose={onClose}
			actions={(
				<>
					<Button onClick={onClose}>Cancel</Button>
					<Button variant="primary" busy={isAsking} disabled={text.trim() == ""} onClick={ask}>Ask to join</Button>
				</>
			)}
		>
			<Field label="Invitation" error={error ?? undefined} hint="Your colleague copies it from the Matter's page, under Invite.">
				<Input
					value={text}
					spellCheck={false}
					autoComplete="off"
					onChange={event => {
						setText(event.target.value);
						setError(null);
					}}
					onKeyDown={event => {
						if (event.key == "Enter") ask();
					}}
				/>
			</Field>
			{firm.isRead == true && firm.firmFolder == "" ? (
				<SecondWay words="This computer calls your colleague's directly, over the network. Where it cannot reach it, choose the folder your firm keeps in step first." onChoose={() => setChoosingFolder(true)} />
			) : null}
		</Dialog>
	);
}

// The computers that ask to join a Matter this computer carries, for every
// Matter: the lawyer compares each one's code with the colleague, then lets it
// in or says it is not theirs. The Matter the window shows has them on its
// page too.
export function AskingToJoin() {
	const firm = useFirmComputers();
	if (firm.asking.length == 0) return null;
	return (
		<Tray icon="key" label="Asks to join" count={firm.asking.length}>
			<Rows>
				{firm.asking.map(computer => <AskingRow key={`${computer.matterId}:${computer.deviceId}`} computer={computer} />)}
			</Rows>
		</Tray>
	);
}

function AskingRow({ computer }) {
	const [isDeciding, setDeciding] = useState(false);
	const first = computer.name.split(/\s+/)[0] ?? "";
	const isNamed = computer.isYou == false && computer.name != "";
	const matterName = computer.matterName;
	const title = computer.isYou ? `Your other computer asks to join ${matterName}`
		: isNamed ? `${computer.name} asks to work on ${matterName} from their computer`
		: `A computer asks to join ${matterName}`;
	const code = <strong className={styles.code}>{computer.code}</strong>;
	const line = computer.isYou ? <>Its code is {code}. Let it in only if your other computer shows the same code.</>
		: isNamed ? <>Its code is {code}. Let it in only if {first} reads you the same code from their screen.</>
		: <>Its code is {code}. Let it in only if the colleague who asked reads you the same code.</>;

	async function decide(isTheirs) {
		setDeciding(true);
		try {
			if (isTheirs) {
				await letComputerIn(computer);
				toast(`${matterName} is on its way to ${computer.isYou ? "your other computer" : isNamed ? `${first}'s computer` : "that computer"}.`);
			} else {
				await refuseComputer(computer);
				toast(`That computer stays out of ${matterName}.`);
			}
		} catch (error) {
			toast(error?.message ?? "It could not be decided. Try again.", { tone: "seal" });
		}
		setDeciding(false);
	}

	return (
		<Row
			lead={<span className={styles.row_icon}><Icon name="key" size={18} /></span>}
			title={title}
			line={line}
			aside={(
				<>
					<Button size="small" disabled={isDeciding} onClick={() => decide(false)}>{computer.isYou ? "Not mine" : isNamed ? `Not ${first}'s` : "Not theirs"}</Button>
					<Button size="small" variant="primary" disabled={isDeciding} onClick={() => decide(true)}>Let it in</Button>
				</>
			)}
		/>
	);
}

// The Matters this computer asked to join: waiting to be let in, offered by a
// computer whose code the lawyer compares, or arriving.
export function AskedToJoin() {
	const firm = useFirmComputers();
	const rows = [];
	for (const matter of firm.matters) {
		if (matter.state == "waiting" && (matter.offering ?? []).length > 0) {
			for (const offer of matter.offering) rows.push({ key: `${matter.matterId}:${offer.deviceId}`, matter, offer });
		} else if (matter.state == "waiting" || isArriving(matter.matterId)) {
			rows.push({ key: matter.matterId, matter, offer: null });
		}
	}
	if (rows.length == 0) return null;
	return (
		<Tray icon="key" label="Asked to join" count={rows.length}>
			<Rows>
				{rows.map(row => <AskedRow key={row.key} matter={row.matter} offer={row.offer} deviceCode={firm.deviceCode} />)}
			</Rows>
		</Tray>
	);
}

function AskedRow({ matter, offer, deviceCode }) {
	const [isTaking, setTaking] = useState(false);
	const lead = <span className={styles.row_icon}><Icon name="matter" size={18} /></span>;

	if (matter.state == "carried") {
		return <Row lead={lead} title={matter.name} line="Arriving: its documents and records come over the next minutes." aside={<Chip tone="work">Arriving</Chip>} />;
	}
	if (offer == null) {
		const line = matter.lastError != "" && matter.lastError != null
			? matter.lastError
			: `Waiting for your colleague to let this computer in. Read them its code: ${deviceCode}.`;
		return <Row lead={lead} title={matter.name} line={line} aside={<Chip>Waiting</Chip>} />;
	}

	async function take() {
		setTaking(true);
		try {
			await takeMatter(matter.matterId, offer.deviceId);
			toast(`${matter.name} is on its way to this computer.`);
		} catch (reason) {
			toast(reason?.message ?? `${matter.name} could not be taken.`, { tone: "seal" });
		}
		setTaking(false);
	}

	return (
		<Row
			lead={lead}
			title={matter.name}
			line={<>A computer offers it, with the code <strong className={styles.code}>{offer.code}</strong>. Take it if your colleague reads you the same code.</>}
			aside={<Button size="small" variant="primary" busy={isTaking} onClick={take}>Take {matter.name}</Button>}
		/>
	);
}

// ---- The firm's computers, checked once by a number both show -------

// The dialogs about the firm's computers the window shows wherever the lawyer
// is: a check of one computer, or a Matter given to this member.
export function FirmDialogs() {
	const firm = useFirmComputers();
	const dialog = firm.dialog;
	if (dialog?.kind == "check") return <ComputerCheckDialog key={dialog.deviceId} deviceId={dialog.deviceId} />;
	if (dialog?.kind == "offer") return <OfferDialog key={`${dialog.matterId}:${dialog.fromDeviceId}`} matterId={dialog.matterId} fromDeviceId={dialog.fromDeviceId} />;
	return null;
}

function capitalised(text) {
	return text.length == 0 ? text : text[0].toUpperCase() + text.slice(1);
}

// One check with one computer of the firm: both computers show the same eight
// digits only when nothing stands between them, so the two lawyers compare
// them once, side by side or on a call, and each says whether they match.
// From then on the computers trust each other, and what the lawyers give each
// other travels sealed, with nothing to read aloud.
function ComputerCheckDialog({ deviceId }) {
	const firm = useFirmComputers();
	const computer = firm.computers.find(entry => entry.deviceId == deviceId);
	const owner = computerOwner(computer);
	// A computer of the member's own seat: they compare the number on their two
	// computers themselves.
	const isYours = owner.isYours == true;
	// One that never answers - wiped, lost, or set up again since - is turned
	// away without the number: turning a computer away opens nothing to anyone,
	// so it needs no check, while letting one in still does.
	const notTheirs = isYours ? "Not mine" : owner.member != null ? `Not ${owner.first}’s` : "Not theirs";
	const lastHeard = (computer?.announcedAt ?? "") != "" ? `It last said where it is ${formatWhen(computer.announcedAt)}.` : "";
	const [check, setCheck] = useState({ state: "waiting", number: "", problem: "" });
	const [attempt, setAttempt] = useState(0);
	const [isDeciding, setDeciding] = useState(false);
	const isDecided = useRef(false);

	useEffect(() => {
		let isShown = true;
		let timer = null;
		const step = async isFirst => {
			try {
				const next = isFirst ? await beginComputerCheck(deviceId) : await readComputerCheck(deviceId);
				if (isShown == false) return;
				setCheck(next);
				if (next.state == "waiting") timer = setTimeout(() => step(false), 1200);
			} catch (error) {
				if (isShown) setCheck({ state: "failed", number: "", problem: error?.message ?? "The check could not begin. Try again." });
			}
		};
		step(true);
		return () => {
			isShown = false;
			clearTimeout(timer);
		};
	}, [deviceId, attempt]);

	function close() {
		if (isDecided.current == false) endComputerCheck(deviceId);
		closeFirmDialog();
	}

	async function decide(isSame) {
		setDeciding(true);
		try {
			if (isSame) {
				await keepComputer(deviceId);
				toast(isYours
					? "Your other computer is checked. Your firm’s work reaches it sealed."
					: `${capitalised(owner.name)}’s computer is checked. What you give each other goes to it sealed.`);
			} else {
				await refuseFirmComputer(deviceId);
				toast("That computer stays out. Nothing of the firm’s goes to it.");
			}
			isDecided.current = true;
			closeFirmDialog();
		} catch (error) {
			toast(error?.message ?? "It could not be decided. Try again.", { tone: "seal" });
		}
		setDeciding(false);
	}

	const actions = check.state == "ready" ? (
		<>
			<Button disabled={isDeciding} onClick={() => decide(false)}>They don’t match</Button>
			<Button variant="primary" busy={isDeciding} onClick={() => decide(true)}>They match</Button>
		</>
	) : check.state == "failed" ? (
		<>
			<Button disabled={isDeciding} onClick={() => decide(false)}>{notTheirs}</Button>
			<Button onClick={close}>Close</Button>
			<Button variant="primary" onClick={() => setAttempt(attempt + 1)}>Try again</Button>
		</>
	) : (
		<>
			<Button disabled={isDeciding} onClick={() => decide(false)}>{notTheirs}</Button>
			<Button onClick={close}>Cancel</Button>
		</>
	);

	return (
		<Dialog
			layout="spacious"
			title={isYours ? "Check your other computer" : `Check ${owner.name}’s computer`}
			description={isYours
				? "Compare the number on your two computers. They show the same one only when nothing stands between them."
				: `Compare the number with ${owner.first}, side by side or on a call. Your two computers show the same one only when nothing stands between them.`}
			onClose={close}
			actions={actions}
		>
			{check.state == "ready" ? (
				<div className={styles.check}>
					<p className={styles.number} aria-label={check.number.split("").join(" ")}>{check.number}</p>
					<p className={styles.note}>{isYours ? "Does your other computer show the same number?" : `Does ${owner.first} see the same number?`}</p>
				</div>
			) : check.state == "failed" ? (
				<p className={styles.problem}>{check.problem}</p>
			) : (
				<div className={styles.check}>
					<p className={styles.number} data-waiting="" aria-hidden="true">•••• ••••</p>
					<p className={styles.note}>{isYours
						? "Waiting for your other computer. Choose Check there too, and the number shows on both."
						: `Waiting for ${owner.first}. Their computer asks them to check with you, and the number shows on both once they do.`}</p>
					{lastHeard != "" ? <p className={styles.note}>{lastHeard} If it is gone, choose {notTheirs}.</p> : null}
				</div>
			)}
		</Dialog>
	);
}

// A Matter a lawyer of the firm gave this member: who gives it, and how much
// of it comes, and Accept takes it into a folder of its own.
function OfferDialog({ matterId, fromDeviceId }) {
	const firm = useFirmComputers();
	const offer = firm.offers.find(entry => entry.matterId == matterId && entry.fromDeviceId == fromDeviceId);
	const owner = computerOwner(firm.computers.find(entry => entry.deviceId == fromDeviceId));
	const [isAnswering, setAnswering] = useState(false);

	if (offer == null) {
		return (
			<Dialog layout="spacious" title="This Matter is no longer offered here" description="It may have come already, or it was taken back." onClose={closeFirmDialog}
				actions={<Button variant="primary" onClick={closeFirmDialog}>Close</Button>} />
		);
	}

	async function answer(isTaken) {
		setAnswering(true);
		try {
			if (isTaken) {
				await acceptGivenMatter(offer);
				toast(`${offer.name} is on its way to this computer.`);
			} else {
				await declineGivenMatter(offer);
				toast(`${offer.name} stays with ${owner.name}. They can share it with you again.`);
			}
			closeFirmDialog();
		} catch (error) {
			toast(error?.message ?? "It could not be answered. Try again.", { tone: "seal" });
		}
		setAnswering(false);
	}

	return (
		<Dialog
			layout="spacious"
			title={`${capitalised(owner.name)} shares ${offer.name} with you`}
			description={`It comes sealed from ${owner.first}’s computer, which you checked together, into a folder of its own.`}
			onClose={closeFirmDialog}
			actions={(
				<>
					<Button disabled={isAnswering} onClick={() => answer(false)}>Not now</Button>
					<Button variant="primary" busy={isAnswering} onClick={() => answer(true)}>Accept</Button>
				</>
			)}
		>
			<p className={styles.size}>{offer.files == 1 ? "1 file" : `${offer.files} files`}, {sizeWords(offer.bytes)}</p>
		</Dialog>
	);
}

// The Matters given to this member that wait for an answer, on Matters.
export function GivenToYou() {
	const firm = useFirmComputers();
	if (firm.offers.length == 0) return null;
	return (
		<Tray icon="matter" label="Shared with you" count={firm.offers.length}>
			<Rows>
				{firm.offers.map(offer => (
					<GivenRow key={`${offer.matterId}:${offer.fromDeviceId}`} offer={offer} owner={computerOwner(firm.computers.find(entry => entry.deviceId == offer.fromDeviceId))} />
				))}
			</Rows>
		</Tray>
	);
}

function GivenRow({ offer, owner }) {
	const [isAnswering, setAnswering] = useState(false);

	async function answer(isTaken) {
		setAnswering(true);
		try {
			if (isTaken) {
				await acceptGivenMatter(offer);
				toast(`${offer.name} is on its way to this computer.`);
			} else {
				await declineGivenMatter(offer);
			}
		} catch (error) {
			toast(error?.message ?? "It could not be answered. Try again.", { tone: "seal" });
		}
		setAnswering(false);
	}

	return (
		<Row
			lead={owner.member != null ? <Avatar member={owner.member} size={36} /> : <span className={styles.row_icon}><Icon name="matter" size={18} /></span>}
			title={offer.name}
			line={`From ${owner.name}, ${sizeWords(offer.bytes)}`}
			aside={(
				<>
					<Button size="small" disabled={isAnswering} onClick={() => answer(false)}>Not now</Button>
					<Button size="small" variant="primary" busy={isAnswering} onClick={() => answer(true)}>Accept</Button>
				</>
			)}
		/>
	);
}

// A Matter given to a lawyer of the firm by choosing them: the firm's lawyers
// in their seats, each with whether their computer is checked here. One whose
// computer is not checked yet is checked together first; the Matter waits for
// them in the meantime.
export function GiveDialog({ projectId, matterName, memberIds = [], invitedIds = [], onClose }) {
	const firm = useFirmComputers();
	const [chosen, setChosen] = useState(null);
	const [isGiving, setGiving] = useState(false);
	const [waitsFor, setWaitsFor] = useState(null);
	const [isInviting, setInviting] = useState(false);

	if (isInviting) return <InviteDialog projectId={projectId} matterName={matterName} onClose={onClose} />;

	const lawyers = teamMembers().filter(member => member.kind != "ai_colleague" && member.id != viewerId
		&& (member.actorId ?? "").startsWith("seat:"));
	const computersOf = member => firm.computers.filter(computer => computer.actorId == member.actorId && computer.state != "this");

	if (waitsFor != null) {
		const first = waitsFor.name.split(/\s+/)[0];
		const toCheck = computersOf(waitsFor).filter(computer => computer.state == "new");
		return (
			<Dialog
				layout="spacious"
				title={`Check ${first}’s computer first`}
				description={`${matterName} waits for ${first}. It reaches their computer once you have checked it together, and they accept it there.`}
				onClose={onClose}
				actions={(
					<>
						<Button onClick={onClose}>Later</Button>
						{toCheck.length > 0 ? (
							<Button variant="primary" onClick={() => {
								onClose();
								openFirmDialog({ kind: "check", deviceId: toCheck[0].deviceId });
							}}>Check now</Button>
						) : null}
					</>
				)}
			>
				{toCheck.length == 0 ? <p className={styles.note}>{first} has not signed in on a computer yet. Once they do, it shows here to check.</p> : null}
			</Dialog>
		);
	}

	async function give() {
		if (chosen == null || isGiving) return;
		setGiving(true);
		try {
			const given = await giveMatter(projectId, chosen.actorId);
			if (given?.reaches == true) {
				toast(`${matterName} is on its way to ${chosen.name.split(/\s+/)[0]}. Their computer asks them to accept it.`);
				onClose();
			} else {
				setWaitsFor(chosen);
			}
		} catch (error) {
			toast(error?.message ?? `${matterName} could not be shared. Try again.`, { tone: "seal" });
		}
		setGiving(false);
	}

	return (
		<Dialog
			layout="spacious"
			title={`Share ${matterName} with a colleague`}
			description="Choose the lawyer. Their computer asks them to accept it, and it travels sealed to them alone."
			onClose={onClose}
			actions={(
				<>
					<Button onClick={onClose}>Cancel</Button>
					<Button variant="primary" busy={isGiving} disabled={chosen == null} onClick={give}>{chosen == null ? "Share" : `Share with ${chosen.name.split(/\s+/)[0]}`}</Button>
				</>
			)}
		>
			{lawyers.length == 0 ? (
				<p className={styles.note}>No other lawyer of the firm has a seat yet. The administrator adds one in Team.</p>
			) : (
				<Rows label="The firm's lawyers">
					{lawyers.map(member => {
						const theirs = computersOf(member);
						// One on it already is not offered it again; one invited
						// may be, and is told so.
						const isInvited = invitedIds.includes(member.id);
						const isOn = isInvited == false && memberIds.includes(member.id);
						const line = isOn ? "On the Matter"
							: isInvited ? "Invited. Their computer asks them to accept it"
							: theirs.some(computer => computer.state == "checked") ? "Their computer is checked"
							: theirs.some(computer => computer.state == "new") ? "You check their computer together first"
							: "Not signed in on a computer yet";
						return (
							<Row
								key={member.id}
								lead={<Avatar member={member} size={36} />}
								title={member.name}
								line={line}
								isSelected={chosen?.id == member.id}
								onOpen={isOn ? undefined : () => setChosen(member)}
							/>
						);
					})}
				</Rows>
			)}
			<button className={styles.other_way} type="button" onClick={() => setInviting(true)}>Copy an invitation instead</button>
		</Dialog>
	);
}
