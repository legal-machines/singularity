import { useSyncExternalStore } from "react";

import { addressOf } from "../components/Navigation/navigation.js";
import { noteOnline } from "../components/Avatar/online.js";
import { dropNotice, endNotice, notice, toast } from "../components/Toast/toast.js";
import { connectRealWork, hasNative, listArrivedMatter, lookForArrivedMatters, matterNameOf, nativeApp, noteConnectedComputers, onMatterGiven, realWork, shownMembers } from "./native.js";
import { viewerMay } from "./workspace.js";
import { endingOf, isWhole, transferNotices } from "./transfers.js";

// The firm's computers (D-123, D-124), as this computer's coordination says:
// the firm's folder it exchanges through, its own code, the Matters it carries
// or waits for, the computers that ask to join a Matter carried here and the
// ones that offer a Matter this computer asked for. Read from the backend
// again every few seconds while the window shows, since a request or an offer
// arrives on the engine's own rounds. Nothing here is a second truth (ARCH-2):
// it is what the backend last said.

// Every few seconds while the window shows; less often while it is minimized
// or behind others, so what waits is there when the lawyer comes back to it.
const EVERY_MILLISECONDS = 3000;
const HIDDEN_EVERY_MILLISECONDS = 15000;

// And the firm's computers (D-205): each with its seat and whether a lawyer
// here checked it, the ones that began a check with this one, the Matters
// given to this member that wait for an answer, and the dialog about them the
// window shows, if any. Read with the rest, as the backend last heard them:
// the window never makes it ask Legal Machines.
let current = {
	isRead: false, firmFolder: "", deviceCode: "", matters: [], asking: [], error: null, version: 0,
	computers: [], checkAsking: [], offers: [], dialog: null,
};
const listeners = new Set();
let timer = null;
let isReading = false;
// Matters this computer took the key of in this session, by when: they arrive
// in the lawyer's folder over the next rounds, where the Matter's page finds
// them. After a quarter of an hour the window stops looking; the Matter is
// still found whenever the Matters are read again.
const arriving = new Map();
const ARRIVAL_MILLISECONDS = 15 * 60 * 1000;
// Offers already said in a toast, by Matter and computer.
const told = new Set();
// What goes on between this computer and the firm's others, said while it goes
// on (D-207): the notices shown, by key; the transfers still going at the last
// reading; the Matters this window asked to join lately, by when; each
// Matter's computers, by device, with their members, read again now and then;
// and the Matters whose arrival a notice already said.
const shownNotices = new Map();
let watchedTransfers = new Set();
const askedHere = new Map();
const ASKED_MILLISECONDS = 60 * 60 * 1000;
const computersByMatter = new Map();
const NAMES_MILLISECONDS = 30 * 1000;
const arrivalSaid = new Set();
// The Matters the member accepted here (D-205), by Matter: what the offer said
// of each - its name, bytes and files, the computer it comes from and that
// computer's member - and when, so its notice stands from the acceptance
// until it is whole here (the owner, 2026-11-02: a Matter came in half a
// second, and no notice said how much had come). And the Matters given from
// this window whose first transfer is not said yet; and each Matter taken
// here being listed among the member's Matters, once.
const accepted = new Map();
const givenHere = new Set();
const listing = new Map();
// Read often while something is on its way or waited for, so its notice moves
// with it.
const BUSY_MILLISECONDS = 700;

function publish(next) {
	current = { ...current, ...next, version: current.version + 1 };
	for (const listener of listeners) listener();
}

function subscribe(listener) {
	listeners.add(listener);
	return () => listeners.delete(listener);
}

export function useFirmComputers() {
	return useSyncExternalStore(subscribe, () => current);
}

export function firmComputers() {
	return current;
}

export function isFirmCoordination() {
	return hasNative() && typeof nativeApp().getCoordination == "function";
}

// When to read again: often while a Matter is on its way or this computer waits
// for another (D-207), else every few seconds, less while the window is away.
function nextReadIn() {
	if (shownNotices.size > 0 || watchedTransfers.size > 0 || accepted.size > 0 || arriving.size > 0) return BUSY_MILLISECONDS;
	return document.visibilityState == "visible" ? EVERY_MILLISECONDS : HIDDEN_EVERY_MILLISECONDS;
}

