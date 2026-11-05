import { useEffect, useState } from "react";
import { isEmulation } from "../../emulation/emulation.js";

import { Button } from "../../components/Button/Button.jsx";
import { GiveDialog } from "../../components/FirmComputers/FirmComputers.jsx";
import { Icon } from "../../components/Icon/Icon.jsx";
import { useNavigation } from "../../components/Navigation/navigation.js";
import { Page, PageHead } from "../../components/Page/Page.jsx";
import { ArchiveMatterDialog } from "../../components/Manage/ArchiveMatterDialog.jsx";
import { EditMatterDialog, MoreButton } from "../../components/Manage/Manage.jsx";
import { MarkPicker, ProjectMark } from "../../components/ProjectMark/ProjectMark.jsx";
import { SheetBar } from "../../components/SheetBar/SheetBar.jsx";
import { toast } from "../../components/Toast/toast.js";
import { Part, PartRow, PartRows, Stack, Word, parts } from "../../components/Parts/Parts.jsx";
import { Views } from "../../components/Views/Views.jsx";
import { matterBilling } from "../../data/billing.js";
import { bindRealMatter, editMatter, moveMatterToTrash, isAcceptedWork, isRealMatter, matterWordsOf, realMatterDocuments, realMatterHistory, realWorkProducts, setMatterArchived, shownThreads, useRealWork } from "../../data/native.js";
import { DocumentsExplorer } from "./DocumentsExplorer.jsx";
import { MatterDesk, MatterThreads } from "./MatterDesk.jsx";
import { documents, viewerId, workProducts } from "../../data/workspace.js";
import { isFirmCoordination, letComputerIn, refuseComputer, useFirmComputers } from "../../data/coordination.js";
import { billOf, leadsBilling, money } from "../Billing/bill.js";
import { MatterBill } from "../Billing/BillingView.jsx";
import { RealMatterBill } from "../Billing/RealBill.jsx";
import { findMatter, findMember, formatWhen } from "../words.js";


import styles from "./MatterPage.module.css";

// A Matter: one client's whole project (PRODUCT.md, "The places"), opened from
// its line on the desk. Its sections are tabs under its name: the overview,
// its threads, its documents, its work products and, for its leads, its
// billing (D-075). The overview says what waits, who works on it, and what
// happened, in order, from the first document on.
export function MatterPage(props) {
	const { place } = useNavigation();
	// An address that names no Matter here - one still on its way from the
	// firm's computers, or an old link - says so on a page of its own, and the
	// window around it keeps working (2026-10-26: such an address left the
	// Mac's window blank).
	if (findMatter(place.matterId) == null) return <MatterNotHere />;
	return <MatterOverview {...props} />;
}

function MatterNotHere() {
	const { open } = useNavigation();
	return (
		<Page bar={<SheetBar trail={[{ label: "Matters", place: { kind: "matters" } }]} />}>
			<PageHead
				title="This Matter is not on this computer"
				line="It may still be on its way from your firm's computers. The Matters this computer holds are in Matters."
				actions={<Button variant="primary" onClick={() => open({ kind: "matters" })}>Open Matters</Button>}
			/>
		</Page>
	);
}

