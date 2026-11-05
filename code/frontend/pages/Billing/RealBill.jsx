import { useEffect, useRef, useState } from "react";

import { Avatar } from "../../components/Avatar/Avatar.jsx";
import { Button } from "../../components/Button/Button.jsx";
import { Checkbox, Input } from "../../components/Field/Field.jsx";
import { Icon } from "../../components/Icon/Icon.jsx";
import { toast } from "../../components/Toast/toast.js";
import { Views } from "../../components/Views/Views.jsx";
import { nowLocal } from "../../data/clock.js";
import { personOf, threadOfChat } from "../../data/native.js";
import { members } from "../../data/workspace.js";
import {
	canSaveWorkStatement,
	confirmWorkCosts,
	changeTime,
	costText,
	isMatterTimeRead,
	monthText,
	problemOf,
	readMatterTime,
	readMatterUsage,
	recordTime,
	saveWorkStatement,
	steppedMonth,
	takeBackTime,
	timeOfMatter,
	usageOnBill,
	useTime,
} from "../../data/time.js";
import { callWords, evidenceText, exactCostText, minutesFrom, requestDetailWords, requestTimeWords, requestWords, shortPrint } from "./billWords.js";

import styles from "./RealBill.module.css";

// A real Matter's bill (D-181), a month at a time, as a firm reads a pre-bill
// (the owner, 2026-11-05: "rebuild the billing page so it works
// professionally and as conveniently as possible for lawyers"): what the month
// came to in one sentence, the time as a ledger, the AI model use as a
// disbursement at its cost, and who worked how long. A person's time is what
// they recorded, never inferred; the AI work is never estimated here; and
// where nothing is recorded, the page says so instead of showing sample rows.
// The time reads by day, as a pre-bill does, or by member, as a partner
// reviews it.
export function RealMatterBill({ matter }) {
	const time = useTime();
	const [layout, setLayout] = useState("day");
	const today = nowLocal().slice(0, 10);
	const [month, setMonth] = useState(today.slice(0, 7));
	const [usage, setUsage] = useState({ month: null, summary: null, problem: null });
	const [form, setForm] = useState(null);

	useEffect(() => {
		readMatterTime(matter.id).catch(() => {});
	}, [matter.id]);

	// The month's OpenRouter costs confirmed by OpenRouter's own record, once a
	// minute at most, and the bill read again when one was (D-252).
	const [confirmations, setConfirmations] = useState(0);
	useEffect(() => {
		let isCurrent = true;
		confirmWorkCosts(matter.id, month, false).then(
			result => isCurrent && (result?.confirmed ?? 0) > 0 && setConfirmations(count => count + 1),
			() => {},
		);
		return () => {
			isCurrent = false;
		};
	}, [matter.id, month]);

	// Read for the month, and again a moment after the Matter changes: the AI
	// colleagues' work goes on while the page is open.
	useEffect(() => {
		let isCurrent = true;
		const timer = setTimeout(() => {
			readMatterUsage(matter.id, month).then(
				summary => isCurrent && setUsage({ month, summary, problem: null }),
				error => isCurrent && setUsage({ month, summary: null, problem: problemOf(error, "The AI work could not be read.") }),
			);
		}, usage.month == month ? 1500 : 0);
		return () => {
			isCurrent = false;
			clearTimeout(timer);
		};
	}, [matter.id, month, time.version, confirmations]);

	const isRead = isMatterTimeRead(matter.id);
	const entries = timeOfMatter(matter.id)
		.filter(entry => entry.date.startsWith(month))
		.sort((one, other) => other.date.localeCompare(one.date) || (other.start ?? "").localeCompare(one.start ?? ""));
	const work = usage.month == month ? usageOnBill(usage.summary) : null;
	const monthName = monthText(month);
	const isThisMonth = month == today.slice(0, 7);

	function startRecording() {
		setForm({ entryId: null, date: isThisMonth ? today : lastDayOf(month), hours: "", text: "", billable: true, problem: null, isSaving: false });
	}

	// Its minutes stay as recorded unless the member changes the hours.
	function startChanging(entry) {
		const shown = entry.hours.toFixed(1);
		setForm({ entryId: entry.id, date: entry.date, start: entry.start ?? "", hours: shown, text: entry.text, billable: entry.billable, problem: null, isSaving: false, kept: { hours: shown, minutes: entry.minutes } });
	}

	async function save() {
		const minutes = form.kept != null && form.hours == form.kept.hours ? form.kept.minutes : minutesFrom(form.hours);
		if (minutes == null) {
			setForm(current => ({ ...current, problem: "Give the time as 1.5, 1:30 or 90m." }));
			return;
		}
		const entry = { date: form.date, start: form.start ?? "", minutes, text: form.text, billable: form.billable };
		setForm(current => ({ ...current, isSaving: true, problem: null }));
		try {
			if (form.entryId == null) await recordTime(matter.id, entry);
			else await changeTime(matter.id, form.entryId, entry);
			if (form.date.slice(0, 7) != month) setMonth(form.date.slice(0, 7));
			setForm(null);
		} catch (error) {
			setForm(current => (current == null ? current : { ...current, isSaving: false, problem: problemOf(error, "The time was not recorded. Try again.") }));
		}
	}

	function takeBack(entry) {
		takeBackTime(matter.id, entry.id).then(
			() => toast("The time is taken back from the Matter."),
			error => toast(problemOf(error, "The time was not taken back."), { tone: "seal" }),
		);
	}

	const editing = {
		form,
		setForm,
		save,
		cancel: () => setForm(null),
		today,
		startChanging,
		takeBack,
	};

	return (
		<div className={styles.bill}>
			<div className={styles.bar}>
				<div className={styles.month} role="group" aria-label="Month">
					<button className={styles.step} type="button" aria-label="The month before" onClick={() => setMonth(steppedMonth(month, -1))}><Icon name="back" size={18} /></button>
					<span className={styles.month_name}>{monthName}</span>
					<button className={`${styles.step} ${styles.flip}`} type="button" aria-label="The month after" disabled={isThisMonth} onClick={() => setMonth(steppedMonth(month, 1))}><Icon name="back" size={18} /></button>
				</div>
				<Button variant="primary" icon="clock" onClick={startRecording} disabled={form != null && form.entryId == null}>Record time</Button>
			</div>

			<p className={styles.summary}>{summaryOf(matter, entries, work, usage, month, monthName, isRead)}</p>

			<section className={styles.part} aria-labelledby="bill-time">
				<div className={styles.part_head}>
					<h3 id="bill-time" className={styles.part_name}>Time</h3>
					{entries.length > 0 ? (
						<Views label="How the time reads" views={[{ id: "day", label: "By day" }, { id: "member", label: "By member" }]} view={layout} onChange={setLayout} />
					) : null}
				</div>
				{form != null && form.entryId == null ? <EntryForm editing={editing} /> : null}
				{entries.length > 0 ? (
					layout == "member" ? <ByMember entries={entries} editing={editing} /> : <ByDay entries={entries} editing={editing} />
				) : form == null ? (
					<p className={styles.empty}>{isRead ? `Nobody has recorded time on ${matter.name} in ${monthName}. Record yours here, or in the calendar.` : "Reading the time recorded on this Matter."}</p>
				) : null}
				{entries.length > 0 ? <Totals entries={entries} /> : null}
			</section>

			<Disbursements work={work} usage={usage} matter={matter} month={month} monthName={monthName} />

			{layout == "day" && entries.length > 0 ? <People entries={entries} /> : null}
		</div>
	);
}

