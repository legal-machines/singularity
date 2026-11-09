// MOCK DATA - Marlowe Finch LLP's time and billing for Halvane Robotics in
// October 2026. Every name, number and figure is fictional; the firm's tax ID
// begins 00, which the IRS never issues. Nothing here is read from, or written
// to, the backend. The AI colleagues' model costs are the ones their profiles
// report (data/profiles.js), as Usage counts them.

// The firm, as its letterhead reads.
export const firm = {
	name: "Marlowe Finch LLP",
	street: "400 Finch Plaza, 32nd Floor",
	city: "New York, NY 10017",
	phone: "(212) 555-0100",
	email: "billing@marlowefinch.example",
	taxId: "00-1234567",
};

// The client, as the engagement letter of January 12, 2026 names it. Its
// Schedule B sets how the AI colleagues' work is billed.
export const client = {
	id: "04512",
	name: "Halvane Robotics, Inc.",
	attention: "Legal Department",
	street: "1 Halvane Way",
	city: "Pittsburgh, PA 15201",
	engagement: "January 12, 2026",
	terms: 30,
};

// Each timekeeper's standard hourly rate for 2026.
export const rates = {
	m_gabriel: 1250,
	m_priya: 1150,
	m_margaret: 1100,
	m_thomas: 980,
	m_raymond: 980,
	m_amina: 925,
	m_omar: 925,
	m_evelyn: 825,
	m_noah: 640,
	m_lena: 340,
};

// How each AI colleague's work is billed, as Schedule B agrees it in advance
// (ABA Formal Opinion 512): a fixed fee for a defined task, or an agreed rate
// for measured work. Its model usage on a Matter passes through at cost, with
// no markup, as a disbursement; a local model costs no one outside the firm,
// so it stays the firm's overhead. No AI colleague bills hours set in advance.
export const agreements = {
	m_juno: { basis: "fixed", task: "NDA review and mark-up", fee: 450 },
	m_teodor: { basis: "fixed", task: "Export-controls memo", fee: 1800 },
	m_themis: { basis: "measured", task: "Review of commercial positions", rate: 400 },
	m_iris: { basis: "overhead", task: "Docketing and deadline reminders" },
};

// Each Matter's billing: its numbers in the firm's books and the client's,
// the fee budget its charter sets, what was billed before this month, the
// partner responsible for the bill and the lawyer who leads the Matter. Its
// leads see its bill and its invoices in the Matter.
export const matterBilling = {
	x_sablewood: { number: "04512-0011", clientNumber: "HR-LGL-131", budget: 3000, billedBefore: 0, partnerId: "m_gabriel", leadId: "m_evelyn", taskSet: "project" },
	x_orbis: { number: "04512-0007", clientNumber: "HR-LGL-118", budget: 35000, billedBefore: 9860, partnerId: "m_gabriel", leadId: "m_evelyn", taskSet: "project" },
	x_kestrel: { number: "04512-0004", clientNumber: "HR-LIT-022", budget: 150000, billedBefore: 6115, partnerId: "m_priya", leadId: "m_priya", taskSet: "litigation", trust: 25000 },
};

// What was paid on each Matter's earlier invoice this month.
export const payments = {
	x_orbis: { invoice: "MF-26-0377", date: "October 14, 2026", amount: 9860 },
	x_kestrel: { invoice: "MF-26-0372", date: "October 9, 2026", amount: 6115 },
};