// Begins reading, once; safe to call again.
export function watchFirmComputers() {
	if (isFirmCoordination() == false || timer != null) return;
	const again = async () => {
		await read();
		timer = setTimeout(again, nextReadIn());
	};
	timer = setTimeout(again, 0);
	// A Matter given by the new Matter's dialog, as one given by Share.
	onMatterGiven(matterId => givenHere.add(matterId));
	window.addEventListener("focus", () => read());
	document.addEventListener("visibilitychange", () => {
		if (document.visibilityState == "visible") read();
	});
}

async function read() {
	if (isReading == true) return;
	isReading = true;
	try {
		const status = await nativeApp().getCoordination();
		const matters = status?.matters ?? [];
		const asking = await askingHere(matters);
		noteConnectedComputers(matters);
		const firm = await firmRead();
		publish({ isRead: true, firmFolder: status?.firmFolder ?? "", deviceCode: status?.deviceCode ?? "", matters, asking, error: null, ...firm });
		// Whose computer is connected now, by seat: a green dot on their faces.
		const connected = new Set(matters.flatMap(matter => matter.connected ?? []));
		noteOnline((firm.computers ?? current.computers ?? []).filter(computer => connected.has(computer.deviceId)).map(computer => computer.actorId));
		tellChecks(firm.computers, firm.checkAsking);
		tellGiven(firm.computers, firm.offers);
		tellOffers(matters);
		tellLinkProblems(matters);
		await tellTransfers(matters, status?.deviceCode ?? "");
		await lookForArrivals(matters);
		listFound(matters);
		await bindCarried(matters);
	} catch (error) {
		publish({ isRead: true, error: error?.message ?? String(error) });
	}
	isReading = false;
}

// The computers that ask to join any Matter carried here, each with its
// member, as that Matter's activity names them (project.sharedActivity:
// "asking") - not only the Matter the window shows (2026-10-26: a request to
// join Halden showed nowhere).
async function askingHere(matters) {
	const found = [];
	for (const link of matters) {
		if ((link.asking ?? []).length == 0) continue;
		const activity = await nativeApp().getSharedActivity(link.matterId);
		for (const member of activity?.members ?? []) {
			for (const computer of member.computers ?? []) {
				if (computer.state != "asking") continue;
				const asked = link.asking.find(entry => entry.deviceId == computer.deviceId);
				found.push({
					matterId: link.matterId,
					matterName: link.name,
					actorId: member.actorId,
					name: (member.name ?? "").trim(),
					isYou: member.isYou == true,
					deviceId: computer.deviceId,
					code: computer.code,
					at: asked?.at ?? "",
				});
			}
		}
	}
	return found;
}

// The firm's computers and the Matters given to this member (D-205); the
// offers read every few readings, and only once a computer of the firm is
// checked here, since none comes from any other.
const OFFERS_MILLISECONDS = 6000;
let offersReadAt = 0;

async function firmRead() {
	const api = nativeApp();
	if (typeof api.getFirmComputers != "function") return {};
	const listed = await api.getFirmComputers().catch(() => null);
	const computers = listed?.computers ?? current.computers;
	const checkAsking = listed?.asking ?? current.checkAsking;
	let offers = current.offers;
	const isChecked = computers.some(computer => computer.state == "checked");
	if (isChecked == false) offers = [];
	else if (Date.now() - offersReadAt >= OFFERS_MILLISECONDS && typeof api.getMatterOffers == "function") {
		offersReadAt = Date.now();
		offers = (await api.getMatterOffers().catch(() => null))?.offers ?? offers;
	}
	return { computers, checkAsking, offers };
}

// Whose a computer of the firm is, as the window can say it: its member's name
// once the firm's record names who sits in its seat, else the seat's office -
// before two computers are checked, no name has reached this one (D-205).
export function computerOwner(computer) {
	const member = computer == null ? null : shownMembers().find(person => (person.actorId ?? "") == computer.actorId);
	if (member != null && (member.name ?? "") != "") return { name: member.name, first: member.name.split(/\s+/)[0], member };
	if (computer?.role == "administrator") return { name: "your firm’s administrator", first: "the administrator", member: null };
	return { name: "a colleague", first: "your colleague", member: null };
}