// ---- The time -----------------------------------------------------------------------

// The ledger by day, the latest first, as a pre-bill reads.
function ByDay({ entries, editing }) {
	const days = groupsOf(entries, entry => entry.date);
	return (
		<div className={styles.ledger}>
			{days.map(([date, dayEntries], index) => (
				<div key={date} className={styles.group}>
					<p className={styles.group_line}>
						<span className={styles.group_name}>{dayText(date)}</span>
						{/* What the figures are, opposite the first day, as a pre-bill heads its column. */}
						{index == 0 ? <span className={styles.caption_words}>Hours</span> : null}
					</p>
					<ul className={styles.rows}>
						{dayEntries.map(entry => <Entry key={entry.id} entry={entry} editing={editing} who />)}
					</ul>
				</div>
			))}
		</div>
	);
}

// Each member's time under their name, the most first, as a partner reviews it.
function ByMember({ entries, editing }) {
	const people = groupsOf(entries, entry => entry.actorId)
		.sort((one, other) => hoursOf(other[1]) - hoursOf(one[1]));
	return (
		<div className={styles.ledger}>
			{people.map(([actorId, own]) => (
				<div key={actorId} className={styles.group}>
					<div className={styles.person}>
						<Face entry={own[0]} size={28} />
						<span className={styles.person_name}>{own[0].isYours ? "You" : own[0].memberName}</span>
						<span className={styles.person_hours}>{hoursText(hoursOf(own))}</span>
					</div>
					<ul className={styles.rows}>
						{own.map(entry => <Entry key={entry.id} entry={entry} editing={editing} when />)}
					</ul>
				</div>
			))}
		</div>
	);
}

