import { useEffect, useState } from "react";

import { Avatar } from "../../components/Avatar/Avatar.jsx";
import { Button } from "../../components/Button/Button.jsx";
import { Dialog } from "../../components/Dialog/Dialog.jsx";
import { useNavigation } from "../../components/Navigation/navigation.js";
import { ModelPicker } from "../../components/ModelPicker/ModelPicker.jsx";
import { Page, PageHead } from "../../components/Page/Page.jsx";
import { ProjectMark } from "../../components/ProjectMark/ProjectMark.jsx";
import { FactList, Part, PartRow, PartRows, Stack, Word, parts } from "../../components/Parts/Parts.jsx";
import { SheetBar } from "../../components/SheetBar/SheetBar.jsx";
import { ConfirmDialog, MoreButton } from "../../components/Manage/Manage.jsx";
import { Dots } from "../../onboarding/Dots.jsx";
import { AboutFields, ColleagueProfileFields, ColleagueWorkFields, ProfileFields } from "../../components/ProfileFields/ProfileFields.jsx";
import { toast } from "../../components/Toast/toast.js";
import { chooseColleagueModel, colleagueMemory, colleagueModelChoices, deleteColleague, editColleague, forgetColleagueNote, hasNative, nameBySeat, nativeApp, refreshFirm, setColleagueArchived, setPersonState, shownMatters, viewerSeatId } from "../../data/native.js";
import { SeatPasswordDialog } from "../Team/TeamPage.jsx";
import { POSITIONS } from "../../onboarding/tour/pages.js";
import { colleagueFacts, personFacts } from "../../data/profiles.js";
import { members, organisation, viewerMay } from "../../data/workspace.js";
import { PROVIDERS } from "../../onboarding/tour/providers.js";
import fixture from "../../public/mock/team/profiles.json";
import { colleagueState, memberAway } from "../threadState.js";
import { findMember, firstName, isViewerMember, officeLine, positionOf, roleLine } from "../words.js";

import styles from "./ProfilePage.module.css";

