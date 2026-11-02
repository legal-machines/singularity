// Fictional US private-company acquisition: buyer acquires 80%, seller retains
// 20%. SPA signing and closing are separate; the SHA takes effect at closing.
// Narrative time is compressed: HSR review is not a six-minute procedure.
// The cast uses its Team roles. AI owns technical work and its own workstreams;
// people negotiate, coordinate external counsel and relay client instructions.
// US disclosure schedules accompany a seller disclosure transmittal letter;
// that letter does not substitute for schedules or cure a disclosed breach.
// HSR notification goes to FTC and DOJ. FTC review assignment is not clearance;
// external antitrust counsel confirms expiry without a Second Request or bar.
// Sources: FTC Premerger Notification Program / HSR Timing Considerations;
// Cooley GO on disclosure schedules; McDermott on US M&A terminology.
// These are illustrative messages and files, not real work or legal advice.
const actors = {
	gabriel: "m_gabriel", kitsune: "m_themis", evelyn: "m_evelyn",
	noah: "m_noah", lena: "m_lena", amina: "m_amina",
	unicorn: "m_juno", dragon: "m_teodor", griffin: "m_iris", phoenix: "entry_phoenix",
};
// Phoenix's illustrative evidence-review role does not hire a real member.
const exchange = (stage, from, to, ask, answer, files, replyFiles) => ({
	stage, from: actors[from], to: actors[to], ask, answer, files, replyFiles,
});
export const scenes = [
	// Scope and independent specialist workstreams.
	exchange("scope", "gabriel", "kitsune", "Buyer 80%; seller 20%.", "I’ll lead legal DD.", ["Letter of intent.pdf", "DD scope.docx", "Data room index.xlsx"], ["DD workplan.xlsx"]),
	exchange("scope", "kitsune", "evelyn", "Take employment DD.", "I’ll speak to HR.", ["Employment pack.pdf", "Staff schedule.xlsx"], []),
	exchange("scope", "phoenix", "noah", "I’ll review IP title.", "I’ll handle consents.", ["IP register.xlsx", "Contracts index.xlsx"], []),
	exchange("scope", "amina", "unicorn", "Are adviser NDAs signed?", "Both NDAs are executed.", ["Adviser access request.eml", "Adviser NDAs.pdf", "Room access list.xlsx"], ["NDA coverage note.docx"]),
	exchange("scope", "dragon", "griffin", "I’ll review licenses.", "I’ll track closing conditions.", ["Export license register.xlsx"], ["Claims register.xlsx", "Closing conditions checklist.xlsx"]),
	exchange("review", "griffin", "gabriel", "Any threatened claims?", "Counsel’s letter attached.", ["Claims register.xlsx"], ["External litigation counsel letter.pdf"]),
	// A joint response to seller's counsel, with a draft/sent distinction.
	exchange("review", "noah", "lena", "Please obtain the signed copy.", "I’ll get it.", ["Seller counsel DD email.eml", "Customer agreement unsigned.pdf"], []),
	exchange("review", "lena", "phoenix", "Signed copy in.", "Consent is required.", ["Customer agreement executed.pdf"], ["Consent clause extract.pdf", "Contract consent matrix.xlsx"]),
	exchange("review", "phoenix", "noah", "Draft reply attached.", "Require consent at closing.", ["Draft DD email reply.eml", "Contract consent matrix.xlsx"], []),
	exchange("review", "noah", "evelyn", "Any HR points?", "Ask about retention triggers.", ["Draft DD email reply.eml"], ["HR DD questions.docx"]),
	exchange("review", "evelyn", "phoenix", "Add these to Noah’s reply.", "Both points added.", ["HR DD questions.docx", "Draft DD email reply.eml"], ["Joint DD reply draft.eml"]),
	exchange("review", "phoenix", "noah", "Joint reply ready.", "Agreed. Reply sent.", ["Joint DD reply draft.eml"], ["Sent joint DD reply.eml"]),
	exchange("review", "noah", "griffin", "Consent at closing agreed.", "Condition recorded.", ["Seller counsel DD response.eml"], ["Closing conditions checklist.xlsx"]),
	// IP, employment and privacy proceed independently.
	exchange("review", "phoenix", "lena", "IP title gap.", "I’ll ask counsel.", ["IP chain gap.docx", "IP assignment agreement draft.docx"], []),
	exchange("review", "lena", "phoenix", "Assignment in.", "IP title verified.", ["Executed contractor IP assignment.pdf"], ["IP ownership note.docx", "IP chain of title.xlsx"]),
	exchange("review", "evelyn", "kitsune", "HR proposed two plans.", "Costs compared.", ["Management retention proposal.docx", "Change-of-control terms.pdf"], ["Retention comparison.docx", "Retention cost model.xlsx"]),
	exchange("risk", "evelyn", "gabriel", "Client prefers plan two?", "Yes. Client approval is in.", ["HR retention recommendation.eml"], ["Client retention approval.eml"]),
	exchange("risk", "gabriel", "kitsune", "Use the approved plan.", "Retention covenant drafted.", ["Client retention approval.eml"], ["Retention covenant draft.docx"]),
	exchange("review", "amina", "phoenix", "Any processor gaps?", "One DPA is missing.", ["Data map.xlsx", "Processor register.xlsx", "Vendor agreements.pdf"], ["Processor gap note.docx"]),
	exchange("risk", "amina", "noah", "DPA required at closing.", "Seller’s counsel agrees.", ["DPA requirement.docx", "Processor gap note.docx"], ["Seller DPA undertaking.eml"]),
	exchange("risk", "amina", "kitsune", "Keep the privacy covenant.", "DPA condition drafted.", ["Agreed privacy wording.docx", "Seller DPA undertaking.eml"], ["Privacy closing condition.docx"]),
	exchange("review", "dragon", "gabriel", "Export license expired.", "No further shipments reported.", ["Export license.pdf", "License review.docx"], ["Export shipment log.xlsx", "Management confirmation.eml"]),
	exchange("risk", "dragon", "griffin", "No post-expiry exports logged.", "Require renewal at closing.", ["Export exposure note.docx", "License renewal requirement.docx"], ["Closing conditions checklist.xlsx"]),
	exchange("risk", "griffin", "noah", "Seek a specific indemnity.", "I’ll seek a cap carve-out.", ["Disputes summary.docx", "Specific indemnity draft.docx"], []),
	exchange("risk", "noah", "kitsune", "Cap carve-out agreed.", "Specific indemnity retained.", ["Seller indemnity agreement.eml", "Specific indemnity draft.docx"], ["SPA indemnity provisions.docx"]),
	exchange("review", "phoenix", "kitsune", "80/20 verified.", "80/20 confirmed.", ["Cap table reconciliation.xlsx", "Share register.pdf", "Share pledge.pdf"], ["Share pledge closing condition.docx"]),
	// DD report, SPA and SHA. A minority remains; SHA is not an unexplained extra.
	exchange("draft", "gabriel", "kitsune", "Tax DD report received.", "Tax protections drafted.", ["Tax DD report.pdf", "Client pledge release requirement.eml"], ["Legal DD report draft.docx", "SPA tax provisions.docx"]),
	exchange("draft", "kitsune", "gabriel", "DD risks reflected in SPA.", "Risk allocation approved.", ["Legal DD report draft.docx", "DD issues register.xlsx", "SPA v1.docx"], []),
	exchange("draft", "gabriel", "kitsune", "Seller gets one board seat.", "SHA draft ready.", ["Minority rights mandate.eml"], ["SHA v1.docx", "Reserved matters comparison.xlsx"]),
	exchange("draft", "noah", "kitsune", "Seller SPA/SHA markups in.", "Both drafts compared.", ["Seller SPA markup.docx", "Seller SHA markup.docx"], ["Buyer SPA v2.docx", "Buyer SHA v2.docx", "Negotiation issues.xlsx"]),
	exchange("draft", "kitsune", "noah", "Limit minority vetoes.", "No operating veto.", ["SHA reserved matters redline.docx", "SPA v2.docx"], ["Seller agreed terms.eml"]),
	exchange("draft", "phoenix", "griffin", "Claim isn’t disclosed.", "Schedule comments ready.", ["Seller disclosure schedules.pdf", "Disclosure gap note.docx"], ["Schedule comments.docx"]),
	exchange("draft", "griffin", "lena", "Obtain revised schedules.", "I’ll follow up.", ["Schedule comments.docx"], []),
	exchange("draft", "lena", "phoenix", "New schedules.", "Claim and DPA listed.", ["Revised disclosure schedules.pdf"], ["Disclosure reconciliation.xlsx"]),
	exchange("signing", "kitsune", "gabriel", "DD and deal drafts final.", "Client approves signing.", ["Legal DD report final.pdf", "SPA agreed.docx", "SHA agreed.docx", "Disclosure schedules final.pdf"], ["Client signing approval.eml"]),
	exchange("signing", "griffin", "lena", "Signing authority confirmed?", "All received.", ["Corporate approvals checklist.xlsx"], ["Buyer board approval.pdf", "Seller board approval.pdf", "Target corporate approvals.pdf", "Signatory authority.pdf"]),
	exchange("signing", "gabriel", "lena", "Sign Tuesday at ten?", "All confirmed.", ["Client signing date instruction.eml"], ["Agreed signing timetable.eml"]),
	exchange("signing", "lena", "phoenix", "Disclosures in.", "Letter matches SPA.", ["Seller disclosure letter.pdf", "Disclosure schedules final.pdf"], ["Disclosure version check.xlsx"]),
	exchange("signing", "kitsune", "phoenix", "Freeze the SPA/SHA set.", "PDF set ready.", ["SPA agreed.docx", "SHA agreed.docx", "Disclosure version check.xlsx"], ["SPA execution.pdf", "SHA execution.pdf", "Signing set index.xlsx"]),
	exchange("signing", "griffin", "gabriel", "Authority and versions checked.", "Buyer authorizes signing.", ["Signing set index.xlsx", "Corporate approvals checklist.xlsx"], ["Buyer release instruction.eml"]),
	exchange("signing", "lena", "griffin", "Seller ready.", "Both releases recorded.", ["Seller release instruction.eml"], ["Signing release log.xlsx"]),
	exchange("signing", "griffin", "lena", "Execution set dispatched.", "Received.", ["SPA execution.pdf", "SHA execution.pdf", "Seller disclosure letter.pdf", "Disclosure schedules final.pdf"], []),
	exchange("signing", "lena", "griffin", "Buyer signed.", "Buyer’s versions match.", ["SPA buyer counterpart.pdf", "SHA buyer counterpart.pdf"], []),
	exchange("signing", "lena", "griffin", "Seller signed.", "SHA effective at closing.", ["SPA seller counterpart.pdf", "SHA seller counterpart.pdf"], ["SPA executed.pdf", "SHA signed.pdf"]),
	exchange("regulatory", "griffin", "gabriel", "Signed; conditions remain.", "Antitrust counsel engaged.", ["SPA executed.pdf", "Closing conditions checklist.xlsx"], ["Antitrust counsel engagement.eml"]),
	// Time advances between these exchanges. Counsel owns HSR advice/filing;
	// Phoenix assembles inputs, Lena coordinates, Griffin tracks evidence.
	exchange("regulatory", "gabriel", "phoenix", "Prepare counsel’s HSR pack.", "Input pack indexed.", ["Antitrust counsel HSR request.eml", "Client HSR source documents.pdf", "Revenue data.xlsx", "SPA executed.pdf"], ["HSR input index.xlsx", "HSR source pack.pdf"]),
	exchange("regulatory", "phoenix", "lena", "Send to counsel.", "Counsel filed.", ["HSR input index.xlsx", "HSR source pack.pdf"], ["Counsel filing confirmation.eml", "FTC and DOJ filing receipts.pdf"]),
	exchange("regulatory", "lena", "griffin", "FTC reviewing.", "HSR period docketed.", ["Counsel FTC review assignment.eml", "FTC and DOJ filing receipts.pdf"], ["HSR timetable.xlsx"]),
	exchange("conditions", "noah", "griffin", "Customer consent signed.", "Consent condition met.", ["Executed customer consent.pdf"], ["Closing conditions checklist.xlsx"]),
	exchange("conditions", "amina", "phoenix", "Vendor DPA signed.", "Agreed terms match.", ["Executed vendor DPA.pdf", "Agreed DPA terms.docx"], ["DPA execution comparison.docx"]),
	exchange("conditions", "dragon", "griffin", "Export license renewed.", "License condition met.", ["Renewed export license.pdf", "Renewal scope comparison.docx"], ["Closing conditions checklist.xlsx"]),
	exchange("regulatory", "lena", "griffin", "HSR period expired.", "Counsel confirms HSR expiry.", ["Antitrust counsel HSR expiry and no-restraint confirmation.eml"], ["HSR condition evidence note.docx"]),
	// Closing is conditional until releases, funds and lender discharge exist.
	exchange("closing", "griffin", "gabriel", "Closing conditions verified.", "Client confirms Friday.", ["Closing conditions checklist.xlsx", "HSR condition evidence note.docx", "DPA execution comparison.docx"], ["Client closing date instruction.eml"]),
	exchange("closing", "gabriel", "kitsune", "Prepare the funds flow.", "Payoff and price reconciled.", ["Client closing date instruction.eml", "Agreed purchase price.xlsx", "Lender payoff letter.pdf"], ["Closing funds flow.xlsx", "Closing memorandum.docx"]),
	exchange("closing", "kitsune", "griffin", "Release SHA at closing.", "Closing set indexed.", ["Closing memorandum.docx", "Closing funds flow.xlsx", "Stock transfer instruments.pdf", "SHA signed.pdf"], ["Closing deliverables index.xlsx"]),
	exchange("closing", "lena", "griffin", "Certificates in.", "Closing certificates checked.", ["Seller bring-down certificate.pdf", "Buyer bring-down certificate.pdf", "Signed stock transfer instruments.pdf"], ["Closing conditions checklist.xlsx"]),
	exchange("closing", "griffin", "gabriel", "Closing set ready. Release?", "Buyer authorizes closing.", ["Closing conditions checklist.xlsx", "Closing funds flow.xlsx"], ["Buyer closing release.eml"]),
	exchange("closing", "lena", "griffin", "Seller ready.", "Await funds confirmation.", ["Seller closing release.eml", "Lender discharge undertaking.pdf"], ["Conditional closing release log.xlsx"]),
	exchange("closing", "lena", "griffin", "Funds received.", "Transfers and SHA released.", ["Seller treasury funds receipt.eml", "Lender payoff receipt.eml", "Share pledge release.pdf"], ["Closing release confirmation.eml", "Updated stock ledger.xlsx", "SHA effective.pdf"]),
	exchange("closed", "griffin", "gabriel", "Closed. Buyer holds 80%.", "Closed. Thank you, everyone.", ["Closing binder index.xlsx", "Closing release confirmation.eml", "Updated stock ledger.xlsx", "SHA effective.pdf"], []),
];

