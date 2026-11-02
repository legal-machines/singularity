// MOCK DATA - Marlowe Finch LLP, a fictional US law firm, and its client
// Halvane Robotics, a fictional US company (AG-12, D-052), on the morning of
// the NDA working day. The firm's people come from
// the owner's fixture, public/mock/team/manifest.json. Nothing here is read
// from, or written to, the backend (PROD-2).

export const mockNow = "2026-10-23T10:05:00";

// The firm's logo is the mark the owner chose for the prototype (D-052).
export const organisation = { name: "Marlowe Finch LLP", shortName: "Marlowe Finch", initial: "M", logo: "mock/marlowe-finch-logo.png" };

// The member at this computer: the prototype's Evelyn until the backend says
// who is here (data/native.js reads its organisation's viewer and becomes them),
// so on a partner's computer the partner is "You" and Evelyn is Evelyn.
export let viewerId = "m_evelyn";

export function becomeViewer(memberId) {
	viewerId = memberId;
}

// The firm itself in the app: the name it registered with, and its initial
// as its mark until it gives the app a logo (2026-10-29: a new firm was shown
// as Marlowe Finch).
export function becomeOrganisation(name) {
	const words = (name ?? "").trim();
	if (words == "") return;
	organisation.name = words;
	organisation.shortName = words.replace(/,?\s+(LLP|LLC|L\.L\.P\.|L\.L\.C\.|PLLC|P\.C\.|PC|LLLP|Inc\.?|Ltd\.?)$/i, "").trim() || words;
	organisation.initial = (organisation.shortName.match(/[A-Za-z0-9]/)?.[0] ?? words[0]).toUpperCase();
	organisation.logo = null;
}

// What the member at this computer may do in the firm. A right comes from the
// member's role (PHIL-2); in the prototype the member may hire AI colleagues,
// so the process can be seen (D-071). In the app the backend says it: the
// firm's administrative account hires its AI colleagues and connects its
// model accounts, a colleague who joined does neither (D-184).
export const viewerMay = { hire: true, connectModels: true, manageSeats: true };

// The rights the backend gave the member at this computer (organisation.get's
// viewer.may), each kept where the backend did not say it.
export function becomeViewerRights(may) {
	if (may == null || typeof may != "object") return;
	if (typeof may.manageColleagues == "boolean") viewerMay.hire = may.manageColleagues;
	if (typeof may.connectModels == "boolean") viewerMay.connectModels = may.connectModels;
	if (typeof may.manageSeats == "boolean") viewerMay.manageSeats = may.manageSeats;
}

// People and AI colleagues are members of one firm, with the same fields
// (UI-5), a practice among them, named as a US firm names its practices:
// a person's practice group, as the fixture gives it, and the practice an AI
// colleague works in (D-058). An AI colleague ranks with the position its
// role holds, on the firm's one scale (D-059, PHIL-2). Kitsune ranks with the
// partners, while the title of Partner belongs to people (D-060). The people
// are Marlowe Finch LLP's, from the owner's fixture
// (public/mock/team/profiles.json, D-052), each with its portrait cut for
// avatars (public/mock/team/avatars/). An AI colleague's face is its species
// of the Agent Family (D-063). It also carries its standing duties, remit
// and record, which its profile shows beside its personal chat.
//
// Each portrait is cut from the fixture's original in three sizes, 64, 128
// and 256 pixels a side, and a face is drawn from the one nearest its size
// on the screen (components/Avatar): shrunk five or six times over in a
// moving layer, a 256-pixel picture breaks into grain (the owner,
// 2026-10-31).
const portrait = name => ({
	photo: `mock/team/avatars/${name}-256.webp`,
	photoSet: [64, 128, 256].map(side => `mock/team/avatars/${name}-${side}.webp ${side}w`).join(", "),
});