function MatterOverview({ state }) {
	// Drawn again whenever the firm's Matter is read again.
	useRealWork();
	const firm = useFirmComputers();
	const [isInviting, setInviting] = useState(false);
	// Edit or Delete, asked in a dialog (D-214).
	const [managing, setManaging] = useState(null);
	const { place, open } = useNavigation();
	const matter = findMatter(place.matterId);
	const tab = place.tab ?? "overview";
	const books = matterBilling[matter.id];
	// The real Matter shows what the backend holds of it, and nothing of the
	// prototype's sample work.
	const isReal = isRealMatter(matter.id);
	// The firm's Matter the member opens is the one the window reads.
	useEffect(() => {
		if (isReal && matter.realMatterId != null) bindRealMatter(matter.realMatterId);
	}, [isReal, matter.realMatterId]);
	const matterThreads = shownThreads().filter(thread => thread.matterId == matter.id).slice(0, isEmulation ? 3 : undefined);
	const matterDocuments = isReal ? realMatterDocuments() : (documents[matter.id] ?? []).filter(item => !isEmulation || !["missing", "failed"].includes(item.kind)).slice(0, isEmulation ? 3 : undefined);
	// What happened on it, from its own records: documents and arrivals.
	const history = isReal ? realMatterHistory() : null;
	const matterWork = isReal
		? realWorkProducts()
		: Object.entries(workProducts).filter(([id]) => matterThreads.some(thread => thread.posts.some(post => post.workProductId == id))).map(([id, item]) => ({ id, ...item }));
	// A colleague the firm let go is no one on the Matter, whatever its record of
	// the work still names (the owner, 2026-11-02).
	const people = matter.memberIds.map(findMember)
		.filter(member => member == null || !(member.kind == "ai_colleague" && member.isFirmColleague === false));
	// The lawyers it was given to whose computer has not opened it yet (D-205).
	const invitedIds = matter.invitedIds ?? [];
	const isLead = leadsBilling(matter.id, viewerId);
	const missing = isReal ? null : matterDocuments.find(item => item.kind == "missing" || item.kind == "failed");
	const goTo = next => open({ ...place, tab: next });
	// The firm's computers that ask to join the Matter open here (D-123), and
	// the invitation that brings a colleague's computer in.
	const asking = isReal ? firm.asking.filter(computer => computer.matterId == matter.realMatterId) : [];
	const mayInvite = isReal && matter.realMatterId != null && isFirmCoordination() && matter.isClosed != true;
	const isArchived = matter.isClosed == true;
	// Archived for everyone on it, and restored by any of them (D-214).
	async function archive(archived) {
		try {
			await setMatterArchived(matter.id, archived);
			toast(archived ? `${matter.name} is archived. It stands apart from your open Matters.` : `${matter.name} is open again.`,
				archived ? { action: { label: "Undo", run: () => setMatterArchived(matter.id, false) } } : undefined);
		} catch (error) {
			toast(error?.message ?? "The Matter could not be changed. Try again.", { tone: "seal" });
		}
	}
	// To this computer's Trash, for 30 days, with its Undo (D-247): no question
	// first, since nothing is lost.
	async function trashIt() {
		try {
			const undo = await moveMatterToTrash(matter.id);
			open({ kind: "matters" });
			toast(`${matter.name} is in the Trash for 30 days.`, { action: { label: "Undo", run: undo } });
		} catch (error) {
			toast(error?.message ?? "The Matter could not be moved to the Trash. Try again.", { tone: "seal" });
		}
	}
	const manage = isReal && !isEmulation ? (
		<MoreButton label={`More for ${matter.name}`} items={[
			{ key: "edit", label: "Edit", icon: "edit", onSelect: () => setManaging("edit") },
			isArchived
				? { key: "restore", label: "Restore", icon: "return", onSelect: () => archive(false) }
				: { key: "archive", label: "Archive", icon: "closed", onSelect: () => setManaging("archive") },
			{ key: "trash", label: "Move to Trash", icon: "trash", onSelect: () => trashIt() },
		]} />
	) : null;

	const sections = [
		{ key: "overview", label: "Overview" },
		{ key: "threads", label: "Threads" },
		{ key: "documents", label: "Documents" },
		{ key: "work", label: "Work products" },
		// A real Matter's time and AI work are its members' to see, and their
		// time theirs to record (D-181); the prototype's bill is its leads'.
		isLead || isReal ? { key: "billing", label: "Billing" } : null,
	].filter(section => section && (!isEmulation || ["threads", "documents", "work"].includes(section.key)));
	const shownTab = sections.some(section => section.key == tab) ? tab : sections[0].key;
	const [markAnchor, setMarkAnchor] = useState(null);
	const openThread = threadId => open({ kind: "thread", threadId, from: "matter" });
	const what = sentenceCase(matter.description.replace(/^For [^:]+: /, ""));

	// Set as a profile is (the owner, 2026-11-02: "rebuild the Matter's page in
	// the style we discussed"): its mark and name in the page's one corner, what
	// it is under them, what can be done with it in the other, carried into the
	// window's band as the page scrolls; its sections chosen as Team chooses
	// its members; each section's parts one under another, each a panel.
	return (
		<Page asInbox hardEdge bar={<SheetBar trail={[{ label: "Matters", place: { kind: "matters" } }, matter.client ? { label: matter.client } : null, { label: matter.name }].filter(Boolean)} />}>
			<PageHead
				asInbox
				compact
				carries
				lead={(
					<button
						className={styles.mark_button}
						type="button"
						aria-label="Change the mark"
						aria-haspopup="dialog"
						aria-expanded={markAnchor != null}
						onClick={event => setMarkAnchor(markAnchor == null ? event.currentTarget : null)}
					>
						<ProjectMark matter={matter} size={56} />
					</button>
				)}
				title={matter.name}
				line={isEmulation ? undefined : matter.client || undefined}
				lineAfter={isArchived ? <Word>{isReal ? "Archived" : "Closed"}</Word> : null}
				actions={isEmulation ? null : (
					<>
						{isArchived && isReal ? <Button icon="return" onClick={() => archive(false)}>Restore</Button> : null}
						{manage}
					</>
				)}
			/>
			{managing == "edit" ? (
				<EditMatterDialog words={matterWordsOf(matter)} onClose={() => setManaging(null)}
					onSave={async words => {
						await editMatter(matter.id, words);
						toast(`${words.name.trim()} is saved.`);
					}} />
			) : null}
			{managing == "archive" ? <ArchiveMatterDialog matter={matter} onClose={() => setManaging(null)} /> : null}
			{markAnchor != null ? <MarkPicker matter={matter} anchor={markAnchor} onClose={() => setMarkAnchor(null)} /> : null}
			{isInviting ? <GiveDialog projectId={matter.realMatterId} matterName={matter.name} memberIds={matter.memberIds} invitedIds={invitedIds} onClose={() => setInviting(false)} /> : null}

			{/* What the Matter is, in its own words under its head (the owner,
			    2026-11-02: "under the name the client, under the head the
			    description"). */}
			{!isEmulation && what != "" ? <p className={styles.about}>{what}</p> : null}
			{sections.length > 1 ? (
				<div className={!isEmulation && what != "" ? `${styles.toolbar} ${styles.toolbar_after}` : styles.toolbar}>
					<Views label={`${matter.name}: sections`} views={sections.map(section => ({ id: section.key, label: section.label }))} view={shownTab} onChange={goTo} controls="matter-section" />
				</div>
			) : null}

			<div id="matter-section" role="tabpanel">
				{shownTab == "overview" ? (
					<>
						{asking.length > 0 || missing != null ? (
							<div className={styles.notices}>
								{asking.map(computer => <AskingBanner key={computer.deviceId} computer={computer} matterName={matter.name} />)}
								{missing != null ? (
									<div className={styles.banner} role="note">
										<span className={styles.banner_icon}><Icon name="alert" size={18} /></span>
										<div className={styles.banner_text}>
											<p className={styles.banner_title}>{missing.kind == "missing" ? `${missing.name} is not in the Documents` : `${missing.name} cannot be read`}</p>
											<p className={styles.banner_line}>{missing.kind == "missing" ? "Section 2.1 of the NDA cites it, so the Purpose is not checked. Lena is asking Sablewood's bankers for it." : "It is a scan with no text. Kitsune waits for the Word file, through Noah."}</p>
										</div>
										<Button size="small" onClick={() => openThread(matterThreads[0].id)}>Open the thread</Button>
									</div>
								) : null}
							</div>
						) : null}
						<MatterDesk
							matter={matter}
							people={people}
							state={state}
							matterThreads={matterThreads}
							matterDocuments={matterDocuments}
							matterWork={isReal ? matterWork : null}
							history={history}
							onDocuments={() => goTo("documents")}
							onThreads={() => goTo("threads")}
							isReal={isReal}
							invitedIds={invitedIds}
							onShare={mayInvite ? () => setInviting(true) : null}
							facts={[
								books != null ? { label: "Matter number", value: books.number } : null,
								books != null ? { label: "Client reference", value: books.clientNumber } : null,
								books != null ? { label: "Lead", value: findMember(books.leadId).name } : null,
								books != null ? { label: "Responsible partner", value: findMember(books.partnerId).name } : null,
								books != null && isLead ? { label: "Fee budget", value: `${money(billOf(matter.id).budget.used)} of ${money(books.budget)}` } : null,
							].filter(Boolean)}
						/>
					</>
				) : null}

				{shownTab == "threads" ? <MatterThreads matter={matter} matterThreads={matterThreads} state={state} isReal={isReal} /> : null}

				{shownTab == "documents" ? (
					<DocumentsExplorer matter={matter} documents={matterDocuments} isReal={isReal} />
				) : null}

				{shownTab == "work" ? (
					<Stack>
						<Part title="Work products" count={matterWork.length}>
							{matterWork.length == 0 ? <p className={parts.paragraph_quiet}>No work product is handed in yet. What a colleague delivers on {matter.name} comes here.</p> : null}
							<PartRows>
								{matterWork.map(item => (
									<PartRow
										key={item.id}
										lead={<span className={styles.version}>v{item.version}</span>}
										title={item.title}
										line={`By ${findMember(item.authorId).name} ${formatWhen(item.delivered)}`}
										aside={item.status != null ? workWord(item.status) : item.state.startsWith("Replaced") ? <Word>Replaced</Word> : <Word tone="yours">Asks you</Word>}
										onOpen={item.documentId != null ? () => open({ kind: "document", documentId: item.documentId }) : undefined}
									/>
								))}
							</PartRows>
						</Part>
					</Stack>
				) : null}

				{shownTab == "billing" && isReal ? <RealMatterBill matter={matter} /> : null}
				{shownTab == "billing" && isLead && !isReal ? <MatterBill bill={billOf(matter.id)} onOpen={open} /> : null}
			</div>
		</Page>
	);
}