export const INTRO_MS = 2200;
export const BEAT_MS = 5800;
export const ANSWER_MS = 2800;
export const DURATION_MS = INTRO_MS + scenes.length * BEAT_MS;
const phases = [
	[0, "ask"], [400, "transfer"], [1800, "work"],
	[ANSWER_MS, "answer"], [3200, "return"], [4600, "settle"],
];

export function frameAt(elapsed) {
	if (elapsed < INTRO_MS) return { step: 0, phase: "entrance", nextAt: INTRO_MS };
	if (elapsed >= DURATION_MS) return { step: scenes.length - 1, phase: "complete", nextAt: null };
	const step = Math.floor((elapsed - INTRO_MS) / BEAT_MS);
	const local = elapsed - INTRO_MS - step * BEAT_MS;
	const index = phases.findLastIndex(([at]) => local >= at);
	return { step, phase: phases[index][1], nextAt: INTRO_MS + step * BEAT_MS + (phases[index + 1]?.[0] ?? BEAT_MS) };
}

// One scheduled boundary at a time. Hidden/reduced-motion pages pause their
// elapsed story, so a six-minute sequence never restarts when returning to it.
export function createPlayback({ now, schedule, cancel, onFrame }) {
	let elapsed = 0;
	let started = null;
	let timer = null;
	let disposed = false;
	const position = () => elapsed + (started === null ? 0 : now() - started);
	const clear = () => { if (timer !== null) cancel(timer); timer = null; };
	const update = () => {
		if (disposed) return;
		const at = position();
		const frame = frameAt(at);
		onFrame({ ...frame, paused: started === null });
		clear();
		if (started !== null && frame.nextAt !== null) timer = schedule(update, Math.max(0, frame.nextAt - at));
	};
	return {
		resume() { if (disposed || started !== null) return; started = now(); update(); },
		pause() { if (disposed) return; elapsed = position(); started = null; clear(); update(); },
		dispose() { disposed = true; clear(); },
	};
}