// A member's profile, in one order for a person and an AI colleague: who it
// is and how to reach it; what it works on, its remit, how it works and its
// record; and beside them the facts of the firm's file.
export function ProfilePage({ memberId, state }) {
	const { open } = useNavigation();
	const [isPointedAt, setPointedAt] = useState(false);
	const member = members.find(item => item.id == memberId);
	if (member == null) return null;

	const isViewer = isViewerMember(member.id);
	const isColleague = member.kind == "ai_colleague";
	const now = isColleague ? colleagueState(member.id, state) : null;
	const away = memberAway(member, state);
	// The prototype's people stand only for themselves: a real member is never
	// given a prototype's facts because their names match.
	const isPrototype = member.isReal != true;
	const facts = isPrototype == false ? null : isColleague ? colleagueFacts[member.id] : personFacts[member.id];
	const profile = isColleague || isPrototype == false ? null : fixture.profiles.find(item => item.id == member.id || item.name == member.name);
	const shared = shownMatters().filter(matter => matter.memberIds.includes(member.id));
	const seatId = useSeatOf(member, isViewer);
	const [isEditing, setEditing] = useState(false);
	const [, setSaved] = useState(0);
	// The firm's administrator alone edits who sits in a seat, the member's own
	// included.
	const mayEdit = !isColleague && seatId != null && viewerMay.manageSeats == true && member.isArchived != true;
	// What the administrator may do to a colleague's seat, or to an AI colleague
	// on the computer that runs it.
	const [managing, setManaging] = useState(null);
	const mayManagePerson = hasNative() && !isColleague && !isViewer && seatId != null && viewerMay.manageSeats == true;
	const mayManageColleague = hasNative() && isColleague && member.agentId != null && member.isFirmCopy != true && viewerMay.hire == true;
	// Its profile, from any of the administrator's computers.
	const mayEditColleague = hasNative() && isColleague && member.agentId != null && viewerMay.hire == true && member.isArchived != true;
	const first = firstName(member);
	const goToTeam = () => open({ kind: "team" });
	async function archiveColleague(archived) {
		try {
			await setColleagueArchived(member.id, archived);
			toast(archived ? `${first} is archived. It takes no more work.` : `${first} is back at work.`,
				archived ? { action: { label: "Undo", run: () => setColleagueArchived(member.id, false) } } : undefined);
		} catch (error) {
			toast(error?.message ?? "It could not be changed. Try again.", { tone: "seal" });
		}
	}
	const manage = mayManagePerson ? (
		<MoreButton label={`More for ${member.name}`} items={[
			member.isArchived == true ? null : { key: "password", label: "Change password", icon: "key", onSelect: () => setManaging("password") },
			member.isArchived == true ? null : { key: "twoStep", label: "Reset two-step sign-in", icon: "shield", onSelect: () => setManaging("twoStep") },
			member.isArchived == true
				? { key: "restore", label: "Restore", icon: "return", onSelect: () => setManaging("restore") }
				: { key: "archive", label: "Archive", icon: "closed", onSelect: () => setManaging("archive") },
			{ key: "remove", label: "Remove from the firm", icon: "trash", onSelect: () => setManaging("remove") },
		]} />
	) : mayManageColleague ? (
		<MoreButton label={`More for ${member.name}`} items={[
			member.isArchived == true
				? { key: "restore", label: "Restore", icon: "return", onSelect: () => archiveColleague(false) }
				: { key: "archive", label: "Archive", icon: "closed", onSelect: () => archiveColleague(true) },
			{ key: "delete", label: "Delete", icon: "trash", onSelect: () => setManaging("delete") },
		]} />
	) : null;

	// What scrolls under the window's band goes under it with a hard edge,
	// no haze.
	return (
		<Page asInbox hardEdge bar={<SheetBar trail={[{ label: organisation.name }, { label: "Team", place: { kind: "team" } }, { label: member.name }]} />}>
			{/* Who they are in the page's one corner - a face in proportion to
			    the name, the office in one phrase and, under it, what they do
			    now - and what can be done with them in the other. */}
			<PageHead
				asInbox
				compact
				carries
				lead={<span onMouseEnter={() => setPointedAt(true)} onMouseLeave={() => setPointedAt(false)}><Avatar member={member} size={56} presence={now?.presence} pointedAt={isPointedAt} /></span>}
				title={isViewer ? `${member.name} (you)` : member.name}
				line={officeLine(member) || roleLine(member)}
				lineAfter={member.isArchived == true ? <Word>Archived</Word>
					: now != null ? <Word tone={now.kind == "yours" ? "yours" : now.kind == "working" ? "work" : "plain"}>{now.text}</Word>
					: away != null ? <Word tone="time">{away}</Word> : null}
				actions={(
					<>
						{!isViewer && member.isArchived != true ? <Button variant="primary" icon="messages" onClick={() => open({ kind: "messages", memberId: member.id })}>Message {firstName(member)}</Button> : null}
						{facts?.email != null ? <Button icon="mail" onClick={() => window.open(`mailto:${facts.email}`)}>Email</Button> : null}
						{mayEdit ? <Button icon="edit" onClick={() => setEditing(true)}>Edit profile</Button> : null}
						{mayEditColleague ? <Button icon="edit" onClick={() => setManaging("edit")}>Edit profile</Button> : null}
						{manage}
					</>
				)}
			/>

			{managing == "edit" ? (
				<EditColleagueProfileDialog member={member} onClose={() => setManaging(null)} onSaved={() => { setManaging(null); setSaved(count => count + 1); }} />
			) : null}
			{managing == "delete" ? (
				<ConfirmDialog
					title={`Delete ${member.name}?`}
					description={`${first} leaves the firm on every computer, and its chats with you go with it. What it wrote stays where it wrote it.`}
					confirm="Delete"
					onClose={() => setManaging(null)}
					onConfirm={async () => {
						await deleteColleague(member.id);
						goToTeam();
						toast(`${member.name} is no longer with the firm.`);
					}} />
			) : null}
			{managing == "archive" ? (
				<ConfirmDialog
					title={`Archive ${member.name}?`}
					description={`${first} can no longer sign in, and their computers stop receiving the firm's work. Their history stays, and you can restore them with a new password.`}
					confirm="Archive"
					onClose={() => setManaging(null)}
					onConfirm={async () => {
						await setPersonState(member.id, "archived");
						toast(`${member.name} is archived.`);
					}} />
			) : null}
			{managing == "remove" ? (
				<ConfirmDialog
					title={`Remove ${member.name} from the firm?`}
					description={`${first} can no longer sign in and leaves the team. What they wrote stays where they wrote it. To work with them again, add them as a new colleague.`}
					confirm="Remove"
					onClose={() => setManaging(null)}
					onConfirm={async () => {
						await setPersonState(member.id, "removed");
						goToTeam();
						toast(`${member.name} is no longer with the firm.`);
					}} />
			) : null}
			{managing == "twoStep" ? (
				<ConfirmDialog
					title={`Reset ${first}’s two-step sign-in?`}
					description={`For a lost phone: ${first}’s authenticator app and recovery codes stop working, and ${first} sets two-step sign-in up again at the next sign-in.`}
					confirm="Reset"
					onClose={() => setManaging(null)}
					onConfirm={async () => {
						await nativeApp().resetSeatTwoFactor(seatId);
						toast(`${first} sets two-step sign-in up again at the next sign-in.`);
					}} />
			) : null}
			{managing == "password" || managing == "restore" ? (
				<SeatPasswordDialog member={member} seatId={seatId} isRestoring={managing == "restore"} onClose={() => setManaging(null)}
					onSet={async password => {
						if (managing == "restore") await setPersonState(member.id, "", password);
						else await nativeApp().setFirmSeatPassword(seatId, password);
					}} />
			) : null}

			{isEditing ? (
				<EditProfileDialog member={member} seatId={seatId} isViewer={isViewer}
					onClose={() => setEditing(false)} onSaved={() => { setEditing(false); setSaved(count => count + 1); }} />
			) : null}
			{/* One column, read from the top down, each part a panel of its own
			    in Apple's grouped manner: who they are, how they think and
			    remember, their work, where they work and with whom, their record,
			    what is on file. */}
			<Stack>
				<Part title="About">
					{isColleague ? ((member.area ?? "").trim() != "" ? <p className={parts.paragraph}>{member.area}</p> : <p className={parts.paragraph_quiet}>Nothing written yet.</p>)
						: <PersonAbout member={member} profile={profile} />}
				</Part>
				{isColleague && member.agentId != null && member.runsHere != false && hasNative() ? <ColleagueModel member={member} /> : null}
				{isColleague && member.agentId != null && hasNative() ? <ColleagueMemory member={member} /> : null}
				{isColleague ? <ColleagueWork member={member} facts={facts} /> : <PersonWork member={member} profile={profile} />}
				<Part title="Matters" count={shared.length}>
					{shared.length > 0 ? (
						<PartRows>
							{shared.map(matter => (
								<PartRow
									key={matter.id}
									lead={<ProjectMark matter={matter} size={32} />}
									title={matter.name}
									line={matter.client}
									aside={matter.isClosed ? <Word>{hasNative() ? "Archived" : "Closed"}</Word> : null}
									onOpen={() => open({ kind: "matter", matterId: matter.id, tab: "overview" })}
								/>
							))}
						</PartRows>
					) : <p className={parts.paragraph_quiet}>{isColleague ? `The firm’s colleague: named in any Matter’s thread, ${firstName(member)} takes the work up there.` : "Not on a Matter you can see."}</p>}
				</Part>
				{isColleague ? null : <WorksWith profile={profile} onOpen={memberId2 => open({ kind: "profile", memberId: memberId2 })} />}
				<Part title="Record">
					<p className={parts.paragraph}>{isColleague ? member.record : personRecord(profile)}</p>
					{isColleague && facts?.attention != null ? <p className={parts.paragraph_quiet}>Lawyer attention: {facts.attention}.</p> : null}
				</Part>
				{facts != null ? (
					<Part title="On file">
						<FactList items={isColleague ? colleagueFile(member, facts) : personFile(facts, profile)} />
					</Part>
				) : null}
			</Stack>
		</Page>
	);
}

