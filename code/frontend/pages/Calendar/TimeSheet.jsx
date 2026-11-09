import { useEffect, useState } from "react";

import { Icon } from "../../components/Icon/Icon.jsx";
import { toast } from "../../components/Toast/toast.js";
import { hasNative, shownBillables, shownMatters } from "../../data/native.js";
import { nowLocal } from "../../data/clock.js";
import { ownTime, problemOf, recordTime as recordRealTime, takeBackTime, useTime } from "../../data/time.js";
import { matters, viewerId } from "../../data/workspace.js";
import { money } from "../Billing/bill.js";
import { addDays, addMinutes, clock, longDay, monthShort, startOfWeek, weekdayShort } from "./dates.js";
import { recordTime, useCalendar, viewerRate } from "./store.js";

import styles from "./Calendar.module.css";

// The member's time, as a timesheet in ProjectMate keeps it: the week by
// Matter and day, the entries of each day with what they bill, a timer on a
// Matter, and the week submitted to the Matters' leads, who bill it. Each
// entry is also an event on the calendar, where it was worked. In the app the
// sheet is the member's own time as the backend keeps it on each Matter: an
// entry is on its Matter the moment it is recorded, so there is no week to
// submit, and no rate is shown that the firm never set.
const today = nowLocal().slice(0, 10);

// What the member has submitted in this session, by entry.
const submitted = new Set();

export function TimeSheet({ onOpen }) {
	useCalendar();
	useTime();
	const isReal = hasNative();
	const [week, setWeek] = useState(startOfWeek(today));
	const [timer, setTimer] = useState(null);
	const [tick, setTick] = useState(0);
	const [, refresh] = useState(0);

	useEffect(() => {
		if (timer == null) return undefined;
		const interval = setInterval(() => setTick(value => value + 1), 1000);
		return () => clearInterval(interval);
	}, [timer]);

	const days = Array.from({ length: 7 }, (_, index) => addDays(week, index));
	const entries = (isReal ? ownTime() : shownBillables().timeEntries.filter(entry => entry.memberId == viewerId))
		.filter(entry => entry.date >= days[0] && entry.date <= days[6]);
	const matterIds = [...new Set(entries.map(entry => entry.matterId))];
	const hoursOf = list => Math.round(list.reduce((total, entry) => total + entry.hours, 0) * 10) / 10;
	const weekHours = hoursOf(entries);
	const billableHours = hoursOf(entries.filter(entry => entry.billable != false));
	const status = entry => (submitted.has(entry.id) || entry.date < week || entry.date < startOfWeek(today) ? "Submitted" : "Draft");
	const drafts = entries.filter(entry => status(entry) == "Draft");
	const elapsed = timer != null ? Math.floor((Date.now() - timer.started) / 1000) : 0;
	void tick;

	function stopTimer() {
		if (isReal) {
			// From the moment it started, for as long as it ran.
			const began = timer.startedAt;
			const running = timer;
			setTimer(null);
			recordRealTime(running.matterId, { date: began.slice(0, 10), start: began.slice(11, 16), minutes: Math.max(1, Math.ceil(elapsed / 60)), text: running.text })
				.catch(error => toast(problemOf(error, "The time was not recorded.")));
			return;
		}
		const minutes = Math.max(6, Math.ceil(elapsed / 60));
		const start = nowLocal().slice(0, 16);
		recordTime({ matterId: timer.matterId, start, end: addMinutes(start, minutes), text: timer.text });
		setTimer(null);
	}

	function submitWeek() {
		for (const entry of drafts) submitted.add(entry.id);
		refresh(value => value + 1);
	}

	return (
		<div className={styles.time}>
			<div className={styles.time_bar}>
				<button className={styles.today_button} type="button" onClick={() => setWeek(startOfWeek(today))}>This week</button>
				<button className={styles.icon_button} type="button" aria-label="Previous week" onClick={() => setWeek(addDays(week, -7))}><Icon name="back" /></button>
				<button className={`${styles.icon_button} ${styles.flip}`} type="button" aria-label="Next week" onClick={() => setWeek(addDays(week, 7))}><Icon name="back" /></button>
				<span className={styles.time_range}>{monthShort(days[0])} {Number(days[0].slice(8))} to {monthShort(days[6])} {Number(days[6].slice(8))}, {days[6].slice(0, 4)}</span>
				<span className={styles.spacer} />
				<Timer timer={timer} elapsed={elapsed} needsWords={isReal} onStart={setTimer} onStop={stopTimer} />
			</div>

			<div className={styles.time_scroll}>
				{
					isReal
					?
						<p className={styles.time_summary}>
							{weekHours} {weekHours == 1 ? "hour" : "hours"} this week{billableHours != weekHours ? `, ${billableHours} of them billable` : ""}.{" "}
							Each entry is on its Matter as you record it, and everyone on the Matter sees it on its Billing tab.
						</p>
					:
						<p className={styles.time_summary}>
							{weekHours} {weekHours == 1 ? "hour" : "hours"} this week, {money(weekHours * viewerRate)} at your rate.{" "}
							{drafts.length > 0 ? `${drafts.length} ${drafts.length == 1 ? "entry is a draft" : "entries are drafts"} until you submit the week.` : "Everything is submitted to the Matters' leads."}{" "}
							ProjectMate receives what you submit.
						</p>
				}

				<table className={styles.sheet}>
					<thead>
						<tr>
							<th className={styles.sheet_matter}>Matter</th>
							{days.map(day => (
								<th key={day} className={day == today ? styles.sheet_today : undefined}>
									<span className={styles.sheet_dow}>{weekdayShort(day).toUpperCase()}</span>
									<span className={styles.sheet_dom}>{Number(day.slice(8))}</span>
								</th>
							))}
							<th className={styles.sheet_total}>Total</th>
						</tr>
					</thead>
					<tbody>
						{matterIds.map(matterId => {
							const matter = matters.find(item => item.id == matterId);
							const own = entries.filter(entry => entry.matterId == matterId);
							return (
								<tr key={matterId}>
									<th className={styles.sheet_matter} scope="row">
										<button className={styles.link} type="button" onClick={() => onOpen({ kind: "matter", matterId, tab: "threads" })}>{matter?.name}</button>
									</th>
									{days.map(day => {
										const hours = hoursOf(own.filter(entry => entry.date == day));
										return <td key={day} className={day == today ? styles.sheet_today : undefined}>{hours > 0 ? hours.toFixed(1) : ""}</td>;
									})}
									<td className={styles.sheet_total}>{hoursOf(own).toFixed(1)}</td>
								</tr>
							);
						})}
						{matterIds.length == 0 ? <tr><td className={styles.sheet_empty} colSpan={9}>{isReal ? "No time recorded this week. Start the timer, or record time in the calendar or on a Matter's Billing tab." : "No time recorded this week."}</td></tr> : null}
					</tbody>
					<tfoot>
						<tr>
							<th className={styles.sheet_matter} scope="row">Day</th>
							{days.map(day => {
								const hours = hoursOf(entries.filter(entry => entry.date == day));
								return <td key={day} className={day == today ? styles.sheet_today : undefined}>{hours > 0 ? hours.toFixed(1) : ""}</td>;
							})}
							<td className={styles.sheet_total}>{weekHours.toFixed(1)}</td>
						</tr>
					</tfoot>
				</table>

				{
					isReal
					?
						null
					:
						<div className={styles.time_actions}>
							<button className={styles.filled} type="button" disabled={drafts.length == 0} onClick={submitWeek}>Submit the week</button>
							<span className={styles.time_hint}>A submitted entry goes to the Matter's leads, who bill it; the invoice is theirs, in the Matter.</span>
						</div>
				}

				{days.filter(day => entries.some(entry => entry.date == day)).reverse().map(day => (
					<section key={day} className={styles.entries}>
						<h2 className={styles.entries_day}>{longDay(day)}<span>{hoursOf(entries.filter(entry => entry.date == day)).toFixed(1)} hours</span></h2>
						<ul>
							{entries.filter(entry => entry.date == day).map(entry => {
								const matter = matters.find(item => item.id == entry.matterId);
								const state = status(entry);
								return (
									<li key={entry.id} className={isReal ? styles.entry + " " + styles.entry_real : styles.entry}>
										<span className={styles.entry_when}>{entry.start != null ? `${clock(`${entry.date}T${entry.start}`)} to ${clock(addMinutes(`${entry.date}T${entry.start}`, entry.minutes ?? Math.round(entry.hours * 60)))}` : "Recorded"}</span>
										<span className={styles.entry_what}>
											<span className={styles.entry_matter}>{matter?.name}</span>
											<span className={styles.entry_text}>{entry.text}</span>
										</span>
										<span className={styles.entry_hours}>{entry.hours.toFixed(1)} h</span>
										{
											isReal
											?
												<>
													<span className={styles.entry_state}>{entry.billable == false ? "Not billed" : "Billable"}</span>
													<button className={styles.icon_button_small} type="button" aria-label="Take this time back from the Matter" title="Take back" onClick={() => {
														takeBackTime(entry.matterId, entry.id).then(
															() => toast("The time is taken back from the Matter."),
															error => toast(problemOf(error, "The time was not taken back.")),
														);
													}}>
														<Icon name="trash" size={16} />
													</button>
												</>
											:
												<>
													<span className={styles.entry_amount}>{money(entry.hours * viewerRate)}</span>
													<span className={state == "Draft" ? `${styles.entry_state} ${styles.entry_draft}` : styles.entry_state}>{state}</span>
												</>
										}
									</li>
								);
							})}
						</ul>
					</section>
				))}
			</div>
		</div>
	);
}

