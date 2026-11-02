import { becomeViewer, inbox, matters, members, threads } from "../data/workspace.js";
import { DEMO_MEMBER } from "./emulation.js";

// MOCK DATA - the deal of onboarding's first page
// (onboarding/entry/conversation.js) in Marlowe Finch LLP's Amsterdam:
// Halvane Robotics buys 80% of Sablewood Distribution and the seller keeps
// 20%, fictional US companies under US law (AG-12, D-134). The member at the
// keyboard is Gabriel Moore, the partner on the deal, who gives Kitsune the
// deal. Only this frame's copy of the
// mock data changes; nothing is read from or written to the backend (PROD-2).

export const VIEWER = "m_gabriel";
export const DD_THREAD = "t_amsterdam_dd";
// Phoenix carries contract and IP evidence in the deal, as on the first page,
// where it comes from the Face catalogue.
export const PHOENIX = "entry_phoenix";

export function prepareWorld() {
	if (members.some((member) => member.id == PHOENIX)) return;
	becomeViewer(VIEWER);
	members.push({
		id: PHOENIX,
		kind: "ai_colleague",
		name: "Phoenix",
		species: "phoenix",
		role: "Lead of Deal Evidence",
		ranksWith: "Senior Associate",
		practice: "Mergers & Acquisitions",
		area: "Contract, IP and version evidence for the firm's deals.",
		duties: [
			{ text: "Checks every new data room document against the DD request list", when: "On every new document" },
			{ text: "Compares each signing set with the agreed versions", when: "Before every signing" },
		],
		remit: [
			{ title: "Decides alone", text: "Which evidence a finding rests on" },
			{ title: "Asks first", text: "Before a reply goes to the seller's counsel: the associate on it" },
			{ title: "Hands on", text: "A gap in title the seller must cure: the partner on the deal" },
		],
		record: "412 contracts reviewed in the last 90 days, 9 findings returned once.",
		model: "Runs on a frontier model through Papinian Guard.",
	});
	const cast = ["m_gabriel", "m_themis", "m_evelyn", PHOENIX, "m_noah", "m_amina", "m_juno", "m_teodor", "m_iris", "m_lena"];
	const amsterdam = matters.find((matter) => matter.id == "x_sablewood");
	for (const memberId of cast) {
		if (amsterdam != null && amsterdam.memberIds.includes(memberId) == false) amsterdam.memberIds.push(memberId);
	}
	threads.unshift(
		{ id: DD_THREAD, matterId: "x_sablewood", title: "Legal DD: scope and workstreams", participantIds: cast.filter((memberId) => memberId != "m_lena"), posts: [] },
	);
	inbox.unshift({ threadId: DD_THREAD, isUnread: false });
}

// The member at the keyboard of onboarding, in the firm as they introduce
// themselves on its first page (D-162): their name as they type it, their
// position, practice and photo. They join the team, and the work of the
// second page is theirs, given in their name. Until they type a name the
// firm is Gabriel's, as before.
export const YOU = DEMO_MEMBER;
let introduced = { name: "", position: "", practice: "", photo: null };

export function profileOf() {
	return introduced;
}

export function applyProfile(profile) {
	const words = value => (typeof value == "string" ? value.trim() : "");
	introduced = {
		name: words(profile?.name),
		position: words(profile?.position),
		practice: words(profile?.practice),
		photo: typeof profile?.photo == "string" && /^data:image\/(jpeg|png);base64,/.test(profile.photo) ? profile.photo : null,
	};
	const index = members.findIndex((member) => member.id == YOU);
	if (introduced.name == "") {
		if (index >= 0) members.splice(index, 1);
		becomeViewer(VIEWER);
		return;
	}
	const member = index >= 0 ? members[index] : { id: YOU, kind: "person", color: "#5b6170" };
	Object.assign(member, {
		name: introduced.name,
		initials: introduced.name.split(/\s+/).map((word) => word[0]).join("").slice(0, 2).toUpperCase(),
		photo: introduced.photo,
		position: introduced.position || undefined,
		role: introduced.position || "Joining the firm",
		practice: introduced.practice || null,
		// Before they choose a position they stand apart, as someone joining.
		joining: introduced.position == "",
	});
	if (index < 0) members.push(member);
	becomeViewer(YOU);
	const amsterdam = matters.find((matter) => matter.id == "x_sablewood");
	if (amsterdam != null && amsterdam.memberIds.includes(YOU) == false) amsterdam.memberIds.push(YOU);
	const thread = threads.find((item) => item.id == DD_THREAD);
	if (thread != null && thread.participantIds.includes(YOU) == false) thread.participantIds.unshift(YOU);
}

// The second page's thread is named for the work it carries.
export function titleWork(title) {
	const thread = threads.find((item) => item.id == DD_THREAD);
	if (thread != null && typeof title == "string" && title != "") thread.title = title;
}

// A file of the deal as the app shows one: its name from the screenplay, and
// a size and page count that stay the same on every showing.
export function fileOf(name) {
	let hash = 0;
	for (const character of name) hash = (hash * 31 + character.charCodeAt(0)) >>> 0;
	const kind = name.split(".").pop();
	if (kind == "pdf") return { name, pages: 2 + (hash % 36), size: `${120 + (hash % 1800)} KB` };
	if (kind == "docx") return { name, pages: 2 + (hash % 14), size: `${28 + (hash % 90)} KB` };
	if (kind == "xlsx") return { name, size: `${36 + (hash % 120)} KB` };
	return { name, size: `${12 + (hash % 48)} KB` };
}