function possessive(owner) {
	return `${owner.name}’s`;
}

// A computer that began a check with this one is said at once, wherever the
// lawyer is, with the act that answers it; gone once it no longer asks.
const checkNotices = new Set();

function tellChecks(computers, asking) {
	const standing = new Set();
	for (const deviceId of asking ?? []) {
		if (current.dialog?.kind == "check" && current.dialog.deviceId == deviceId) continue;
		const computer = computers.find(entry => entry.deviceId == deviceId);
		const owner = computerOwner(computer);
		const key = `check:${deviceId}`;
		standing.add(key);
		checkNotices.add(key);
		notice(key, {
			text: `${sentence(possessive(owner))} computer asks to check with this one.`,
			tone: "waiting",
			action: { label: "Check", run: () => openFirmDialog({ kind: "check", deviceId }) },
		});
	}
	// The administrator's computer offers the check of a colleague's new
	// computer by itself, once it signs in (the owner, 2026-11-02: nothing on
	// the Mac said a check was to be made); a member's computer that no lawyer
	// checked yet says what it waits for.
	const isWaited = current.dialog?.kind == "check";
	for (const computer of computers ?? []) {
		if (computer.state != "new" || viewerMay.manageSeats != true || isWaited) continue;
		if ((asking ?? []).includes(computer.deviceId)) continue;
		const owner = computerOwner(computer);
		const key = `new:${computer.deviceId}`;
		standing.add(key);
		checkNotices.add(key);
		notice(key, {
			text: `${sentence(possessive(owner))} new computer waits for you to check it.`,
			tone: "waiting",
			action: { label: "Check", run: () => openFirmDialog({ kind: "check", deviceId: computer.deviceId }) },
		});
	}
	const isChecked = (computers ?? []).some(computer => computer.state == "checked");
	const hasOthers = (computers ?? []).some(computer => computer.state == "new");
	if (viewerMay.manageSeats != true && isChecked == false && hasOthers == true && (asking ?? []).length == 0 && isWaited == false) {
		const key = "waits-for-check";
		standing.add(key);
		checkNotices.add(key);
		notice(key, { text: "This computer waits for your firm’s administrator to check it with you.", tone: "waiting" });
	}
	for (const key of [...checkNotices]) {
		if (standing.has(key)) continue;
		checkNotices.delete(key);
		dropNotice(key);
	}
}

// A Matter given to this member is said once it is here to answer, with who
// gives it and how much of it comes; gone once answered.
const givenNotices = new Set();

function tellGiven(computers, offers) {
	const standing = new Set();
	for (const offer of offers ?? []) {
		if (current.dialog?.kind == "offer" && current.dialog.matterId == offer.matterId) continue;
		const owner = computerOwner(computers.find(entry => entry.deviceId == offer.fromDeviceId));
		const key = `given:${offer.matterId}:${offer.fromDeviceId}`;
		standing.add(key);
		givenNotices.add(key);
		notice(key, {
			text: `${sentence(owner.name)} shares ${offer.name || "a Matter"} with you.`,
			tone: "waiting",
			action: { label: "Open", run: () => openFirmDialog({ kind: "offer", matterId: offer.matterId, fromDeviceId: offer.fromDeviceId }) },
		});
	}
	for (const key of [...givenNotices]) {
		if (standing.has(key)) continue;
		givenNotices.delete(key);
		dropNotice(key);
	}
}

function sentence(text) {
	return text.length == 0 ? text : text[0].toUpperCase() + text.slice(1);
}

// The dialog about the firm's computers the window shows: a check of one
// computer, or a Matter given to this member.
export function openFirmDialog(dialog) {
	if (dialog?.kind == "check") {
		dropNotice(`check:${dialog.deviceId}`);
		dropNotice(`new:${dialog.deviceId}`);
	}
	if (dialog?.kind == "offer") dropNotice(`given:${dialog.matterId}:${dialog.fromDeviceId}`);
	publish({ dialog });
}

export function closeFirmDialog() {
	publish({ dialog: null });
	read();
}

