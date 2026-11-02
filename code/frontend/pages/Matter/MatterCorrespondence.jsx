import { useEffect, useState } from "react";

import { Avatar } from "../../components/Avatar/Avatar.jsx";
import { Button } from "../../components/Button/Button.jsx";
import { Icon } from "../../components/Icon/Icon.jsx";
import { useNavigation } from "../../components/Navigation/navigation.js";
import { toast } from "../../components/Toast/toast.js";
import { Dialog } from "../../components/Dialog/Dialog.jsx";
import { Field, Input } from "../../components/Field/Field.jsx";
import { OfficeGuide } from "../../components/OfficeConnection/OfficeGuide.jsx";
import { ConfirmDialog } from "../../components/Manage/Manage.jsx";
import { hasNative, nativeApp, removeLetter, setMatterIntake } from "../../data/native.js";
import { mockNow } from "../../data/workspace.js";
import { inboxTime } from "../Inbox/inboxView.js";
import { contactOf } from "../Mail/LetterPage.jsx";

import styles from "./MatterCorrespondence.module.css";

// A Matter's correspondence (D-106, D-180): its letters, as the Inbox lists
// its mail, each saying how it came, and the settings of its intake (the
// owner, 2026-11-02: "add a button for the correspondence intake settings:
// if letters come to such an account from such a mailbox - that is the basic
// case - and it works only with Outlook connected"; D-228).
export function MatterCorrespondence({ matter, letters, people, isReal }) {
	const { open } = useNavigation();
	const [isIntakeOpen, setIntakeOpen] = useState(false);
	const [removing, setRemoving] = useState(null);
	const now = hasNative() ? new Date() : new Date(mockNow);
	const sorted = [...letters].sort((first, second) => (second.sentAt ?? "").localeCompare(first.sentAt ?? ""));

	return (
		<div className={styles.page}>
			<section className={styles.section} aria-label="Letters">
				<div className={styles.title_row}>
					<h2 className={styles.title}>Letters{letters.length > 0 ? <span className={styles.count}>{letters.length}</span> : null}</h2>
					{isReal ? <Button size="small" icon="settings" onClick={() => setIntakeOpen(true)}>Intake settings</Button> : null}
				</div>
				{(matter.intake ?? []).length > 0 ? <p className={styles.intake_line}>{intakeLine(matter.intake)}</p> : null}
				{sorted.length == 0 ? <p className={styles.none}>No letter is filed to {matter.name} yet.</p> : (
					<ul className={styles.rows}>
						{sorted.map(letter => {
							const isOutgoing = letter.direction == "outgoing";
							const other = isOutgoing ? letter.to?.[0] : letter.from;
							const whom = other?.name || other?.address || "someone";
							const files = letter.attachments?.length ?? 0;
							const filedBy = (letter.filing?.actorName ?? "").trim();
							const how = letter.filing?.by == "rule" ? "by its intake" : letter.filing?.from == "folder" ? "from its folder" : filedBy != "" ? `by ${filedBy.split(" ")[0]}` : "from Outlook";
							return (
								<li key={letter.id} className={styles.row}>
									<button type="button" className={styles.line} onClick={() => open({ kind: "letter", letterId: letter.id })}>
										<span className={styles.face}>
											<Avatar member={contactOf(other)} size={40} showsOnline={false} />
											<span className={styles.mail} aria-hidden="true">@</span>
										</span>
										<span className={styles.text}>
											<span className={styles.head}>
												<span className={styles.subject}>{letter.subject || "A letter with no subject"}</span>
												{letter.sentAt ? <time className={styles.time} dateTime={letter.sentAt}>{inboxTime(letter.sentAt, now)}</time> : null}
											</span>
											<span className={styles.said}>
												{isOutgoing ? "To" : "From"} {whom}{files > 0 ? `, with ${files == 1 ? "a file" : `${files} files`}` : ""}; filed {how}
											</span>
										</span>
									</button>
									{isReal ? (
										<span className={styles.actions}>
											<button type="button" aria-label={`Remove from ${matter.name}: ${letter.subject || "the letter"}`} data-tooltip={`Remove from ${matter.name}`}
												onClick={() => setRemoving(letter)}><Icon name="trash" size={17} /></button>
										</span>
									) : null}
								</li>
							);
						})}
					</ul>
				)}
			</section>

			{isIntakeOpen ? <IntakeDialog matter={matter} letters={letters} onClose={() => setIntakeOpen(false)} /> : null}
			{removing != null ? (
				<ConfirmDialog
					title={`Remove “${removing.subject || "this letter"}” from ${matter.name}?`}
					description={removing.filing?.from == "folder"
						? `Its email file goes to this computer's bin, and the letter leaves ${matter.name} for everyone on it.`
						: `It leaves ${matter.name} for everyone on it; the email stays in Outlook.`}
					confirm="Remove"
					onClose={() => setRemoving(null)}
					onConfirm={async () => {
						await removeLetter(matter.id, removing);
						toast(`The letter is no longer in ${matter.name}.`);
					}} />
			) : null}
		</div>
	);
}

// "Letters to evelyn@dksp.com from helena@northbridge.com are filed here as
// Outlook shows them", and "and 2 more rules".
function intakeLine(rules) {
	const [first, ...rest] = rules;
	const more = rest.length == 0 ? "" : ` and ${rest.length == 1 ? "1 more rule" : `${rest.length} more rules`}`;
	return `Letters to ${first.mailbox} from ${first.from} are filed here as Outlook shows them${more}.`;
}