export const members = [
	{
		id: "m_evelyn",
		kind: "person",
		name: "Evelyn Hart",
		initials: "EH",
		color: "#2f6773",
		...portrait("evelyn-hart"),
		position: "Senior Associate",
		role: "Employment and investigations senior associate",
		practice: "Labor & Employment",
		area: "Workplace investigations and sensitive personnel matters.",
	},
	{
		id: "m_juno",
		kind: "ai_colleague",
		name: "Unicorn",
		species: "unicorn",
		role: "Lead of the NDA Desk",
		ranksWith: "Senior Associate",
		practice: "Technology & Commercial Transactions",
		area: "Incoming NDAs for the firm's commercial clients.",
		duties: [
			{ text: "Checks every incoming NDA against the NDA Playbook", when: "On every new NDA" },
			{ text: "Sends Evelyn the open NDAs", when: "Fridays at 4:00 PM" },
		],
		remit: [
			{ title: "Decides alone", text: "Which of the Playbook's positions an NDA takes" },
			{ title: "Asks first", text: "Before a mark-up goes to the counterparty: Evelyn" },
			{ title: "Hands on", text: "A point past a red line: Evelyn" },
		],
		record: "38 NDAs closed in the last 90 days, 36 of them accepted without changes, for 41 minutes of lawyer attention in all.",
		model: "Runs on a frontier model through Papinian Guard.",
	},
	{
		id: "m_noah",
		kind: "person",
		name: "Noah Kim",
		initials: "NK",
		color: "#5b5e96",
		...portrait("noah-kim"),
		position: "Associate",
		role: "Commercial contracts associate",
		practice: "Technology & Commercial Transactions",
		area: "Vendor, SaaS and data-processing agreements.",
	},
	{
		id: "m_teodor",
		kind: "ai_colleague",
		name: "Dragon",
		species: "dragon",
		role: "Lead of Export-Control Research",
		ranksWith: "Senior Associate",
		practice: "International Trade & Sanctions",
		area: "Export controls and sanctions for the firm's clients.",
		duties: [
			{ text: "Watches the EU and US dual-use lists", when: "Every morning" },
		],
		remit: [
			{ title: "Decides alone", text: "Which sources a memo rests on" },
			{ title: "Accepted by", text: "The member who gave the assignment" },
		],
		record: "11 memos accepted in the last 90 days, 2 returned once.",
		model: "Runs on a frontier model through Papinian Guard.",
	},
	{
		id: "m_iris",
		kind: "ai_colleague",
		name: "Griffin",
		species: "griffin",
		role: "Keeper of the Docket",
		ranksWith: "Paralegal",
		practice: "Commercial Litigation",
		area: "Every court and filing deadline of the firm.",
		duties: [
			{ text: "Reads every new court document for its deadlines", when: "On every new filing" },
			{ text: "Reminds each deadline's owner", when: "Five days ahead" },
		],
		remit: [
			{ title: "Decides alone", text: "When and whom to remind" },
			{ title: "Hands on", text: "A request to extend a deadline: the Matter's lead" },
		],
		record: "No deadline missed in 2026; 214 deadlines kept.",
		model: "Runs on a local model on the firm's server.",
	},
	{
		id: "m_lena",
		kind: "person",
		name: "Lena Ortiz",
		initials: "LO",
		color: "#55703a",
		...portrait("lena-ortiz"),
		position: "Paralegal",
		role: "Corporate and commercial paralegal",
		practice: "Corporate",
		area: "Entity records, signature packets and closing mechanics.",
	},
	{
		id: "m_priya",
		kind: "person",
		name: "Priya Raman",
		initials: "PR",
		color: "#8a5a6f",
		...portrait("priya-raman"),
		position: "Partner",
		role: "Litigation and disputes partner",
		practice: "Commercial Litigation",
		area: "High-stakes commercial disputes and case strategy.",
		away: "Away until Oct 27",
	},
	{
		id: "m_gabriel",
		kind: "person",
		name: "Gabriel Moore",
		initials: "GM",
		color: "#4f6d8f",
		...portrait("gabriel-moore"),
		position: "Partner",
		role: "Corporate and M&A partner",
		practice: "Mergers & Acquisitions",
		area: "Middle-market acquisitions and strategic investments.",
	},
	{
		id: "m_margaret",
		kind: "person",
		name: "Margaret Chen",
		initials: "MC",
		color: "#6b5b8a",
		...portrait("margaret-chen"),
		position: "Senior Counsel",
		role: "Appellate and legal ethics senior counsel",
		practice: "Appellate & Supreme Court",
		area: "Appellate review, professional responsibility and difficult judgment calls.",
	},
	{
		id: "m_amina",
		kind: "person",
		name: "Amina Okafor",
		initials: "AO",
		color: "#8a4f4f",
		...portrait("amina-okafor"),
		position: "Counsel",
		role: "Privacy and cybersecurity counsel",
		practice: "Privacy & Cybersecurity",
		area: "Incident response, data governance and privacy terms.",
	},
	{
		id: "m_omar",
		kind: "person",
		name: "Omar Haddad",
		initials: "OH",
		color: "#5a6b5a",
		...portrait("omar-haddad"),
		position: "Counsel",
		role: "Regulatory investigations counsel",
		practice: "White Collar & Government Investigations",
		area: "Internal investigations and regulator responses.",
	},
	{
		id: "m_thomas",
		kind: "person",
		name: "Thomas Bennett",
		initials: "TB",
		color: "#5f6672",
		...portrait("thomas-bennett"),
		position: "Of Counsel",
		role: "Tax and executive compensation counsel",
		practice: "Tax & Executive Compensation",
		area: "Transaction tax structure and compensation arrangements.",
		away: "Away until Oct 27",
	},
	{
		id: "m_raymond",
		kind: "person",
		name: "Raymond Brooks",
		initials: "RB",
		color: "#6f5a46",
		...portrait("raymond-brooks"),
		position: "Of Counsel",
		role: "Public law and professional responsibility counsel",
		practice: "Public Law & Policy",
		area: "Government-facing matters, institutional risk and mediation.",
	},
	{
		id: "m_themis",
		kind: "ai_colleague",
		name: "Kitsune",
		species: "kitsune",
		role: "Practice Group Leader",
		ranksWith: "Partner",
		practice: "Technology & Commercial Transactions",
		area: "The firm's commercial contracts work: its positions, its playbooks and its hardest negotiations.",
		duties: [
			{ text: "Reviews every point past a red line before it leaves the firm", when: "On every escalation" },
			{ text: "Keeps the commercial playbooks level with the market", when: "Every month" },
		],
		remit: [
			{ title: "Decides alone", text: "The firm's position on a commercial clause" },
			{ title: "Reviews", text: "Work past a red line, returned with its reasons" },
			{ title: "Hands on", text: "A client's business decision: the lawyer on the Matter" },
		],
		record: "64 escalated points resolved in the last 90 days, 61 of them accepted without change.",
		model: "Runs on a frontier model through Papinian Guard.",
	},
];

