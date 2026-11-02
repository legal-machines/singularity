// MOCK DATA - mail from the member's Outlook (the owner, 2026-10-24). A
// message from a contact recorded on a project is filed to that project and
// comes to the Inbox under the project's band; any other message stays in
// Outlook. Fictional throughout (AG-12): the people, the companies and the
// addresses, on the reserved .example domain.

// The people outside the firm recorded on each project, with what they are
// to it, written to follow a comma.
export const projectContacts = {
	x_sablewood: [
		{ id: "c_price", name: "Daniel Price", initials: "DP", color: "#8b6a4b", email: "dprice@harborline.example", organisation: "Harborline Capital", role: "Sablewood's bankers" },
		{ id: "c_moreno", name: "Sofia Moreno", initials: "SM", color: "#2a7d8c", email: "smoreno@sablewood.example", organisation: "Sablewood Distribution", role: "general counsel" },
	],
	x_orbis: [
		{ id: "c_kim", name: "Rachel Kim", initials: "RK", color: "#b04a6e", email: "rkim@orbisfreight.example", organisation: "Orbis Freight", role: "senior counsel" },
	],
	x_kestrel: [
		{ id: "c_reyes", name: "Thomas Reyes", initials: "TR", color: "#4a4f61", email: "treyes@pellashdown.example", organisation: "Pell & Ashdown LLP", role: "local counsel in Delaware" },
	],
};

// What came to the member's Outlook this morning and yesterday.
const outlook = [
	{
		id: "mail_exhibit_b",
		from: "dprice@harborline.example",
		cc: ["m_lena"],
		subject: "RE: Sablewood NDA: Exhibit B",
		at: "2026-10-23T10:01:00",
		isUnread: true,
		greeting: "Evelyn, Lena,",
		paragraphs: [
			"As requested, attached is Exhibit B, the summary of terms that Section 2.1 of the draft NDA refers to. It describes the transaction as we discussed it: a purchase of all of Sablewood's membership interests, with a working-capital adjustment at closing.",
			"Please treat it as Confidential Information under the NDA once it is signed. We are free this afternoon if Halvane has questions.",
		],
		signoff: ["Best regards,", "Daniel"],
		attachments: [{ name: "Exhibit B, Summary of Terms.pdf", size: "188 KB" }],
	},
	{
		id: "mail_orbis_cap",
		from: "rkim@orbisfreight.example",
		cc: [],
		subject: "Orbis MSA: Section 12, limitation of liability",
		at: "2026-10-23T08:47:00",
		isUnread: true,
		greeting: "Evelyn,",
		paragraphs: [
			"We have reviewed Halvane's mark-up of Section 12. Orbis can accept a cap of twelve months' fees, provided the carve-outs for gross negligence and wilful misconduct stay as drafted and data-protection losses fall under the cap.",
			"Could you confirm Halvane's position by Friday?",
		],
		signoff: ["Thanks,", "Rachel"],
		attachments: [],
	},
	{
		id: "mail_digest",
		from: "digest@legalweekly.example",
		cc: [],
		subject: "This week in commercial law",
		at: "2026-10-23T07:00:00",
		isUnread: true,
		greeting: "",
		paragraphs: ["The week's rulings and deals in one page."],
		signoff: [],
		attachments: [],
	},
];

// The contact a message comes from, and the project that contact is
// recorded on.
export function contactOf(address) {
	for (const [matterId, contacts] of Object.entries(projectContacts)) {
		const contact = contacts.find(item => item.email == address);
		if (contact != null) return { ...contact, kind: "person", matterId };
	}
	return null;
}

// The messages filed to a project: those from a contact recorded on it.
export const projectMail = outlook
	.map(message => ({ ...message, contact: contactOf(message.from) }))
	.filter(message => message.contact != null)
	.map(message => ({ ...message, matterId: message.contact.matterId }));

export function findMail(mailId) {
	return projectMail.find(message => message.id == mailId) ?? null;
}