// The model a colleague thinks with, for all of its work: the one chosen for it,
// or else the firm's default. The firm's administrator chooses it here; a member
// reads who does.
function ColleagueModel({ member }) {
	const { open } = useNavigation();
	const [choices, setChoices] = useState(null);
	const [isChoosing, setChoosing] = useState(false);
	useEffect(() => {
		let live = true;
		colleagueModelChoices().then(found => live && setChoices(found), () => live && setChoices({ models: [], firmDefault: null }));
		return () => { live = false; };
	}, [member.ownModel?.modelId]);
	if (choices == null) return null;
	const own = member.ownModel ?? null;
	const current = own ?? choices.firmDefault;
	const first = firstName(member);
	const may = viewerMay.connectModels == true;
	const line = current == null
		? (may ? `Add an API key of a model account in Settings, and choose ${first}'s model here.` : "The firm's administrator chooses it.")
		: own != null ? `Chosen for ${first}, through ${providerWords(own.providerId)}.` : `The firm's default, through ${providerWords(current.providerId)}.`;
	const aside = may == false ? null : choices.models.length > 0
		? <Button size="small" onClick={() => setChoosing(true)}>Change</Button>
		: <Button size="small" onClick={() => open({ kind: "settings", tab: "keys" })}>API keys</Button>;
	return (
		<Part title="Model">
			<PartRows>
				{/* The model by its name alone; whose choice it is and through which
				    account stay in its dialog. */}
				<PartRow title={current == null ? "No model yet" : modelWords(current, choices.models)} line={current == null ? line : null} aside={aside} />
			</PartRows>
			{isChoosing ? <ModelDialog member={member} choices={choices} onClose={() => setChoosing(false)} /> : null}
		</Part>
	);
}