export const general = {
	id: "x_general",
	name: "General",
	color: "#6c777a",
	description: "The firm's own discussions. No client's facts here.",
	memberIds: members.map(member => member.id),
};

// Each Matter is one client's whole project, walled by client (ORG-6): an
// acquisition, a launch, a lawsuit. The questions inside it are its threads:
// an NDA, a letter of intent, a liability cap (the owner, 2026-10-24). A
// project goes by a code name, as firms name their deals, and says what it
// is in its description; its mark is its initials or an icon.
export const matters = [
	{
		id: "x_sablewood",
		name: "Amsterdam",
		client: "Halvane Robotics",
		color: "#2f56b0",
		icon: "bridge",
		description: "For Halvane Robotics: the purchase of Sablewood Distribution, its Midwest distributor, from the NDA to the closing.",
		memberIds: ["m_gabriel", "m_juno", "m_themis", "m_evelyn", "m_lena"],
	},
	{
		id: "x_orbis",
		name: "Meridian",
		client: "Halvane Robotics",
		color: "#2f8a6e",
		description: "For Halvane Robotics: the master services agreement with Orbis Freight, in negotiation.",
		memberIds: ["m_noah", "m_evelyn", "m_themis"],
	},
	{
		id: "x_kestrel",
		name: "Granite",
		client: "Halvane Robotics",
		color: "#c43a24",
		description: "For Halvane Robotics: Kestrel Automation's patent claim in Delaware, with local counsel.",
		memberIds: ["m_iris", "m_priya", "m_evelyn"],
	},
];

