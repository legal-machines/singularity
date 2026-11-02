import { useEffect, useState } from "react";

import { Avatar } from "../../components/Avatar/Avatar.jsx";
import { Button } from "../../components/Button/Button.jsx";
import { Checkbox, Field, Input } from "../../components/Field/Field.jsx";
import { Icon } from "../../components/Icon/Icon.jsx";
import { toast } from "../../components/Toast/toast.js";
import { Row, Rows, Tray } from "../../components/Tray/Tray.jsx";
import { nowLocal } from "../../data/clock.js";
import { personOf } from "../../data/native.js";
import {
	changeTime,
	costText,
	isMatterTimeRead,
	monthText,
	problemOf,
	readMatterTime,
	readMatterUsage,
	recordTime,
	steppedMonth,
	takeBackTime,
	timeOfMatter,
	usageOnBill,
	useTime,
} from "../../data/time.js";
import { shortDate } from "./bill.js";

import styles from "./RealBill.module.css";

// A real Matter's billing (D-181), month by month: the time its members
// recorded, each changing only their own, and the AI work on it, as Usage
// counts it from the Matter's own record of each request, at cost. A
// person's time is what they recorded, never inferred; the AI work is never
// estimated here; and where nothing is recorded, the page says so instead of
// showing sample rows.
export function RealMatterBill({ matter }) {
	const time = useTime();
	const today = nowLocal().slice(0, 10);
	const [month, setMonth] = useState(today.slice(0, 7));
	const [usage, setUsage] = useState({ month: null, summary: null, problem: null });
	const [form, setForm] = useState(null);

	useEffect(() => {
		readMatterTime(matter.id).catch(() => {});
	}, [matter.id]);

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
	}, [matter.id, month, time.version]);

	const isRead = isMatterTimeRead(matter.id);
	const entries = timeOfMatter(matter.id).filter(entry => entry.date.startsWith(month));
	const hours = sumHours(entries);
	const billable = sumHours(entries.filter(entry => entry.billable));
	const people = new Set(entries.map(entry => entry.actorId)).size;
	const work = usage.month == month ? usageOnBill(usage.summary) : null;
	const monthName = monthText(month);
	const isThisMonth = month == today.slice(0, 7);

	function startRecording() {
		setForm({ entryId: null, date: isThisMonth ? today : lastDayOf(month), start: "", hours: "", text: "", billable: true, problem: null, isSaving: false });
	}

	// Its minutes stay as recorded unless the member changes the hours.
	function startChanging(entry) {
		const shown = entry.hours.toFixed(1);
		setForm({ entryId: entry.id, date: entry.date, start: entry.start ?? "", hours: shown, text: entry.text, billable: entry.billable, problem: null, isSaving: false, kept: { hours: shown, minutes: entry.minutes } });
	}

	async function save(event) {
		event.preventDefault();
		const minutes = form.kept != null && form.hours == form.kept.hours ? form.kept.minutes : Math.round(Number(form.hours) * 60);
		const entry = { date: form.date, start: form.start, minutes, text: form.text, billable: form.billable };
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
			error => toast(problemOf(error, "The time was not taken back.")),
		);
	}

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

			<p className={styles.summary}>
				{
					isRead == false
					? "Reading the time recorded on this Matter."
					: hours > 0
					? `${hoursText(hours)} recorded on ${matter.name} in ${monthName}, ${billable == hours ? "all of it billable" : billable == 0 ? "none of it billable" : `${hoursText(billable)} of it billable`}, by ${people == 1 ? "one member" : `${people} members`}.`
					: `No time is recorded on ${matter.name} in ${monthName}.`
				}{" "}
				{workSentence(work, usage, month, monthName)}
			</p>

			{
				form != null
				?
					<form className={styles.form} onSubmit={save}>
						<div className={styles.form_fields}>
							<Field label="Day">
								<Input type="date" value={form.date} max={today} onChange={event => setForm({ ...form, date: event.target.value, problem: null })} />
							</Field>
							<Field label="Started" optional>
								<Input type="time" value={form.start} onChange={event => setForm({ ...form, start: event.target.value, problem: null })} />
							</Field>
							<Field label="Hours">
								<Input type="number" inputMode="decimal" min="0.1" max="24" step="0.1" placeholder="1.5" value={form.hours} onChange={event => setForm({ ...form, hours: event.target.value, problem: null })} />
							</Field>
						</div>
						<Field label="What you did" error={form.problem ?? undefined}>
							<Input value={form.text} placeholder="Reviewed and marked up the escrow agreement" onChange={event => setForm({ ...form, text: event.target.value, problem: null })} />
						</Field>
						<div className={styles.form_foot}>
							<Checkbox label="Billable to the client" checked={form.billable} onChange={checked => setForm({ ...form, billable: checked, problem: null })} />
							<span className={styles.spacer} />
							<Button variant="secondary" onClick={() => setForm(null)}>Cancel</Button>
							<Button variant="primary" type="submit" busy={form.isSaving} disabled={form.text.trim() == "" || !(Number(form.hours) > 0) || form.date == ""}>
								{form.entryId == null ? "Record" : "Save"}
							</Button>
						</div>
					</form>
				:
					null
			}

			<div className={styles.columns}>
				<Tray icon="people" label="Time" count={entries.length}>
					{
						entries.length > 0
						?
							<Rows label={`Time recorded in ${monthName}`}>
								{entries.map(entry => (
									<Row
										key={entry.id}
										lead={<Face entry={entry} />}
										title={entry.text}
										wrap
										line={entryLine(entry)}
										aside={
											<span className={styles.entry_aside}>
												<span className={styles.amount}>{entry.hours.toFixed(1)} h</span>
												{
													entry.isYours
													?
														<>
															<button className={styles.icon} type="button" aria-label="Change this entry" title="Change" onClick={() => startChanging(entry)}><Icon name="edit" size={16} /></button>
															<button className={styles.icon} type="button" aria-label="Take this entry back" title="Take back" onClick={() => takeBack(entry)}><Icon name="trash" size={16} /></button>
														</>
													:
														null
												}
											</span>
										}
									/>
								))}
							</Rows>
						:
							<p className={styles.empty}>
								{isRead ? `Nobody has recorded time on ${matter.name} in ${monthName}. Record yours here, or in the calendar.` : "Reading the time recorded on this Matter."}
							</p>
					}
				</Tray>

				<Tray icon="document" label="AI work, at cost" count={work != null ? work.models.length + (work.unmodelled.requests > 0 ? 1 : 0) : 0} footer="From the Matter's own record of each request, whichever of the firm's computers ran it, at the cost its provider charged or estimated.">
					{
						work != null && work.requests > 0
						?
							<Rows label={`AI work in ${monthName}`}>
								{work.models.map(model => (
									<Row
										key={model.id}
										title={model.id}
										line={`${model.requests} ${model.requests == 1 ? "request" : "requests"}, ${model.tokens.toLocaleString("en-US")} tokens`}
										aside={<span className={styles.amount}>{model.totals.length > 0 ? model.totals.map(total => costText(total.micros, total.currency)).join(" and ") : "No cost recorded"}</span>}
									/>
								))}
								{
									work.unmodelled.requests > 0
									?
										<Row
											key="unmodelled"
											title="Requests that reached no model"
											line={`${work.unmodelled.requests} ${work.unmodelled.requests == 1 ? "request" : "requests"}`}
											aside={<span className={styles.amount}>No cost</span>}
										/>
									:
										null
								}
							</Rows>
						:
							<p className={styles.empty}>
								{usage.problem ?? (work == null ? "Reading the AI work on this Matter." : `No AI work was recorded on ${matter.name} in ${monthName}.`)}
							</p>
					}
				</Tray>
			</div>
		</div>
	);
}