// A computer that asks to join the Matter (D-123): the lawyer here compares
// its code with the colleague, then lets it in, which gives it the Matter, or
// says it is not theirs, and it stays out for good.
function AskingBanner({ computer, matterName }) {
	const [isDeciding, setDeciding] = useState(false);
	const first = computer.name.split(/\s+/)[0] ?? "";
	const isNamed = computer.isYou == false && computer.name != "";
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
		<div className={`${styles.banner} ${styles.banner_ask}`} role="note">
			<span className={styles.banner_icon}><Icon name="key" size={18} /></span>
			<div className={styles.banner_text}>
				<p className={styles.banner_title}>{title}</p>
				<p className={styles.banner_line}>{line}</p>
			</div>
			<Button size="small" disabled={isDeciding} onClick={() => decide(false)}>{computer.isYou ? "Not mine" : isNamed ? `Not ${first}'s` : "Not theirs"}</Button>
			<Button size="small" variant="primary" disabled={isDeciding} onClick={() => decide(true)}>Let it in</Button>
		</div>
	);
}



// What a real draft waits for, as its review says it. A draft ready for
// review is ordinary work, never drawn in the accent of a problem (D-174).
function workWord(status) {
	switch (status.kind) {
		case "yours": return <Word tone="yours">{/Ready for review$/.test(status.words ?? "") ? "Ready for review" : "Asks you"}</Word>;
		case "settled": return isAcceptedWork(status) ? <Word tone="done">Approved</Word> : <Word>Replaced</Word>;
		case "failed": return <Word tone="yours">Not completed</Word>;
		case "underway": return <Word tone="work">Working</Word>;
		default: return null;
	}
}

function sentenceCase(text) {
	return text.charAt(0).toUpperCase() + text.slice(1);
}