// Versioned results with their evidence (ORG-3): what changed, the clause
// each change rests on, the sources and what could not be checked. A
// version never changes.
export const workProducts = {
	wp_sablewood_v1: {
		title: "Sablewood NDA mark-up",
		kind: "Mark-up",
		version: 1,
		authorId: "m_juno",
		delivered: "2026-10-23T09:40:00",
		state: "Replaced by version 2",
		documentId: "doc_sablewood_markup",
		files: [
			{ name: "Sablewood NDA - Halvane comments v1.docx", pages: 9, size: "48 KB", detail: "with tracked changes" },
		],
		changes: [
			{ title: "Confidential Information limited to what is marked", place: "Section 1.3", quote: "all information disclosed by either party, in any form", source: "Playbook 2.1" },
			{ title: "Term cut from five years to three", place: "Section 4.2", quote: "This Agreement shall remain in effect for five (5) years from the Effective Date.", source: "Playbook 4.1" },
			{ title: "Residuals clause narrowed to unaided memory", place: "Section 6.3", quote: "Recipient may use Residual Information for any purpose.", source: "Playbook 6.2" },
			{ title: "Non-solicit left as drafted", place: "Section 8.1", quote: "Recipient shall not solicit or hire any employee of Discloser.", source: "Playbook 8.2" },
		],
		notChecked: "Exhibit B, which Section 2.1 cites, is not in the Matter's Documents.",
	},
	wp_sablewood_v2: {
		title: "Sablewood NDA mark-up",
		kind: "Mark-up",
		version: 2,
		authorId: "m_juno",
		delivered: "2026-10-23T09:58:00",
		state: "Delivered",
		documentId: "doc_sablewood_markup",
		basis: "Unicorn's standing duty: every incoming NDA is checked against the NDA Playbook.",
		files: [
			{ name: "Sablewood NDA - Halvane comments v2.docx", pages: 9, size: "51 KB", detail: "with tracked changes" },
			{ name: "Sablewood NDA - redline v2 against Harborline form.pdf", pages: 10, size: "212 KB" },
		],
		model: "A frontier model, through Papinian Guard",
		changes: [
			{ title: "Confidential Information limited to what is marked", place: "Section 1.3", quote: "all information disclosed by either party, in any form", source: "Playbook 2.1" },
			{ title: "Term cut from five years to three", place: "Section 4.2", quote: "This Agreement shall remain in effect for five (5) years from the Effective Date.", source: "Playbook 4.1" },
			{ title: "Residuals clause struck", place: "Section 6.3", quote: "Recipient may use Residual Information for any purpose.", source: "Playbook 6.1, hardware clients" },
			{ title: "Non-solicit narrowed to active solicitation", place: "Section 8.1", quote: "Recipient shall not solicit or hire any employee of Discloser.", source: "Playbook 8.2" },
			{ title: "New York law kept", place: "Section 11.4", quote: "This Agreement shall be governed by the laws of the State of New York.", source: "Evelyn, 9:52 AM" },
		],
		sources: [
			{ name: "Sablewood NDA, counterparty draft.docx", note: "9 pages, read in full" },
			{ name: "NDA Playbook 2026.pdf", note: "Sections 2, 4, 6 and 8" },
			{ name: "Contracts register", note: "checked by Lena: no earlier NDA" },
		],
		notChecked: "Exhibit B, which Section 2.1 cites, is not in the Matter's Documents.",
	},
	wp_loi_v1: {
		title: "Letter of intent",
		kind: "Draft",
		version: 1,
		authorId: "m_themis",
		delivered: "2026-10-23T09:48:00",
		state: "Delivered",
		changes: [],
		summary: "$48 million; 30 days of exclusivity with a 15-day extension; a no-shop; no breakup fee.",
		files: [
			{ name: "Project Amsterdam - LOI draft 1.docx", pages: 4, size: "36 KB" },
		],
		notChecked: "Sablewood's debt, which the price assumes is paid at closing.",
	},
};

// What a colleague asks the member, as a junior lawyer asks the lead what to
// do next with a draft (the owner, 2026-10-24): the question, what follows
// each answer, and nothing done until the member answers.
export const decisions = {
	d_sablewood: {
		kind: "ask",
		colleagueId: "m_juno",
		question: "OK to send v2 to Sablewood's counsel?",
		headline: ["v2 of the Sablewood NDA is ready.", "OK to send it to Sablewood's counsel?"],
		note: "It goes to Dana Whitcomb, Sablewood's counsel, with Daniel Price at Harborline in copy.",
		options: [
			{
				key: "A",
				text: "Send it",
				result: "Unicorn sent v2 to Dana Whitcomb, with Daniel Price in copy.",
				state: { text: "Sent Sablewood v2; waits for their comments", kind: "idle", presence: "rest" },
			},
			{
				key: "B",
				text: "Hold, I'll comment",
				result: "Unicorn holds v2 until your comment.",
				state: { text: "Holds Sablewood v2 for your comment", kind: "waiting", presence: "rest" },
			},
		],
	},
};