// The face of whoever recorded an entry.
function Face({ entry }) {
	const member = personOf(entry.memberName, entry.isYours);
	return member != null ? <Avatar member={member} size={32} /> : null;
}

function entryLine(entry) {
	const when = entry.start != null ? `${shortDate(entry.date)}, ${clockText(entry.start)} to ${clockText(entry.end)}` : shortDate(entry.date);
	return `${entry.isYours ? "You" : entry.memberName}, ${when}${entry.billable ? "" : ", not billed"}`;
}

// What the AI work came to, in one sentence, or why it is not known yet.
function workSentence(work, usage, month, monthName) {
	if (usage.month != month) return "";
	if (usage.problem != null) return usage.problem;
	if (work == null || work.requests == 0) return `No AI work was recorded on it in ${monthName}.`;
	const cost = work.totals.length > 0
		? `${work.totals.map(total => costText(total.micros, total.currency)).join(" and ")} at cost${work.totals.some(total => total.isEstimated) ? ", part of it as the provider estimated" : ""}`
		: "with no cost recorded";
	const unpriced = work.withoutCost > 0 && work.totals.length > 0 ? ` ${work.withoutCost} of the requests have no cost recorded.` : "";
	return `The AI work recorded on it came to ${work.requests} ${work.requests == 1 ? "request" : "requests"}, ${cost}.${unpriced}`;
}

function sumHours(entries) {
	return Math.round(entries.reduce((total, entry) => total + entry.hours, 0) * 10) / 10;
}

function hoursText(hours) {
	return `${hours.toFixed(1)} ${hours == 1 ? "hour" : "hours"}`;
}

// "9:30 AM" from "09:30".
function clockText(time) {
	const [hours, minutes] = time.split(":").map(Number);
	const date = new Date(Date.UTC(2026, 0, 1, hours % 24, minutes));
	return new Intl.DateTimeFormat("en-US", { hour: "numeric", minute: "2-digit", timeZone: "UTC" }).format(date);
}

function lastDayOf(month) {
	const [year, number] = month.split("-").map(Number);
	return new Date(Date.UTC(year, number, 0)).toISOString().slice(0, 10);
}