// The check of one computer of the firm (D-205): begun or gone on with, read
// as it stands, and ended by the lawyer's word.
export function beginComputerCheck(deviceId) {
	return nativeApp().checkFirmComputer(deviceId);
}

export function readComputerCheck(deviceId) {
	return nativeApp().getComputerCheck(deviceId);
}

export async function keepComputer(deviceId) {
	await nativeApp().keepFirmComputer(deviceId);
	await read();
}

export async function refuseFirmComputer(deviceId) {
	await nativeApp().refuseFirmComputer(deviceId);
	await read();
}

export async function endComputerCheck(deviceId) {
	await nativeApp().endComputerCheck(deviceId).catch(() => null);
}

// A Matter given to a lawyer of the firm by choosing them; `reaches` says
// whether a computer of theirs is checked here, which the offer reaches now.
export async function giveMatter(projectId, actorId) {
	const given = await nativeApp().giveMatterTo(projectId, actorId);
	givenHere.add(projectId);
	offersReadAt = 0;
	await read();
	return given;
}

// The member takes a Matter given to them, or leaves it. Taken, the offer's
// notice goes at once, and the Matter's own stands from now until it is whole
// here, with how much of it has come (D-207).
export async function acceptGivenMatter(offer) {
	await nativeApp().acceptMatterOffer(offer.matterId, offer.fromDeviceId);
	arriving.set(offer.matterId, Date.now());
	const giver = computerOwner(current.computers.find(entry => entry.deviceId == offer.fromDeviceId));
	accepted.set(offer.matterId, { name: offer.name ?? "", bytes: offer.bytes ?? 0, files: offer.files ?? 0,
		fromDeviceId: offer.fromDeviceId, fromName: giver.name, at: Date.now() });
	const given = `given:${offer.matterId}:${offer.fromDeviceId}`;
	givenNotices.delete(given);
	dropNotice(given);
	publish({ offers: current.offers.filter(entry => entry.matterId != offer.matterId || entry.fromDeviceId != offer.fromDeviceId) });
	await tellTransfers(current.matters, current.deviceCode);
	offersReadAt = 0;
	await read();
}

export async function declineGivenMatter(offer) {
	await nativeApp().declineMatterOffer(offer.matterId, offer.fromDeviceId);
	offersReadAt = 0;
	await read();
}

// An offer this computer waits for is said once, wherever the lawyer is.
function tellOffers(matters) {
	for (const matter of matters) {
		if (matter.state != "waiting") continue;
		for (const offer of matter.offering ?? []) {
			const key = `${matter.matterId}:${offer.deviceId}`;
			if (told.has(key)) continue;
			told.add(key);
			// An offer that says how big the Matter is stands as its notice while
			// the Matter comes (D-207); only one that says nothing is told here.
			if ((offer.bytes ?? 0) > 0) continue;
			toast(`A computer offers ${matter.name}. Compare its code with your colleague in Matters, then take it.`);
		}
	}
}