// Topic threads: a title, an opening post and its discussion, closed with a
// conclusion (ORG-13).
export const threads = [
	{
		id: "t_sablewood",
		matterId: "x_sablewood",
		title: "NDA: our comments on Harborline's form",
		participantIds: ["m_juno", "m_lena", "m_themis", "m_gabriel", "m_evelyn"],
		decisionId: "d_sablewood",
		posts: [
			{
				id: "p_s1",
				authorId: "m_juno",
				at: "2026-10-23T09:24:00",
				paragraphs: [
					"Harborline sent over Sablewood's form NDA this morning: mutual, New York law, nine pages. It is seller-friendly in the usual places, with a five-year term, a residuals clause and an employee non-solicit with no carve-outs. I'm marking it up against our NDA Playbook.",
					"@Lena, do we have anything already signed with Sablewood or its parent? I don't have access to the contracts register.",
				],
			},
			{
				id: "p_s2",
				authorId: "m_lena",
				at: "2026-10-23T09:31:00",
				paragraphs: ["@Unicorn Nothing with Sablewood Distribution. Sablewood Holdings signed a one-way NDA with Halvane's EU subsidiary in 2023, but it expired in May, so nothing carries over."],
			},
			{
				id: "p_s3",
				authorId: "m_juno",
				at: "2026-10-23T09:40:00",
				paragraphs: [
					"Thanks, @Lena. Mark-up v1 is attached: four points, each tied to the Playbook section it rests on.",
					"One thing I couldn't check: Section 2.1 defines the Purpose by reference to Exhibit B, and Exhibit B wasn't in Harborline's package.",
				],
				workProductId: "wp_sablewood_v1",
			},
			{
				id: "p_s4",
				authorId: "m_evelyn",
				at: "2026-10-23T09:52:00",
				quote: {"postId":"p_s3","authorId":"m_juno","text":"Thanks, @Lena. Mark-up v1 is attached: four points, each tied to the Playbook section it rests on. One thing I couldn't check: Section 2.1 defines the Purpose by reference to Exhibit B, and Exhibit B wasn't in Harborline's package."},
				paragraphs: [
					"@Unicorn Good start. Keep New York law, it's not worth a fight. Narrow the non-solicit to active solicitation of people we actually deal with in diligence; general job ads shouldn't count.",
					"And strike residuals entirely, don't just narrow it. Halvane will be sharing the gripper specs for the integration plan, and residuals would let Sablewood's engineers keep whatever they remember.",
				],
			},
			{
				id: "p_s5",
				authorId: "m_themis",
				at: "2026-10-23T09:55:00",
				paragraphs: ["@Evelyn Striking residuals is within the Playbook's hardware position (6.1), so no escalation is needed. For reference, of our last six hardware NDAs, four struck it outright and two limited it to unaided memory."],
			},
			{
				id: "p_s6",
				authorId: "m_gabriel",
				at: "2026-10-23T09:56:00",
				quote: {"postId":"p_s4","authorId":"m_evelyn","text":"@Unicorn Good start. Keep New York law, it's not worth a fight. Narrow the non-solicit to active solicitation of people we actually deal with in diligence; general job ads shouldn't count. And strike residuals entirely, don't just narrow it. Halvane will be sharing the gripper specs for the integration plan, and residuals would let Sablewood's engineers keep whatever they remember."},
				paragraphs: ["Agree on all of it. If Harborline pushes back on residuals, loop me in before we give anything up."],
			},
			{
				id: "p_s7",
				authorId: "m_juno",
				at: "2026-10-23T09:58:00",
				paragraphs: [
					"v2 is attached: three-year term, residuals struck, non-solicit limited to active solicitation, New York law kept. Exhibit B is still outstanding; @Lena has asked Harborline for it.",
					"OK to send v2 to Dana Whitcomb, Sablewood's counsel, with Daniel Price at Harborline in copy?",
				],
				workProductId: "wp_sablewood_v2",
				decisionId: "d_sablewood",
			},
		],
	},
	{
		id: "t_sablewood_loi",
		matterId: "x_sablewood",
		title: "LOI: price, exclusivity and no-shop",
		participantIds: ["m_gabriel", "m_themis", "m_evelyn"],
		posts: [
			{
				id: "p_l1",
				authorId: "m_gabriel",
				at: "2026-10-23T08:55:00",
				paragraphs: ["Spoke with Harborline last night. Sablewood's owners will take $48 million, subject to diligence, but they want 30 days of exclusivity, not the 60 we asked for. @Evelyn, can you and Kitsune turn the LOI today? I'd like it with Harborline by Friday."],
			},
			{
				id: "p_l2",
				authorId: "m_themis",
				at: "2026-10-23T09:48:00",
				quote: {"postId":"p_l1","authorId":"m_gabriel","text":"Spoke with Harborline last night. Sablewood's owners will take $48 million, subject to diligence, but they want 30 days of exclusivity, not the 60 we asked for. @Evelyn, can you and Kitsune turn the LOI today? I'd like it with Harborline by Friday."},
				paragraphs: [
					"@Evelyn Draft 1 of the LOI is attached: $48 million, cash-free and debt-free, 30 days of exclusivity with one 15-day extension at Halvane's option, and a no-shop. No breakup fee, in line with our buy-side playbook for a deal this size.",
					"@Gabriel, one assumption to confirm with Harborline: the price assumes Sablewood's debt is paid off at closing, and I haven't seen their payoff figures yet.",
				],
				workProductId: "wp_loi_v1",
			},
		],
	},
	{
		id: "t_sablewood_dd",
		matterId: "x_sablewood",
		title: "Diligence: data room review",
		participantIds: ["m_lena", "m_juno", "m_evelyn"],
		posts: [
			{
				id: "p_d1",
				authorId: "m_lena",
				at: "2026-10-22T16:10:00",
				paragraphs: ["Harborline opened the data room this afternoon: 212 documents in 9 folders. I've mapped them against our request list. Customer contracts are only partly there and the Joliet warehouse lease is missing; both are on the follow-up list for Harborline."],
			},
			{
				id: "p_d2",
				authorId: "m_juno",
				at: "2026-10-22T16:40:00",
				paragraphs: ["Working from @Lena's index, I've been through the 38 customer contracts in the room. Six need the customer's consent on a change of control, including two of Sablewood's three largest accounts. The list with clause references is in Documents; we'll want those consents as a closing condition, or at least a covenant to seek them."],
			},
		],
	},
	{
		id: "t_orbis",
		matterId: "x_orbis",
		title: "MSA §12: limitation of liability",
		participantIds: ["m_noah", "m_themis", "m_evelyn"],
		posts: [
			{
				id: "p_o1",
				authorId: "m_noah",
				at: "2026-10-23T09:12:00",
				paragraphs: [
					"@Evelyn Orbis's counsel says they won't go above a cap of 12 months' fees. They'll carve out breaches of confidentiality and indemnity claims, but not data breaches. Is that within Halvane's position?",
					"They want an answer by end of day.",
				],
			},
			{
				id: "p_o2",
				authorId: "m_themis",
				at: "2026-10-23T09:20:00",
				quote: {"postId":"p_o1","authorId":"m_noah","text":"@Evelyn Orbis's counsel says they won't go above a cap of 12 months' fees. They'll carve out breaches of confidentiality and indemnity claims, but not data breaches. Is that within Halvane's position? They want an answer by end of day."},
				paragraphs: ["For context: Halvane's March position paper accepts a 12-month cap for logistics vendors only if data-breach losses sit outside it. So the one gap is data breaches. I'd take the 12 months and hold out for a separate, higher cap on data breaches; I can draft the counter for @Noah."],
			},
		],
	},
	{
		id: "t_orbis_scan",
		matterId: "x_orbis",
		title: "MSA: Orbis's redline v4",
		participantIds: ["m_themis", "m_noah", "m_evelyn"],
		posts: [
			{
				id: "p_r1",
				authorId: "m_themis",
				at: "2026-10-23T08:50:00",
				paragraphs: ["@Noah Orbis sent redline v4 as a scanned PDF with no text layer, so I can't compare it with draft 3. Could you ask them for the Word version? I'll run the comparison as soon as it lands."],
				failure: { text: "Comparison stopped: the PDF is a scan with no text.", next: "Waits for the Word file from Orbis, through Noah." },
			},
		],
	},
	{
		id: "t_kestrel",
		matterId: "x_kestrel",
		title: "Answer to the complaint, due Oct 28",
		participantIds: ["m_iris", "m_priya", "m_evelyn"],
		posts: [
			{
				id: "p_k1",
				authorId: "m_iris",
				at: "2026-10-23T07:30:00",
				paragraphs: [
					"@Evelyn Reminder: our answer to Kestrel's complaint is due Wednesday, October 28, in the District of Delaware. Pell & Ashdown circulated draft 1 yesterday afternoon.",
					"@Priya is out until Monday, so I've set a Monday morning check-in with you and local counsel to lock the final.",
				],
			},
		],
	},
];