// One entry: who or when, what was done, its hours; the member's own changes
// where it stands.
function Entry({ entry, editing, who = false, when = false }) {
	if (editing.form?.entryId == entry.id) return <li className={styles.editing}><EntryForm editing={editing} /></li>;
	return (
		<li className={styles.entry}>
			{who ? (
				<span className={styles.who}>
					<Face entry={entry} size={24} />
					<span className={styles.who_name}>{entry.isYours ? "You" : entry.memberName}</span>
				</span>
			) : null}
			{when ? <span className={styles.when}>{dayText(entry.date)}</span> : null}
			<span className={styles.text}>{entry.text}</span>
			{entry.isYours ? (
				<span className={styles.tools}>
					<button className={styles.icon} type="button" aria-label="Change this entry" data-tooltip="Change" onClick={() => editing.startChanging(entry)}><Icon name="edit" size={16} /></button>
					<button className={styles.icon} type="button" aria-label="Take this entry back" data-tooltip="Take back" onClick={() => editing.takeBack(entry)}><Icon name="trash" size={16} /></button>
				</span>
			) : null}
			{entry.billable ? null : <span className={styles.state}>No charge</span>}
			<span className={styles.hours}>{entry.hours.toFixed(1)}</span>
		</li>
	);
}

// The month's hours, and how many of them are billed.
function Totals({ entries }) {
	const hours = hoursOf(entries);
	const billable = hoursOf(entries.filter(entry => entry.billable));
	return (
		<div className={styles.totals}>
			<span className={styles.text}>Total</span>
			<span className={styles.state}>{billedWords(hours, billable)}</span>
			<span className={styles.hours}>{hours.toFixed(1)}</span>
		</div>
	);
}

// The time recorded or changed in one line, as a sheet's row is typed: the
// day, the time worked, what was done, and whether it is billed.
function EntryForm({ editing }) {
	const { form, setForm, save, cancel, today } = editing;
	const words = useRef(null);
	useEffect(() => {
		words.current?.focus();
	}, []);
	const canSave = form.text.trim() != "" && form.hours.trim() != "" && form.date != "";
	return (
		<form
			className={styles.form}
			onSubmit={event => {
				event.preventDefault();
				if (canSave) save();
			}}
			onKeyDown={event => {
				if (event.key == "Escape") {
					event.preventDefault();
					cancel();
				}
			}}
		>
			<Input className={styles.form_date} type="date" aria-label="Day" value={form.date} max={today} onChange={event => setForm({ ...form, date: event.target.value, problem: null })} />
			<Input className={styles.form_hours} aria-label="Time worked" placeholder="1.5" inputMode="decimal" value={form.hours} onChange={event => setForm({ ...form, hours: event.target.value, problem: null })} />
			<Input ref={words} className={styles.form_text} aria-label="What you did" placeholder="What you did" value={form.text} onChange={event => setForm({ ...form, text: event.target.value, problem: null })} />
			<Checkbox label="Billable" checked={form.billable} onChange={checked => setForm({ ...form, billable: checked, problem: null })} />
			<span className={styles.form_actions}>
				<Button size="small" onClick={cancel}>Cancel</Button>
				<Button size="small" variant="primary" type="submit" busy={form.isSaving} disabled={!canSave}>{form.entryId == null ? "Record" : "Save"}</Button>
			</span>
			{form.problem != null ? <p className={styles.form_problem}>{form.problem}</p> : null}
		</form>
	);
}