// What goes on between this computer and the firm's others, said while it goes
// on (D-207): each notice the coordination's state calls for, shown or changed
// in place; each no longer called for, ended - said done when what it told of
// came whole, else gone unsaid. A Matter that arrived is listed at once.
async function tellTransfers(matters, deviceCode) {
	const names = new Map();
	for (const link of matters) {
		if (link.state != "carried" || ((link.receiving ?? []).length == 0 && (link.sending ?? []).length == 0)) continue;
		names.set(link.matterId, await computersOf(link.matterId));
	}
	for (const [matterId, at] of [...askedHere]) {
		if (Date.now() - at > ASKED_MILLISECONDS) askedHere.delete(matterId);
	}
	// One accepted that never came stops being waited for, as any arrival does.
	for (const [matterId, taken] of [...accepted]) {
		if (Date.now() - taken.at > ARRIVAL_MILLISECONDS) accepted.delete(matterId);
	}
	// Each Matter by the name the window shows now: the coordination keeps the
	// one it was shared under (the owner, 2026-11-02: a notice still named
	// Harbor "Project Harbor - Data Room").
	const named = matters.map(link => ({ ...link, name: matterNameOf(link.matterId) ?? link.name }));
	const said = transferNotices(named, { deviceCode, names, watched: watchedTransfers, askedHere: new Set(askedHere.keys()),
		accepted, givenHere });
	watchedTransfers = said.watched;
	const standing = new Set();
	for (const item of said.notices) {
		const matterId = item.key.split(":")[1];
		notice(item.key, { text: item.text, progress: item.progress, tone: item.tone });
		// A Matter given from here that went whole between two readings: said as
		// it went, and ended at once.
		if (item.isDone == true) {
			endNotice(item.key, { text: item.doneText });
			givenHere.delete(matterId);
			continue;
		}
		shownNotices.set(item.key, item);
		standing.add(item.key);
	}
	for (const [key, item] of [...shownNotices]) {
		if (standing.has(key)) continue;
		shownNotices.delete(key);
		const matterId = key.split(":")[1];
		if (item.kind == "out") givenHere.delete(matterId);
		if (endingOf(item, matters) != "done") {
			dropNotice(key);
			continue;
		}
		if (item.kind != "in") {
			endNotice(key, { text: item.doneText });
			continue;
		}
		arrivalSaid.add(matterId);
		const link = matters.find(entry => entry.matterId == matterId);
		// Taken here: listed among the member's Matters by itself, and opened
		// from its notice.
		if (accepted.delete(matterId) || arriving.has(matterId)) {
			const opening = listArrived(link);
			endNotice(key, { text: item.doneText, duration: 8000, action: { label: "Open", run: () => openArrived(opening) } });
			continue;
		}
		endNotice(key, { text: item.doneText });
		lookForArrivedMatters();
	}
}

// A Matter taken here and come whole, listed among the member's Matters once:
// the window's id for it, or null where it could not be listed - then it is
// still found among the arrivals, as one brought by the firm's computers, and
// tried again in a while.
const RELIST_MILLISECONDS = 30 * 1000;

function listArrived(link) {
	if (link == null) return Promise.resolve(null);
	if (listing.has(link.matterId) == false) {
		listing.set(link.matterId, listArrivedMatter(link.matterId, link.folder).catch(() => {
			lookForArrivedMatters();
			setTimeout(() => listing.delete(link.matterId), RELIST_MILLISECONDS);
			return null;
		}));
	}
	return listing.get(link.matterId);
}

// A Matter taken here that came while no window waited for it - before this
// window opened, or while Singularity was closed - is listed by itself too,
// once the folders' search finds it in its folder and nothing of it is still
// coming: it was taken here, so it is never left among the arrivals to open.
function listFound(matters) {
	for (const arrival of realWork().arrivals ?? []) {
		const link = matters.find(entry => entry.matterId == arrival.id && entry.state == "carried");
		if (link == null || (link.receiving ?? []).length > 0 || listing.has(link.matterId)) continue;
		listArrived(link);
	}
}

async function openArrived(opening) {
	const matterId = await opening;
	window.location.hash = matterId != null ? addressOf({ kind: "matter", matterId, tab: "overview" }) : addressOf({ kind: "matters" });
}

// A Matter's computers, by device, with their members' names, as its activity
// names them; read again now and then. One not open here yet names no one:
// its computers are said by their codes.
async function computersOf(matterId) {
	const known = computersByMatter.get(matterId);
	if (known != null && Date.now() - known.at < NAMES_MILLISECONDS) return known.byDevice;
	const byDevice = new Map();
	try {
		const activity = await nativeApp().getSharedActivity(matterId);
		for (const member of activity?.members ?? []) {
			for (const computer of member.computers ?? []) {
				if ((computer.deviceId ?? "") != "") byDevice.set(computer.deviceId, { name: (member.name ?? "").trim(), isYou: member.isYou == true });
			}
		}
	} catch {
		// Named by code instead.
	}
	computersByMatter.set(matterId, { at: Date.now(), byDevice });
	return byDevice;
}