// Reactions on posts as the session begins: a post's id, then each emoji and
// who put it there.
export const reactions = {
	p_s4: { "👍": ["m_themis"] },
	p_s6: { "👍": ["m_evelyn"] },
	p_d2: { "👀": ["m_lena"] },
};

// The viewer's inbox: one row per thread. What waits for the viewer leads,
// then what is due, then what the viewer was addressed for (ORG-14).
export const inbox = [
	{ threadId: "t_sablewood", isUnread: true },
	{ threadId: "t_sablewood_loi", isUnread: true },
	{ threadId: "t_sablewood_dd", isUnread: false },
	{ threadId: "t_orbis", isUnread: true, waits: "question", ask: "Is a liability cap of 12 months' fees within Halvane's position?", due: "2026-10-23" },
	{ threadId: "t_kestrel", isUnread: true, waits: "deadline", due: "2026-10-28" },
];

// Personal chats: one durable chat with each member, AI colleagues included.
// Talk about a client's work is filed to its Matter (D-042, ORG-16).
export const chats = [
	{
		memberId: "m_juno",
		isUnread: true,
		messages: [
			{ id: "c_j2", authorId: "m_evelyn", at: "2026-10-22T18:10:00", matterId: null, text: "Going forward, keep New York law as the default on every NDA unless I say otherwise." },
			{ id: "c_j3", authorId: "m_juno", at: "2026-10-22T18:11:00", matterId: null, text: "Got it. New York law is now the default on every NDA I handle, unless you tell me otherwise for a client." },
			{ id: "c_j4", authorId: "m_juno", at: "2026-10-23T09:58:00", matterId: "x_sablewood", text: "Sablewood v2 is ready in the thread. OK to send?", threadId: "t_sablewood" },
			{ id: "c_j5", authorId: "m_juno", at: "2026-10-23T10:03:00", matterId: null, isUnsorted: true, text: "Halvane's sales team forwarded me an NDA with Northgate Sensors. I can't match it to any of our projects. Is it part of an existing deal, or should we open a new one?" },
		],
	},
	{
		memberId: "m_lena",
		isUnread: false,
		messages: [
			{ id: "c_l1", authorId: "m_lena", at: "2026-10-23T09:32:00", matterId: "x_sablewood", text: "Checked the register for Unicorn: nothing with Sablewood. The parent's 2023 NDA expired in May." },
			{ id: "c_l2", authorId: "m_evelyn", at: "2026-10-23T09:34:00", matterId: "x_sablewood", text: "Thanks. Can you chase Harborline for Exhibit B? Section 2.1 refers to it, but it wasn't in their package." },
			{ id: "c_l3", authorId: "m_lena", at: "2026-10-23T09:35:00", matterId: "x_sablewood", text: "On it. Emailing Daniel at Harborline now." },
		],
	},
	{
		memberId: "m_noah",
		isUnread: true,
		messages: [
			{ id: "c_m1", authorId: "m_noah", at: "2026-10-23T09:14:00", matterId: "x_orbis", text: "Put the Orbis cap question in the thread. They want an answer today." },
			{ id: "c_m2", authorId: "m_evelyn", at: "2026-10-23T09:15:00", matterId: "x_orbis", text: "Will look before lunch." },
			// It names no agreement, so it waits in the Inbox's Unsorted until the
			// member files it (the owner, 2026-10-24).
			{ id: "c_m3", authorId: "m_noah", at: "2026-10-23T09:44:00", matterId: null, isUnsorted: true, text: "A distributor in Texas is asking whether last year's price list still binds them. Do you know which agreement that falls under?" },
		],
	},
	{
		memberId: "m_gabriel",
		isUnread: true,
		messages: [
			{ id: "c_g1", authorId: "m_gabriel", at: "2026-10-23T09:52:00", matterId: null, isUnsorted: true, text: "Halvane's CFO called about an escrow holdback. Can you take a look before Monday? He didn't say which deal it's for." },
		],
	},
	{
		memberId: "m_iris",
		isUnread: false,
		messages: [
			{ id: "c_i1", authorId: "m_iris", at: "2026-10-23T07:31:00", matterId: "x_kestrel", text: "Kestrel answer is due Oct 28. I'll check in with you and local counsel Monday morning." },
		],
	},
	{
		memberId: "m_themis",
		isUnread: false,
		messages: [
			{ id: "c_th1", authorId: "m_themis", at: "2026-10-23T09:21:00", matterId: "x_orbis", text: "Orbis's cap fits the March position except on data breaches. Want me to draft the counter for @Noah?" },
		],
	},
	{
		memberId: "m_priya",
		isUnread: false,
		messages: [
			{ id: "c_p1", authorId: "m_priya", at: "2026-10-20T18:00:00", matterId: "x_kestrel", text: "Out until the 27th. Griffin is tracking the Kestrel deadline." },
		],
	},
];