// Who worked how long this month, the most first.
function People({ entries }) {
	const people = groupsOf(entries, entry => entry.actorId)
		.sort((one, other) => hoursOf(other[1]) - hoursOf(one[1]));
	return (
		<section className={styles.part} aria-labelledby="bill-people">
			<h3 id="bill-people" className={styles.part_name}>By member</h3>
			<ul className={styles.rows}>
				{people.map(([actorId, own]) => {
					const hours = hoursOf(own);
					const billable = hoursOf(own.filter(entry => entry.billable));
					return (
						<li key={actorId} className={styles.entry}>
							<span className={styles.who}>
								<Face entry={own[0]} size={24} />
								<span className={styles.who_name}>{own[0].isYours ? "You" : own[0].memberName}</span>
							</span>
							<span className={styles.state}>{billedWords(hours, billable)}</span>
							<span className={styles.hours}>{hours.toFixed(1)}</span>
						</li>
					);
				})}
			</ul>
		</section>
	);
}

// ---- The AI work ----------------------------------------------------------------------

// The AI model use as the bill charges it: a disbursement at the cost its
// providers charged, with no markup (D-075). One press away, the work by whose
// it was - each AI colleague's, each member's own, Singularity's upkeep of the
// Matter's context - and under each, its requests; under each request, the
// evidence its cost rests on, which the firm can send its client (D-252).
function Disbursements({ work, usage, matter, month, monthName }) {
	const [isOpen, setOpen] = useState(false);
	const [openWorker, setOpenWorker] = useState(null);
	const [openRequest, setOpenRequest] = useState(null);
	const [isSaving, setSaving] = useState(false);
	const amount = work != null && work.totals.length > 0 ? amountText(work.totals) : null;

	// The statement for the client, signed here: the window names only the
	// Matter and the month; the folder is the system's to ask for.
	async function saveStatement() {
		if (isSaving) return;
		setSaving(true);
		try {
			const saved = await saveWorkStatement(matter.id, month);
			if (saved != null) toast(`The statement for ${monthName} is saved, with its signed file beside it.`);
		} catch (error) {
			toast(problemOf(error, "The statement was not saved. Try again."), { tone: "seal" });
		}
		setSaving(false);
	}

	return (
		<section className={styles.part} aria-labelledby="bill-disbursements">
			<div className={styles.part_head}>
				<h3 id="bill-disbursements" className={styles.part_name}>Disbursements</h3>
				{work != null && work.requests > 0 && canSaveWorkStatement() ? (
					<Button size="small" variant="quiet" icon="download" busy={isSaving} onClick={saveStatement}>Statement for the client</Button>
				) : null}
			</div>
			{work == null || work.requests == 0 ? (
				<p className={styles.empty}>{usage.problem ?? (work == null ? "Reading the AI work on this Matter." : `No AI model was used on ${matter.name} in ${monthName}.`)}</p>
			) : (
				<>
					<ul className={styles.rows}>
						<li className={styles.entry}>
							<span className={styles.text}>
								AI model use, at cost
								<button className={styles.more} type="button" aria-expanded={isOpen} onClick={() => setOpen(!isOpen)}>{isOpen ? "Hide the requests" : "Show the requests"}</button>
							</span>
							<span className={styles.money}>{amount ?? "No cost"}</span>
						</li>
					</ul>
					{isOpen ? (
						<ul className={`${styles.rows} ${styles.detail}`}>
							{work.workers.map(worker => (
								<Worker
									key={worker.key}
									worker={worker}
									matter={matter}
									isOpen={openWorker == worker.key}
									onToggle={() => setOpenWorker(openWorker == worker.key ? null : worker.key)}
									openRequest={openRequest}
									onRequest={id => setOpenRequest(openRequest == id ? null : id)}
								/>
							))}
						</ul>
					) : null}
				</>
			)}
		</section>
	);
}