function ModelDialog({ member, choices, onClose }) {
	const own = member.ownModel ?? null;
	const [picked, setPicked] = useState(own != null ? keyOf(own) : "");
	const [isSaving, setSaving] = useState(false);
	const [failure, setFailure] = useState(null);
	const first = firstName(member);
	async function save() {
		if (isSaving) return;
		setSaving(true);
		setFailure(null);
		try {
			await chooseColleagueModel(member.agentId, choices.models.find(model => keyOf(model) == picked) ?? null);
			onClose();
		} catch (error) {
			setFailure(error?.message || `${first}'s model is as it was. Try again.`);
			setSaving(false);
		}
	}
	return (
		<Dialog
			title={`${first}'s model`}
			description={`What ${first} thinks with, for all of its work. Client data is protected before any of it reaches an external model.`}
			onClose={onClose}
			actions={<>
				<Button onClick={onClose}>Cancel</Button>
				<Button variant="primary" onClick={save} busy={isSaving}>Save</Button>
			</>}
		>
			{/* Found by its name or its maker. */}
			<ModelPicker
				models={choices.models}
				keyOf={keyOf}
				value={picked}
				onChange={setPicked}
				leading={[{ key: "", label: choices.firmDefault != null ? `The firm's default: ${modelWords(choices.firmDefault, choices.models)}` : "The firm's default" }]}
			/>
			{failure != null ? <p className={styles.editError} role="alert">{failure}</p> : null}
		</Dialog>
	);
}

function keyOf(model) {
	return `${model.providerId} ${model.modelId}`;
}

// A model as its account names it, or by its id in words: "openai/gpt-5.6-sol"
// is GPT 5.6 Sol.
function modelWords(model, known) {
	const named = known.find(item => item.providerId == model.providerId && item.modelId == model.modelId)?.name;
	if (named != null && named != model.modelId) return named;
	const name = model.modelId.split("/").pop();
	return name.split("-").map(word => /^[a-z]/.test(word) ? word[0].toUpperCase() + word.slice(1) : word).join(" ").replace(/^Gpt\b/, "GPT");
}

function providerWords(providerId) {
	if (providerId == "local") return "this computer";
	const provider = PROVIDERS.find(item => item.id == providerId || item.key == providerId);
	return provider != null ? `the firm's ${provider.name} account` : "the firm's model account";
}

// What a colleague remembers: its own notes from every conversation it has in
// the firm, each with what it is about and who said it. Any member lets one
// go; the colleague keeps the rest in every conversation.
function ColleagueMemory({ member }) {
	const [notes, setNotes] = useState(null);
	const [forgetting, setForgetting] = useState(null);
	const [failure, setFailure] = useState("");
	const [gathering, setGathering] = useState(null);
	const [isChoosing, setChoosing] = useState(false);
	useEffect(() => {
		let live = true;
		colleagueMemory(member.agentId).then(found => live && setNotes(found), () => live && setNotes([]));
		memoryGathering(member.agentId).then(found => live && setGathering(found), () => live && setGathering(null));
		return () => { live = false; };
	}, [member.agentId]);
	if (notes == null) return null;
	const first = firstName(member);
	const may = viewerMay.connectModels == true;
	async function forget(note) {
		setForgetting(note.id);
		setFailure("");
		try {
			setNotes(await forgetColleagueNote(member.agentId, note.id));
		} catch {
			setFailure("That note is still there. Try again.");
		} finally {
			setForgetting(null);
		}
	}
	return (
		<Part title="Memory" count={notes.length}>
			{gathering != null ? (
				<PartRows>
					<PartRow
						title={gathering.isOff ? "Memory is off" : `Kept by ${modelWords(gathering.own ?? gathering.firmContext ?? { providerId: "", modelId: "its own model" }, gathering.models)}`}
						line={gathering.isOff ? `${first} keeps no new notes; the ones below stay.` : null}
						aside={may ? <Button size="small" onClick={() => setChoosing(true)}>Change</Button> : null}
					/>
				</PartRows>
			) : null}
			{isChoosing ? (
				<MemoryModelDialog
					member={member}
					gathering={gathering}
					onClose={next => {
						setChoosing(false);
						if (next != null) setGathering(next);
					}}
				/>
			) : null}
			{notes.length > 0 ? (
				<PartRows>
					{notes.map(note => (
						<PartRow
							key={note.id}
							title={note.text}
							line={memoryLine(note)}
							wrap={true}
							aside={<Button size="small" variant="quiet" busy={forgetting == note.id} disabled={forgetting != null} onClick={() => forget(note)}>Forget</Button>}
						/>
					))}
				</PartRows>
			) : <p className={parts.paragraph_quiet}>{firstName(member)} keeps what your conversations settle here, from every chat and thread.</p>}
			{failure != "" ? <p className={parts.paragraph_quiet} role="status">{failure}</p> : null}
		</Part>
	);
}