// The timer: on a Matter, with what is being done; stopped, it becomes an
// entry from when it started.
function Timer({ timer, elapsed, needsWords = false, onStart, onStop }) {
	const [matterId, setMatterId] = useState("");
	const [text, setText] = useState("");

	if (timer != null) {
		const matter = matters.find(item => item.id == timer.matterId);
		const clockText = [Math.floor(elapsed / 3600), Math.floor((elapsed % 3600) / 60), elapsed % 60].map((part, index) => (index == 0 ? String(part) : String(part).padStart(2, "0"))).join(":");
		return (
			<div className={styles.timer}>
				<span className={styles.timer_running} aria-live="polite">{clockText}</span>
				<span className={styles.timer_matter}>{matter?.name}</span>
				<button className={styles.filled} type="button" onClick={onStop}>Stop</button>
			</div>
		);
	}

	return (
		<form className={styles.timer} onSubmit={event => { event.preventDefault(); if (matterId && (!needsWords || text.trim() != "")) onStart({ matterId, text, started: Date.now(), startedAt: nowLocal().slice(0, 16) }); }}>
			<select className={styles.form_field} value={matterId} onChange={event => setMatterId(event.target.value)} aria-label="Matter for the timer">
				<option value="">Choose a Matter</option>
				{shownMatters().filter(matter => matter.isClosed != true).map(matter => <option key={matter.id} value={matter.id}>{matter.name}</option>)}
			</select>
			<input className={styles.form_field} value={text} placeholder="What you are doing" onChange={event => setText(event.target.value)} aria-label="What you are doing" />
			<button className={styles.tonal} type="submit" disabled={!matterId || (needsWords && text.trim() == "")}>
				<Icon name="clock" size={18} />
				Start
			</button>
		</form>
	);
}