// People's time, in tenths of an hour, with the UTBMS task and activity codes
// a US client's e-billing expects. The viewer's entries keep the hour they
// began, so the calendar shows them where they were worked.
export const timeEntries = [
	{ id: "te_02", memberId: "m_noah", matterId: "x_orbis", date: "2026-10-05", hours: 2.4, task: "P500", activity: "A103", text: "Revise the limitation of liability and indemnity sections of the MSA draft." },
	{ id: "te_03", memberId: "m_priya", matterId: "x_kestrel", date: "2026-10-06", hours: 2.0, task: "L120", activity: "A104", text: "Analyze Kestrel's complaint and claim charts; outline the answer and defenses." },
	{ id: "te_04", memberId: "m_evelyn", matterId: "x_orbis", date: "2026-10-08", start: "14:00", hours: 1.2, task: "P500", activity: "A104", text: "Review Orbis's comments to sections 9 and 12; list the open points." },
	{ id: "te_05", memberId: "m_evelyn", matterId: "x_kestrel", date: "2026-10-09", start: "09:30", hours: 3.5, task: "L210", activity: "A103", text: "Draft the answer and affirmative defenses." },
	{ id: "te_06", memberId: "m_priya", matterId: "x_kestrel", date: "2026-10-12", hours: 1.1, task: "L210", activity: "A104", text: "Review and revise the draft answer." },
	{ id: "te_08", memberId: "m_noah", matterId: "x_orbis", date: "2026-10-14", hours: 1.0, task: "P500", activity: "A108", text: "Call with Orbis's counsel on service levels and credits." },
	{ id: "te_09", memberId: "m_priya", matterId: "x_kestrel", date: "2026-10-15", hours: 0.8, task: "L210", activity: "A107", text: "Conference with Delaware local counsel on filing and the local rules." },
	{ id: "te_11", memberId: "m_noah", matterId: "x_orbis", date: "2026-10-19", hours: 1.5, task: "P500", activity: "A103", text: "Draft fallback language for the liability cap." },
	{ id: "te_12", memberId: "m_evelyn", matterId: "x_sablewood", date: "2026-10-20", start: "16:00", hours: 0.3, task: "P300", activity: "A104", text: "Review Sablewood's incoming NDA with Unicorn; set the positions on term and residuals." },
	{ id: "te_16", memberId: "m_evelyn", matterId: "x_kestrel", date: "2026-10-22", start: "09:30", hours: 1.2, task: "L210", activity: "A103", text: "Revise the answer for local counsel's comments." },
	{ id: "te_19", memberId: "m_lena", matterId: "x_sablewood", date: "2026-10-23", hours: 0.2, task: "P100", activity: "A110", text: "Check the firm's register for earlier NDAs with Sablewood." },
	{ id: "te_20", memberId: "m_evelyn", matterId: "x_sablewood", date: "2026-10-23", start: "09:40", hours: 0.3, task: "P500", activity: "A104", text: "Review mark-up v2 on term, residuals and non-solicit; approve sending it." },
	{ id: "te_21", memberId: "m_noah", matterId: "x_orbis", date: "2026-10-23", hours: 0.6, task: "P300", activity: "A104", text: "Analyze whether a cap of 12 months' fees fits Halvane's position." },
];

// The AI colleagues' work on each Matter, billed as Schedule B agrees: a task
// done for a fixed fee, or work measured from its own record of when it ran.
export const aiWork = [
	{ id: "aw_03", memberId: "m_juno", matterId: "x_sablewood", date: "2026-10-23", units: 1, task: "P500", activity: "A103", text: "Review Sablewood's NDA against the Playbook; mark-up v2, sent on Evelyn Hart's approval." },
	{ id: "aw_05", memberId: "m_themis", matterId: "x_orbis", date: "2026-10-23", hours: 1.4, task: "P300", activity: "A104", text: "Review the liability cap and indemnity positions against the firm's commercial positions." },
];

// Each AI colleague's model usage on a Matter, at the provider's actual cost.
export const aiUsage = [
	{ id: "au_01", memberId: "m_juno", matterId: "x_sablewood", from: "2026-10-20", to: "2026-10-23", requests: 41, cost: 112.6 },
	{ id: "au_04", memberId: "m_themis", matterId: "x_orbis", from: "2026-10-05", to: "2026-10-23", requests: 22, cost: 88.15 },
];

// Other disbursements, at cost, with their UTBMS expense codes.
export const expenses = [
	{ id: "ex_01", matterId: "x_kestrel", date: "2026-10-06", code: "E112", text: "Court fees: certified copies of the complaint's exhibits.", amount: 24 },
	{ id: "ex_02", matterId: "x_kestrel", date: "2026-10-09", code: "E106", text: "Online legal research: prosecution histories of the patents in suit.", amount: 212.3 },
	{ id: "ex_03", matterId: "x_kestrel", date: "2026-10-15", code: "E107", text: "Courier: the draft answer to Delaware local counsel.", amount: 38.5 },
];

// The invoice each Matter's October bill becomes, a draft until its partner
// approves it.
export const invoices = {
	x_sablewood: { number: "MF-26-0431" },
	x_orbis: { number: "MF-26-0432" },
	x_kestrel: { number: "MF-26-0434" },
};

export const billingPeriod = { from: "2026-10-01", to: "2026-10-23", label: "October 2026", invoiceDate: "2026-10-23" };