// What gathers a colleague's memory: the model its page chose, else the
// firm's model for context work, or none; and the models a connected
// account gives for context work, to choose from.
async function memoryGathering(agentId) {
	const api = nativeApp();
	if (typeof api?.setAgentMemoryModel != "function") return null;
	const [detail, capabilities, status] = await Promise.all([api.getAgent(agentId), api.getAiSetupCapabilities(), api.getOnboardingStatus()]);
	const agent = detail?.agent ?? null;
	const context = (capabilities?.roles ?? []).find(role => role.role == "context");
	const models = (context?.eligibleModels ?? []).map(model => ({ providerId: model.providerId ?? "local", modelId: model.modelId,
		name: model.displayName || model.modelId }));
	const assigned = (status?.localSetup?.roleAssignments ?? []).find(entry => entry.role == "context");
	const firmContext = assigned?.providerId && assigned?.modelId ? { providerId: assigned.providerId, modelId: assigned.modelId } : null;
	return { own: agent?.memoryModel ?? null, isOff: agent?.memoryOff == true, models, firmContext };
}

function MemoryModelDialog({ member, gathering, onClose }) {
	const own = gathering?.own ?? null;
	const [picked, setPicked] = useState(gathering?.isOff ? "off" : own != null ? keyOf(own) : "");
	const [isSaving, setSaving] = useState(false);
	const [failure, setFailure] = useState(null);
	const first = firstName(member);
	const models = gathering?.models ?? [];
	async function save() {
		if (isSaving) return;
		setSaving(true);
		setFailure(null);
		try {
			const chosen = models.find(model => keyOf(model) == picked) ?? null;
			const isOff = picked == "off";
			await nativeApp().setAgentMemoryModel(member.agentId, chosen?.providerId ?? "", chosen?.modelId ?? "", isOff);
			onClose({ ...gathering, own: isOff ? null : chosen, isOff });
		} catch (error) {
			setFailure(error?.message || `${first}'s memory is as it was. Try again.`);
			setSaving(false);
		}
	}
	return (
		<Dialog
			title={`${first}'s memory`}
			description={`What keeps ${first}'s notes after each conversation: a short call of its own, protected as all of ${first}'s work. Off, ${first} keeps no new notes.`}
			onClose={() => onClose(null)}
			actions={<>
				<Button onClick={() => onClose(null)}>Cancel</Button>
				<Button variant="primary" onClick={save} busy={isSaving}>Save</Button>
			</>}
		>
			<ModelPicker
				models={models}
				keyOf={keyOf}
				value={picked}
				onChange={setPicked}
				label="Memory model"
				leading={[{ key: "", label: gathering?.firmContext != null ? `The firm's model for context: ${modelWords(gathering.firmContext, models)}` : `${first}'s own model` }]}
				trailing={[{ key: "off", label: "Off: no new notes" }]}
			/>
			{failure != null ? <p className={styles.editError} role="alert">{failure}</p> : null}
		</Dialog>
	);
}

// Whom a note is kept for and who said it, in words: a member's note is
// used only when that member takes part, a Matter's in that Matter, the
// firm's everywhere; who said it, by their seat, under the name they have
// now.
function memoryLine(note) {
	const person = nameBySeat(note.personActorId, note.personName);
	const said = nameBySeat(note.saidByActorId, note.saidByName);
	const kept = note.scope == "matter" ? `Kept for ${note.matterName || "a Matter"}`
		: note.scope == "person" ? `Kept for ${person || "a member"}`
		: note.kind == "rule" ? "A rule for the whole firm" : "Kept for the whole firm";
	if (said == "") return kept;
	if (note.scope == "person" && said == person) return `${kept}, who said it`;
	return `${kept}, told by ${said}`;
}