// A Matter whose key was taken here is in the lawyer's folder once its records
// have come: it is listed among the member's Matters then, by itself (the
// owner, 2026-11-02: Matters did not show the Matter that had come).
async function lookForArrivals(matters) {
	if (arriving.size == 0) return;
	for (const [matterId, since] of [...arriving]) {
		if (Date.now() - since > ARRIVAL_MILLISECONDS) {
			arriving.delete(matterId);
			continue;
		}
		const link = matters.find(matter => matter.matterId == matterId);
		// Arrived once all of it is here, not when its key was taken (D-207).
		if (isWhole(link) == false) continue;
		arriving.delete(matterId);
		const opening = listArrived(link);
		// Said by the notice that showed it coming, else here.
		if (arrivalSaid.delete(matterId) == false) {
			toast(`${link.name} has arrived.`, { duration: 8000, action: { label: "Open", run: () => openArrived(opening) } });
		}
	}
}

// A Matter carried here while the window has none open - its key taken
// elsewhere than this window, the window opened before it came, or the Matter
// opened by itself once whole - is looked for again, at most every few
// seconds, so the window shows it without a reload (the owner, 2026-10-26: the
// Mac's window stayed empty after Amsterdam had come).
const REBIND_MILLISECONDS = 5000;
let lastRebind = 0;

async function bindCarried(matters) {
	if (["absent", "discoverable", "failed"].includes(realWork().status) == false) return;
	if (matters.some(matter => matter.state == "carried") == false) return;
	if (Date.now() - lastRebind < REBIND_MILLISECONDS) return;
	lastRebind = Date.now();
	await connectRealWork();
}

export function isArriving(matterId) {
	return arriving.has(matterId);
}

// Asks the lawyer for the firm's folder and sets it; false when none was chosen.
export async function chooseFirmFolder() {
	const folder = await nativeApp().chooseFolder();
	if (folder == null || folder == "") return false;
	await nativeApp().useFirmFolder(folder);
	await read();
	return true;
}

// The line a lawyer sends a colleague to bring the Matter to their computer.
// Asking for it carries the Matter to the firm's computers from now on.
export async function invitationFor(projectId) {
	const answer = await nativeApp().getMatterInvitation(projectId);
	await read();
	return answer?.invitation ?? "";
}

// The lawyer compared the asking computer's code with its colleague: it is
// confirmed as theirs and given the Matter's key (project.confirmComputer).
export async function letComputerIn(computer) {
	await nativeApp().confirmComputer(computer.matterId, computer.actorId, computer.deviceId);
	await read();
}

// The lawyer says the asking computer is not their colleague's.
export async function refuseComputer(computer) {
	await nativeApp().revokeComputer(computer.matterId, computer.actorId, computer.deviceId);
	await read();
}

// Asks, by a colleague's invitation, to bring their Matter to this computer.
export async function joinWithInvitation(invitation) {
	const waitingBefore = new Set(current.matters.filter(matter => matter.state == "waiting").map(matter => matter.matterId));
	const status = await nativeApp().joinFirmMatter(invitation.trim());
	// The Matter this window asked for: until it is let in, a notice says what
	// it waits for (D-207).
	for (const matter of status?.matters ?? []) {
		if (matter.state == "waiting" && waitingBefore.has(matter.matterId) == false) askedHere.set(matter.matterId, Date.now());
	}
	await read();
	return status;
}

// The lawyer compared the offering computer's code with the colleague who
// invited them: the Matter's key is taken, and the Matter arrives.
export async function takeMatter(matterId, deviceId) {
	await nativeApp().acceptMatterKey(matterId, deviceId);
	arriving.set(matterId, Date.now());
	await read();
}

// This module keeps the window's state. While the window runs it is never
// swapped for a fresh copy, which would start empty: a change to it reloads
// the window (2026-10-26: a hot swap left the Mac's window with no Matter, so
// it read nothing and every post was refused).
if (import.meta.hot) import.meta.hot.decline();

// A Matter's end-to-end link that went wrong is told once, wherever the member
// is, in its own words (the owner, 2026-11-02: "if there are problems, it
// should glow red, with an explanation and a notification"); the shield on
// the faces of its people says it too.
const toldProblems = new Set();
function tellLinkProblems(matters) {
	for (const matter of matters) {
		const problem = (matter.lastError ?? "").trim();
		if (problem == "") continue;
		const key = `${matter.matterId}:${problem}`;
		if (toldProblems.has(key)) continue;
		toldProblems.add(key);
		toast(`${matter.name ?? "A Matter"}: ${problem}`, { tone: "seal", duration: 12000 });
	}
}