// Whether Outlook is connected on this computer: the intake files only as
// Outlook shows a letter in Singularity's pane.
function useOutlook() {
	const [state, setState] = useState("reading");
	useEffect(() => {
		const api = nativeApp();
		if (typeof api?.getOfficeStatus != "function" || typeof api?.listOfficeClients != "function") {
			setState("off");
			return undefined;
		}
		let isCurrent = true;
		Promise.all([api.getOfficeStatus(), api.listOfficeClients()]).then(([status, paired]) => {
			if (isCurrent == false) return;
			const isPaired = (paired?.clients ?? []).some(client => client.client == "outlook");
			setState(status?.supported == false ? "unsupported" : isPaired ? "on" : "off");
		}, () => isCurrent && setState("off"));
		return () => {
			isCurrent = false;
		};
	}, []);
	return state;
}

// The intake's settings: each rule an account it reaches and the mailbox it
// comes from; filed as Outlook shows the letter, with Outlook connected.
function IntakeDialog({ matter, letters, onClose }) {
	const outlook = useOutlook();
	const [rules, setRules] = useState(() => (matter.intake ?? []).map(rule => ({ ...rule })));
	const [mailbox, setMailbox] = useState("");
	const [from, setFrom] = useState("");
	const [problem, setProblem] = useState(null);
	const [isSaving, setSaving] = useState(false);
	const [isGuideOpen, setGuideOpen] = useState(false);
	const mailboxes = [...new Set(letters.map(letter => (letter.mailbox ?? "").trim().toLowerCase()).filter(Boolean))];
	const senders = [...new Set(letters.flatMap(letter => [letter.from, ...(letter.to ?? [])]).map(party => (party?.address ?? "").trim().toLowerCase())
		.filter(address => address != "" && mailboxes.includes(address) == false))];
	const isAddress = text => /^[^\s@]+@[^\s@]+\.[^\s@]+$/.test(text.trim());
	const canAdd = isAddress(mailbox) && isAddress(from);

	function add() {
		if (!canAdd) return;
		const rule = { mailbox: mailbox.trim().toLowerCase(), from: from.trim().toLowerCase() };
		if (rules.some(kept => kept.mailbox == rule.mailbox && kept.from == rule.from) == false) setRules([...rules, rule]);
		setFrom("");
		setProblem(null);
	}

	async function save() {
		setSaving(true);
		setProblem(null);
		try {
			await setMatterIntake(matter.id, rules);
			toast(rules.length == 0 ? `${matter.name}'s intake files nothing by rule now.` : `${matter.name}'s intake is saved.`);
			onClose();
		} catch (error) {
			setProblem(error?.message ?? "The intake could not be saved. Try again.");
			setSaving(false);
		}
	}

	if (isGuideOpen) return <OfficeGuide onClose={() => setGuideOpen(false)} />;
	return (
		<Dialog
			title="Correspondence intake"
			description={`A letter that reaches one of these accounts from one of these mailboxes is filed to ${matter.name} as Outlook shows it, for everyone on it.`}
			onClose={onClose}
			actions={(
				<>
					<Button disabled={isSaving} onClick={onClose}>Cancel</Button>
					<Button variant="primary" busy={isSaving} onClick={save}>Save</Button>
				</>
			)}
		>
			<div className={styles.intake}>
				<p className={styles.outlook} data-state={outlook}>
					{outlook == "on" ? "Outlook is connected on this computer."
						: outlook == "reading" ? "Reading Outlook's connection…"
						: outlook == "unsupported" ? "Outlook cannot be connected on this computer."
						: "Outlook is not connected here; the rules work once it is."}
					{outlook == "off" ? <> <button type="button" className={styles.link} onClick={() => setGuideOpen(true)}>Connect Outlook</button></> : null}
				</p>
				{rules.length > 0 ? (
					<ul className={styles.intake_rules}>
						{rules.map(rule => (
							<li key={`${rule.mailbox}\n${rule.from}`} className={styles.intake_rule}>
								<span className={styles.intake_words}>
									<span>To <strong>{rule.mailbox}</strong></span>
									<span>From <strong>{rule.from}</strong></span>
								</span>
								<button type="button" className={styles.remove} aria-label={`Remove the rule for ${rule.from}`} data-tooltip="Remove"
									onClick={() => setRules(rules.filter(kept => kept != rule))}><Icon name="trash" size={16} /></button>
							</li>
						))}
					</ul>
				) : <p className={styles.none}>No rule yet: letters come here only as a member files them.</p>}
				<div className={styles.intake_add}>
					<Field label="The account it reaches">
						<Input value={mailbox} list="intake-mailboxes" placeholder="you@yourfirm.com" autoComplete="off" spellCheck={false}
							onChange={event => setMailbox(event.target.value)} onKeyDown={event => { if (event.key == "Enter") add(); }} />
					</Field>
					<Field label="The mailbox it comes from">
						<Input value={from} list="intake-senders" placeholder="name@client.com" autoComplete="off" spellCheck={false}
							onChange={event => setFrom(event.target.value)} onKeyDown={event => { if (event.key == "Enter") add(); }} />
					</Field>
					<Button disabled={!canAdd} icon="plus" onClick={add}>Add</Button>
					<datalist id="intake-mailboxes">{mailboxes.map(address => <option key={address} value={address} />)}</datalist>
					<datalist id="intake-senders">{senders.map(address => <option key={address} value={address} />)}</datalist>
				</div>
				{problem != null ? <p className={styles.problem} role="alert">{problem}</p> : null}
			</div>
		</Dialog>
	);
}