function ColleagueWork({ member, facts }) {
	return (
		<>
			{/* A colleague of the firm keeps what its hiring said of its remit in
			    this session only; its office and area come from the backend. */}
			{member.remit?.length > 0 ? (
				<Part title="Remit">
					<FactList isLong items={groupRemit(member.remit).map(group => ({ label: group.title, value: group.items.join("; ") }))} />
				</Part>
			) : null}
			{member.duties?.length > 0 ? (
				<Part title="Standing duties">
					<PartRows>{member.duties.map(duty => <PartRow key={duty.text} title={duty.text} line={duty.when} wrap={true} />)}</PartRows>
				</Part>
			) : null}
			{/* What its profile says it does - its standing duties, the brief of
			    its own runs - and how it works, which every conversation of it
			    reads (ColleagueBrief), as a person's page says how they work. */}
			{(member.goal ?? "").trim() != "" ? (
				<Part title="What it does">
					{member.goal.trim().split(/\n\s*\n/).map((paragraph, index) => (
						<p key={index} className={`${parts.paragraph} ${parts.paragraph_lines}`}>{paragraph}</p>
					))}
				</Part>
			) : facts?.can?.length > 0 ? (
				<Part title="What it does">
					<PartRows>{facts.can.map(item => <PartRow key={item} title={item} wrap={true} />)}</PartRows>
				</Part>
			) : null}
			{(member.manner ?? "").trim() != "" ? (
				<Part title={`How ${firstName(member)} works`}>
					<p className={`${parts.paragraph} ${parts.paragraph_lines}`}>{member.manner.trim()}</p>
				</Part>
			) : null}
		</>
	);
}

function PersonWork({ member, profile }) {
	if (profile == null) return null;
	const decisions = profile.decisionProfile;
	return (
		<>
			{decisions != null ? (
				<Part title="Remit">
					<FactList isLong items={[
						decisions.decidesAlone?.length ? { label: "Decides alone", value: decisions.decidesAlone.join("; ") } : null,
						decisions.needsReview?.length ? { label: "Brings for review", value: decisions.needsReview.join("; ") } : null,
						decisions.escalates?.length ? { label: "Escalates", value: decisions.escalates.join("; ") } : null,
					]} />
				</Part>
			) : null}
			{profile.character?.workingStyle != null ? (
				<Part title={`How ${firstName(member)} works`}>
					<p className={parts.paragraph}>{profile.character.workingStyle}</p>
					{profile.character.communicationStyle != null ? <p className={parts.paragraph_quiet}>{profile.character.communicationStyle}</p> : null}
				</Part>
			) : null}
			{profile.strengths?.length > 0 ? (
				<Part title="Strengths">
					<PartRows>{profile.strengths.map(item => <PartRow key={item} title={item} wrap={true} />)}</PartRows>
				</Part>
			) : null}
		</>
	);
}

function WorksWith({ profile, onOpen }) {
	const together = [...(profile?.collaboration?.mentorIds ?? []), ...(profile?.collaboration?.frequentCollaboratorIds ?? [])]
		.map(id => fixture.profiles.find(item => item.id == id)?.name)
		.map(name => members.find(item => item.name == name))
		.filter((item, index, list) => item != null && list.indexOf(item) == index);
	if (together.length == 0) return null;
	return (
		<Part title="Works with">
			<PartRows>
				{together.map(person => (
					<PartRow key={person.id} lead={<Avatar member={person} size={32} />} title={person.name} line={positionOf(person)} onOpen={() => onOpen(person.id)} />
				))}
			</PartRows>
		</Part>
	);
}

// No member it "answers to": an AI colleague is set over no one's
// shoulder.
function colleagueFile(member, facts) {
	return [
		{ label: "At the firm since", value: facts.joined },
		{ label: "Probation", value: facts.probation },
		{ label: "May read", value: facts.reads },
		{ label: "Runs", value: facts.runs },
		member.model ? { label: "Model", value: member.model.replace(/^Runs on /, "").replace(/\.$/, "").replace(/^./, letter => letter.toUpperCase()) } : null,
		{ label: "Protection", value: facts.protection },
		{ label: "Usage", value: facts.spend },
	];
}