// Whose work some requests were, with how many and what they cost; pressed,
// its requests, the latest first.
function Worker({ worker, matter, isOpen, onToggle, openRequest, onRequest }) {
	const member = worker.kind == "colleague" ? members.find(item => item.agentId == worker.agentId) ?? null
		: worker.kind == "person" ? personOf(worker.name, worker.isYours) : null;
	const name = worker.kind == "upkeep" ? "Singularity"
		: worker.isYours ? "You"
		: member?.name ?? (worker.name != "" ? worker.name : worker.kind == "colleague" ? "An AI colleague" : "A member");
	const isEstimated = worker.totals.some(total => total.isEstimated);
	return (
		<li className={styles.worker}>
			<button className={`${styles.entry} ${styles.toggle}`} type="button" aria-expanded={isOpen} onClick={onToggle}>
				<span className={styles.who}>
					{member != null ? <Avatar member={member} size={24} />
						: <span className={worker.kind == "upkeep" ? styles.face_space : styles.face_none} style={{ width: 24, height: 24 }} aria-hidden="true" />}
					<span className={styles.who_name}>{name}</span>
				</span>
				<span className={styles.text}>{countText(worker.requests.length, "request")}</span>
				{isEstimated ? <span className={styles.state}>Estimated</span> : null}
				<span className={styles.money}>{worker.totals.length > 0 ? amountText(worker.totals, exactCostText) : "No cost"}</span>
			</button>
			{isOpen ? (
				<ul className={styles.rows}>
					{worker.requests.map(request => (
						<Request key={request.id} request={request} worker={worker} matter={matter} isOpen={openRequest == request.id} onToggle={() => onRequest(request.id)} />
					))}
				</ul>
			) : null}
		</li>
	);
}

// One request: when, what for, on which model and at whose asking, its cost;
// pressed, each of its calls' evidence and a copy of it all for the client.
function Request({ request, worker, matter, isOpen, onToggle }) {
	const thread = request.chatId != null ? threadOfChat(request.chatId) : null;
	const what = requestWords(request, thread?.title ?? "");
		const state = request.isFailed ? "Failed" : request.executionKind == "local" ? "On this computer" : request.cost?.isEstimated ? "Estimated" : null;
	async function copy() {
		try {
			await navigator.clipboard.writeText(evidenceText(request, { matterName: matter.name, what, costText: exactCostText }));
			toast("The request’s evidence is copied.");
		} catch {
			toast("The evidence could not be copied.", { tone: "seal" });
		}
	}
	return (
		<li className={styles.request}>
			<button className={`${styles.entry} ${styles.toggle}`} type="button" aria-expanded={isOpen} onClick={onToggle}>
				<span className={styles.when}>{requestTimeWords(request.time)}</span>
				<span className={styles.text}>{what}<span className={styles.text_line}>{requestDetailWords(request, worker.kind == "colleague", request.isAskerYou)}</span></span>
				{state != null ? <span className={styles.state}>{state}</span> : null}
				<span className={styles.money}>{request.cost != null && request.cost.micros > 0 ? exactCostText(request.cost.micros, request.cost.currency) : "No cost"}</span>
			</button>
			{isOpen ? (
				<div className={styles.evidence}>
					{request.calls.length == 0 ? (
						<p className={styles.evidence_line}>{request.callsRecorded ? "Nothing left this computer for it, and nothing was charged." : "It ran before Singularity kept the evidence of each call."}</p>
					) : (
						request.calls.map((call, index) => (
							<div key={`${call.time}-${index}`} className={styles.call}>
								<p className={styles.evidence_line}>{callWords(call, exactCostText)}</p>
								{(call.payloadSha256 ?? "") != "" || (call.responseSha256 ?? "") != "" ? (
									<p className={styles.prints}>
										{(call.payloadSha256 ?? "") != "" ? <span title={call.payloadSha256}>Sent {shortPrint(call.payloadSha256)}</span> : null}
										{(call.responseSha256 ?? "") != "" ? <span title={call.responseSha256}>Received {shortPrint(call.responseSha256)}</span> : null}
									</p>
								) : null}
							</div>
						))
					)}
					<button className={styles.more} type="button" onClick={copy}>Copy the evidence</button>
				</div>
			) : null}
		</li>
	);
}