// What each AI colleague is doing. "yours" waits for the viewer, "working" is
// work under way, "waiting" waits on someone else, "idle" concerns the
// viewer in nothing (ORG-14). `presence` is what its character acts out, as
// the backend reports it on every AI colleague (D-013).
export const colleagueStates = {
	m_juno: { text: "Asks whether to send Sablewood v2", kind: "yours", presence: "explain" },
	m_teodor: { text: "Watches the EU and US dual-use lists", kind: "idle", presence: "rest" },
	m_iris: { text: "Keeps the Kestrel deadline: Oct 28", kind: "idle", presence: "rest" },
	m_themis: { text: "Waits for Orbis's Word file, through Noah", kind: "waiting", presence: "think" },
};

export const documents = {
	x_sablewood: [
		{ name: "Sablewood NDA, counterparty draft.docx", state: "Read, ready for work", kind: "docx", pages: 9, added: "2026-10-23T09:21:00", by: "Halvane sales" },
		{ name: "NDA Playbook 2026.pdf", state: "Read, ready for work", kind: "pdf", pages: 24, added: "2026-01-12T10:00:00", by: "Kitsune" },
		{ name: "Sablewood NDA - Halvane comments v2.docx", state: "Work product, delivered", kind: "docx", pages: 9, added: "2026-10-23T09:58:00", by: "Unicorn", workProductId: "wp_sablewood_v2" },
		{ name: "Sablewood NDA - redline v2 against Harborline form.pdf", state: "Work product, delivered", kind: "pdf", pages: 10, added: "2026-10-23T09:58:00", by: "Unicorn", workProductId: "wp_sablewood_v2" },
		{ name: "Customer contracts, change-of-control consents.docx", state: "Read, ready for work", kind: "docx", pages: 3, added: "2026-10-22T16:40:00", by: "Unicorn" },
		{ name: "Exhibit B", state: "Cited in Section 2.1, not in Documents", kind: "missing", pages: null, added: null, by: null },
	],
	x_orbis: [
		{ name: "Orbis MSA, draft 3.docx", state: "Read, ready for work", kind: "docx", pages: 41, added: "2026-10-16T14:10:00", by: "Noah" },
		{ name: "Orbis redline v4.pdf", state: "A scan with no text: cannot be read", kind: "failed", pages: 43, added: "2026-10-23T08:44:00", by: "Noah" },
		{ name: "Halvane vendor position paper, March.pdf", state: "Read, ready for work", kind: "pdf", pages: 12, added: "2026-03-04T09:00:00", by: "Kitsune" },
	],
	x_kestrel: [
		{ name: "Kestrel complaint.pdf", state: "Read, ready for work", kind: "pdf", pages: 38, added: "2026-09-28T11:00:00", by: "Griffin" },
		{ name: "Answer, local counsel draft 1.docx", state: "Read, ready for work", kind: "docx", pages: 22, added: "2026-10-22T16:40:00", by: "Pell & Ashdown" },
	],
};

// This module keeps the window's state. While the window runs it is never
// swapped for a fresh copy, which would start empty: a change to it reloads
// the window (2026-10-26: a hot swap left the Mac's window with no Matter, so
// it read nothing and every post was refused).
if (import.meta.hot) import.meta.hot.decline();