function personFile(facts, profile) {
	return [
		{ label: "Telephone", value: facts.phone },
		{ label: "Office", value: facts.office },
		{ label: "At the firm since", value: facts.joined },
		profile?.experienceYears != null ? { label: "In practice", value: `${profile.experienceYears} years` } : null,
		{ label: "Education", value: facts.education.join("; ") },
		facts.admissions?.length > 0 ? { label: "Bar admissions", value: facts.admissions.join("; ") } : null,
		{ label: "Languages", value: facts.languages.join(", ") },
	];
}

function groupRemit(remit) {
	const groups = [];
	for (const item of remit ?? []) {
		const group = groups.find(entry => entry.title == item.title);
		if (group != null) group.items.push(item.text);
		else groups.push({ title: item.title, items: [item.text] });
	}
	return groups;
}

// A person's About: their own lines from the firm's record, each paragraph as
// they wrote it; the prototype's biography in the prototype.
function PersonAbout({ member, profile }) {
	const words = (member.about ?? "").trim() || (profile?.bio ?? "").trim();
	if (words == "") return <p className={parts.paragraph_quiet}>Nothing written yet.</p>;
	return words.split(/\n\s*\n/).map((paragraph, index) => (
		<p key={index} className={`${parts.paragraph} ${parts.paragraph_lines}`}>{paragraph}</p>
	));
}

function personRecord(profile) {
	const record = profile?.recordFixture;
	if (record == null) return "No record kept here yet.";
	const actions = record.medianLawyerActionsPerAcceptedResult;
	const attention = actions != null ? ` ${actions == 1 ? "One lawyer action" : `${actions} lawyer actions`} per accepted result, at the median.` : "";
	const latest = record.recentOutcome != null ? ` Lately: ${record.recentOutcome.replace(/^./, letter => letter.toLowerCase())}` : "";
	return `${record.acceptedWorkProducts} results accepted and ${record.returnedWorkProducts} returned.${attention}${latest}`;
}

// The seat a person sits in, where the firm's record can say who they
// are: another member's from the firm's team, the viewer's from their own
// sign-in. None for a computer signed in to no seat.
function useSeatOf(member, isViewer) {
	const [own, setOwn] = useState(null);
	useEffect(() => {
		if (!isViewer || hasNative() == false || typeof nativeApp().getAccountState != "function") return;
		nativeApp().getAccountState().then(state => setOwn(state?.signedIn ? state.seatId ?? null : null), () => {});
	}, [isViewer]);
	if (isViewer) return own ?? viewerSeatId();
	const actor = member.actorId ?? "";
	return actor.startsWith("seat:") ? actor.slice(5) : null;
}

// Who sits in the seat, edited as it was given: written into the firm's own
// record, which the member's computers take as theirs. In two steps, as a
// colleague's: the profile, then a few lines about them, so neither scrolls.
const PERSON_STEPS = ["Profile", "About"];

function EditProfileDialog({ member, seatId, isViewer, onClose, onSaved }) {
	const [step, setStep] = useState(0);
	const [profile, setProfile] = useState(() => ({
		name: member.name ?? "",
		rank: POSITIONS.includes(member.position) ? member.position : "",
		title: member.title ?? "",
		practice: member.practice ?? "",
		about: member.about ?? "",
		photo: member.photo ?? null,
	}));
	const [isSaving, setSaving] = useState(false);
	const [error, setError] = useState(null);
	const first = (member.name ?? "").split(" ")[0];
	const hasName = profile.name.trim() != "";

	async function save() {
		if (isSaving || hasName == false) return;
		setSaving(true);
		setError(null);
		try {
			const saved = await nativeApp().saveSeatProfile({ seatId, ...profile, name: profile.name.trim(), photo: profile.photo ?? "" });
			member.name = saved.name;
			member.position = saved.rank || member.position;
			member.role = member.position;
			member.title = saved.title || null;
			member.practice = saved.practice || null;
			member.about = saved.about || null;
			member.photo = saved.photo || null;
			member.photoSet = null;
			refreshFirm();
			toast(isViewer ? "Your profile is saved." : `${saved.name.split(" ")[0]}’s profile is saved.`);
			onSaved();
		} catch (reason) {
			setError(reason?.detail ?? reason?.message ?? "The profile could not be saved. Try again.");
			setSaving(false);
		}
	}

	const change = next => {
		setProfile(next);
		setError(null);
	};
	return (
		<Dialog
			layout="spacious"
			size="small"
			title={step == 0 ? (isViewer ? "Your profile" : `${first}’s profile`) : (isViewer ? "About you" : `About ${first}`)}
			description={step == 0 ? "Kept in your firm. Legal Machines never sees it."
				: isViewer ? "What the firm should know of you." : `What the firm should know of ${first}.`}
			onClose={onClose}
			actions={(
				<>
					<Dots className={styles.steps} steps={PERSON_STEPS} current={step} label={isViewer ? "Editing your profile" : `Editing ${first}’s profile`}
						canGo={at => at != step && isSaving == false && hasName} onGo={setStep} />
					{step == 0 ? <Button disabled={isSaving} onClick={onClose}>Cancel</Button> : <Button disabled={isSaving} onClick={() => setStep(0)}>Back</Button>}
					{step == 0
						? <Button variant="primary" disabled={hasName == false} onClick={() => setStep(1)}>Next</Button>
						: <Button variant="primary" busy={isSaving} disabled={hasName == false} onClick={save}>Save</Button>}
				</>
			)}
		>
			{step == 0
				? <ProfileFields value={profile} onChange={change} disabled={isSaving} />
				: <AboutFields value={profile} onChange={change} disabled={isSaving}
					placeholder={isViewer ? "What you lead, and what to bring you" : "What they lead, and what to bring them"} />}
			{error != null ? <p className={styles.editError} role="alert">{error}</p> : null}
		</Dialog>
	);
}