function amountText(totals, money = costText) {
	return totals.map(total => money(total.micros, total.currency)).join(" and ");
}

// ---- Words ----------------------------------------------------------------------------

function summaryOf(matter, entries, work, usage, month, monthName, isRead) {
	if (isRead == false) return "Reading the time recorded on this Matter.";
	const hours = hoursOf(entries);
	const billable = hoursOf(entries.filter(entry => entry.billable));
	const people = new Set(entries.map(entry => entry.actorId)).size;
	const timeWords = hours > 0
		? `${hoursText(hours)} on ${matter.name} in ${monthName}${billable == hours ? ", all billable" : billable == 0 ? ", none billable" : `, ${billable.toFixed(1)} billable`}, by ${people == 1 ? "one member" : `${people} members`}.`
		: `No time on ${matter.name} in ${monthName}.`;
	if (usage.month != month) return timeWords;
	if (usage.problem != null) return `${timeWords} ${usage.problem}`;
	if (work == null || work.requests == 0) return timeWords;
	const amount = work.totals.length > 0 ? work.totals.map(total => costText(total.micros, total.currency)).join(" and ") : null;
	// Requests that found no model cost nothing, and are no part of the sentence.
	if (amount == null) return work.models.length > 0 ? `${timeWords} The AI work has no cost recorded.` : timeWords;
	return `${timeWords} AI models cost ${amount}, billed at cost.`;
}

// The face of whoever recorded an entry.
function Face({ entry, size }) {
	const member = personOf(entry.memberName, entry.isYours);
	return member != null ? <Avatar member={member} size={size} /> : <span className={styles.face_none} style={{ width: size, height: size }} aria-hidden="true" />;
}

function groupsOf(entries, keyOf) {
	const groups = new Map();
	for (const entry of entries) {
		const key = keyOf(entry);
		if (!groups.has(key)) groups.set(key, []);
		groups.get(key).push(entry);
	}
	return [...groups.entries()];
}

function hoursOf(entries) {
	return Math.round(entries.reduce((total, entry) => total + entry.hours, 0) * 10) / 10;
}

// How much of some hours is billed, in the words a pre-bill uses.
function billedWords(hours, billable) {
	if (billable == hours) return "All billable";
	if (billable == 0) return "No charge";
	return `${billable.toFixed(1)} billable`;
}

function hoursText(hours) {
	return `${hours.toFixed(1)} ${hours == 1 ? "hour" : "hours"}`;
}

function countText(count, word) {
	return `${count.toLocaleString("en-US")} ${count == 1 ? word : `${word}s`}`;
}

// "Wednesday, November 4"
function dayText(date) {
	const [year, month, day] = date.split("-").map(Number);
	return new Intl.DateTimeFormat("en-US", { weekday: "long", month: "long", day: "numeric", timeZone: "UTC" }).format(new Date(Date.UTC(year, month - 1, day)));
}

function lastDayOf(month) {
	const [year, number] = month.split("-").map(Number);
	return new Date(Date.UTC(year, number, 0)).toISOString().slice(0, 10);
}