// An AI colleague's profile, edited as a lawyer's is, in two steps as
// onboarding goes: first its profile - a lawyer's fields, its face where a
// person's photo stands - then its work, with the steps' dots at the
// dialog's foot; written where the firm keeps it, on every computer.
const COLLEAGUE_STEPS = ["Profile", "Work"];

export function EditColleagueProfileDialog({ member, onClose, onSaved }) {
	const office = member.office ?? {};
	const [step, setStep] = useState(0);
	const [profile, setProfile] = useState(() => ({
		name: member.name ?? "",
		character: member.character || member.species || "",
		ranksWith: office.ranksWith ?? member.ranksWith ?? "",
		title: office.title ?? member.title ?? member.role ?? "",
		practice: office.practice ?? member.practice ?? "",
		area: office.area ?? member.area ?? "",
		goal: member.goal ?? "",
		manner: member.manner ?? "",
	}));
	const [isSaving, setSaving] = useState(false);
	const [error, setError] = useState(null);
	const first = (member.name ?? "").split(" ")[0];
	const hasName = profile.name.trim() != "";

	async function save() {
		if (isSaving || hasName == false) return;
		setSaving(true);
		setError(null);
		try {
			const saved = await editColleague(member.id, { ...profile, name: profile.name.trim(), answersTo: "" });
			// Another computer runs it: the change shows once that computer takes it.
			toast(saved?.isAsked == true
				? `Saved. It shows once the computer that runs ${first} takes it.`
				: `${profile.name.trim().split(" ")[0]}’s profile is saved.`);
			onSaved();
		} catch (reason) {
			setError(reason?.detail ?? reason?.message ?? "The profile could not be saved. Try again.");
			setSaving(false);
		}
	}

	const change = next => {
		setProfile(next);
		setError(null);
	};
	return (
		<Dialog
			layout="spacious"
			size="small"
			title={step == 0 ? `${first}’s profile` : `${first}’s work`}
			description={step == 0 ? "Kept in your firm. Legal Machines never sees it." : "What it takes up, and how it works with you"}
			onClose={onClose}
			actions={(
				<>
					<Dots className={styles.steps} steps={COLLEAGUE_STEPS} current={step} label={`Editing ${first}’s profile`}
						canGo={at => at != step && isSaving == false && hasName} onGo={setStep} />
					{step == 0 ? <Button disabled={isSaving} onClick={onClose}>Cancel</Button> : <Button disabled={isSaving} onClick={() => setStep(0)}>Back</Button>}
					{step == 0
						? <Button variant="primary" disabled={hasName == false} onClick={() => setStep(1)}>Next</Button>
						: <Button variant="primary" busy={isSaving} disabled={hasName == false} onClick={save}>Save</Button>}
				</>
			)}
		>
			{step == 0
				? <ColleagueProfileFields value={profile} onChange={change} disabled={isSaving} />
				: <ColleagueWorkFields value={profile} onChange={change} disabled={isSaving} />}
			{error != null ? <p className={styles.editError} role="alert">{error}</p> : null}
		</Dialog>
	);
}
