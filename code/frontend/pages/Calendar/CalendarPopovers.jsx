import { useEffect, useLayoutEffect, useRef, useState } from "react";
import { createPortal } from "react-dom";

import { Avatar } from "../../components/Avatar/Avatar.jsx";
import { Icon } from "../../components/Icon/Icon.jsx";
import { hasNative, shownMatters } from "../../data/native.js";
import { matters } from "../../data/workspace.js";
import { money } from "../Billing/bill.js";
import { findMember } from "../words.js";
import { at, clock, isBar, longDay, minuteOf, minutesBetween, spanOf, weekdayLong, weekdayShort } from "./dates.js";
import { viewerRate } from "./store.js";
import { tone } from "./TimeGrid.jsx";

import styles from "./Calendar.module.css";

// The calendars the member writes to; the rest are kept by others (a
// deadline by Griffin) or come from outside (the holidays).
const writable = ["cal_mine", "cal_time", "cal_outlook", "cal_google"];

// A card over the calendar, beside what opened it, kept inside the window.
export function Popover({ anchor, wide, onClose, children }) {
	const cardRef = useRef(null);
	const [position, setPosition] = useState(null);

	useLayoutEffect(() => {
		const card = cardRef.current;
		const width = card.offsetWidth;
		const height = card.offsetHeight;
		const margin = 12;
		let left;
		let top;
		if (anchor == null) {
			left = (window.innerWidth - width) / 2;
			top = Math.max(margin, (window.innerHeight - height) / 3);
		} else {
			left = anchor.right + margin + width <= window.innerWidth - margin ? anchor.right + margin : anchor.left - margin - width;
			top = anchor.top;
		}
		left = Math.max(margin, Math.min(window.innerWidth - width - margin, left));
		top = Math.max(margin, Math.min(window.innerHeight - height - margin, top));
		setPosition({ left, top });
	}, [anchor, wide, children]);

	useEffect(() => {
		function handlePointer(event) {
			if (cardRef.current != null && !cardRef.current.contains(event.target)) onClose();
		}
		function handleKey(event) {
			if (event.key == "Escape") onClose();
		}
		const timer = setTimeout(() => window.addEventListener("pointerdown", handlePointer), 0);
		window.addEventListener("keydown", handleKey);
		return () => {
			clearTimeout(timer);
			window.removeEventListener("pointerdown", handlePointer);
			window.removeEventListener("keydown", handleKey);
		};
	}, [onClose]);

	// Set at the page's top, with the calendar's tokens: it stands where its
	// anchor is, in the window, never cut at the calendar's edge (2026-10-31:
	// "Record time" now stands in the shared sidebar, and its form opened half
	// under it).
	return createPortal(
		<div ref={cardRef} className={[styles.tokens, styles.card, wide ? styles.card_wide : null].filter(Boolean).join(" ")} style={position ?? { visibility: "hidden" }} role="dialog">
			{children}
		</div>,
		document.body,
	);
}

// An event's card: when it is, where, on which Matter, with whom, what it
// says, and on which calendar; a time entry also says what it bills.
export function EventDetails({ instance, calendar, onClose, onEdit, onDelete, onOpen }) {
	const matter = matters.find(item => item.id == instance.matterId);
	const people = (instance.memberIds ?? []).map(findMember).filter(Boolean);
	// A colleague's working time is the record of its work, never edited here.
	const canWrite = writable.includes(instance.calendarId) && instance.isRecord != true;
	const hours = instance.timeEntryId != null ? Math.max(0.1, Math.round(minutesBetween(instance.start, instance.end) / 6) / 10) : null;

	return (
		<div className={styles.details}>
			<div className={styles.card_actions}>
				{canWrite ? <button className={styles.icon_button} type="button" aria-label="Edit" title="Edit" onClick={onEdit}><Icon name="edit" size={18} /></button> : null}
				{canWrite ? <button className={styles.icon_button} type="button" aria-label="Delete" title="Delete" onClick={onDelete}><Icon name="trash" size={18} /></button> : null}
				<button className={styles.icon_button} type="button" aria-label="Close" title="Close" onClick={onClose}><Icon name="close" size={18} /></button>
			</div>
			<div className={styles.detail_row}>
				<span className={styles.detail_swatch} style={{ background: calendar?.color }} />
				<div>
					<h2 className={styles.detail_title}>{instance.title}</h2>
					<p className={styles.detail_when}>{whenText(instance)}</p>
					{instance.repeat?.every == "week" ? <p className={styles.detail_sub}>Weekly on {weekdayLong(instance.start ?? instance.startDay)}s</p> : null}
				</div>
			</div>
			{instance.location != null ? <Line icon="place">{instance.location}</Line> : null}
			{
				matter != null
				?
					<Line icon="matter">
						<button className={styles.link} type="button" onClick={() => onOpen({ kind: "matter", matterId: matter.id, tab: "threads" })}>
							{matter.name}<span style={{ color: matter.color }}> #</span>
						</button>
						{instance.threadId != null ? <button className={styles.link_quiet} type="button" onClick={() => onOpen({ kind: "thread", threadId: instance.threadId, from: "calendar" })}>Open the thread</button> : null}
					</Line>
				:
					null
			}
			{
				people.length > 0
				?
					<Line icon="people">
						<span className={styles.people}>{people.length} {people.length == 1 ? "person" : "people"}</span>
						<ul className={styles.people_list}>
							{people.map(member => (
								<li key={member.id}>
									<button className={styles.person} type="button" onClick={() => onOpen({ kind: "profile", memberId: member.id })}>
										<Avatar member={member} size={24} />
										<span>{member.name}</span>
									</button>
								</li>
							))}
						</ul>
					</Line>
				:
					null
			}
			{instance.description != null ? <Line icon="notes">{instance.description}</Line> : null}
			{hours != null && hasNative() ? <Line icon="clock">{hours} {hours == 1 ? "hour" : "hours"} recorded on the Matter, {instance.billable == false ? "not billed" : "billable"}.</Line> : null}
			{hours != null && !hasNative() ? <Line icon="clock">{hours} {hours == 1 ? "hour" : "hours"} recorded, {money(hours * viewerRate)} at {money(viewerRate)} an hour.</Line> : null}
			<Line icon="calendar">{calendar?.name}{writable.includes(instance.calendarId) || calendar == null ? "" : ", kept by its own keeper"}</Line>
		</div>
	);
}

function Line({ icon, children }) {
	return (
		<div className={styles.detail_line}>
			<span className={styles.detail_icon}><Icon name={icon} size={18} /></span>
			<div className={styles.detail_text}>{children}</div>
		</div>
	);
}

function whenText(instance) {
	if (isBar(instance)) {
		const [from, to] = spanOf(instance);
		return from == to ? longDay(from) : `${longDay(from)} to ${longDay(to)}`;
	}
	const sameDay = instance.start.slice(0, 10) == instance.end.slice(0, 10) || instance.end.slice(11) == "00:00";
	return sameDay ? `${longDay(instance.start)}, ${clock(instance.start)} to ${clock(instance.end)}` : `${longDay(instance.start)}, ${clock(instance.start)} to ${longDay(instance.end)}, ${clock(instance.end)}`;
}

const quarterHours = Array.from({ length: 96 }, (_, index) => index * 15);

// Creating or changing an event, or recording time on a Matter: the time a
// member records is billed at its rate, as a timesheet in ProjectMate is. In
// the app it records time alone (D-181), billable unless the member says
// otherwise, and a refusal of the backend's stands under it in its words.
export function EventForm({ initial, isNew, calendars, isReal = false, problem, onChange, onSave, onCancel }) {
	const form = initial;
	const change = patch => onChange({ ...form, ...patch });
	const day = form.start.slice(0, 10);
	const startMinute = minuteOf(form.start);
	const endMinute = form.end.slice(0, 10) > day ? 24 * 60 : minuteOf(form.end);
	const minutes = Math.max(15, endMinute - startMinute);
	const hours = Math.max(0.1, Math.round(minutes / 6) / 10);
	const titleRef = useRef(null);
	const missingMatter = form.kind == "time" && !form.matterId;
	const missingWords = isReal && form.kind == "time" && (form.title ?? "").trim() == "";

	useEffect(() => {
		titleRef.current?.focus();
	}, [form.kind]);

	function setStart(minute) {
		change({ start: at(day, minute), end: at(day, Math.min(24 * 60, minute + minutes)) });
	}

	function submit(event) {
		event.preventDefault();
		if (missingMatter || missingWords) return;
		onSave(form);
	}

	return (
		<form className={styles.form} onSubmit={submit}>
			<div className={styles.card_actions}>
				<button className={styles.icon_button} type="button" aria-label="Close" onClick={onCancel}><Icon name="close" size={18} /></button>
			</div>
			<input
				ref={titleRef}
				className={styles.form_title}
				value={form.title}
				placeholder={form.kind == "time" ? "What you did" : "Add title"}
				aria-label={form.kind == "time" ? "What you did" : "Title"}
				onChange={event => change({ title: event.target.value })}
			/>
			{
				!isReal && (isNew || form.kind == "time")
				?
					<div className={styles.form_tabs} role="tablist">
						<button className={form.kind == "event" ? `${styles.form_tab} ${styles.form_tab_on}` : styles.form_tab} type="button" role="tab" aria-selected={form.kind == "event"} disabled={!isNew} onClick={() => change({ kind: "event" })}>Event</button>
						<button className={form.kind == "time" ? `${styles.form_tab} ${styles.form_tab_on}` : styles.form_tab} type="button" role="tab" aria-selected={form.kind == "time"} disabled={!isNew} onClick={() => change({ kind: "time", matterId: form.matterId || "" })}>Time</button>
					</div>
				:
					null
			}

			<div className={styles.form_line}>
				<span className={styles.detail_icon}><Icon name="clock" size={18} /></span>
				<div className={styles.form_when}>
					<input className={styles.form_field} type="date" value={day} onChange={event => event.target.value && change({ start: `${event.target.value}${form.start.slice(10)}`, end: `${event.target.value}${form.end.slice(10)}` })} aria-label="Day" />
					{/* The times keep to one line of their own under the day: "7 AM to 8 AM", never split by "to". */}
					<span className={styles.form_break} aria-hidden="true" />
					{
						form.allDay && form.kind == "event"
						?
							null
						:
							<>
								<select className={styles.form_field} value={startMinute} onChange={event => setStart(Number(event.target.value))} aria-label="Starts">
									{quarterHours.map(minute => <option key={minute} value={minute}>{clock(minute)}</option>)}
								</select>
								<span className={styles.form_to}>to</span>
								<select className={styles.form_field} value={endMinute} onChange={event => change({ end: at(day, Number(event.target.value)) })} aria-label="Ends">
									{quarterHours.concat([24 * 60]).filter(minute => minute > startMinute).map(minute => (
										<option key={minute} value={minute}>{clock(minute)} ({durationText(minute - startMinute)})</option>
									))}
								</select>
							</>
					}
					{
						form.kind == "event"
						?
							<label className={styles.form_check}>
								<input type="checkbox" checked={form.allDay == true} onChange={event => change({ allDay: event.target.checked, endDay: day })} />
								All day
							</label>
						:
							null
					}
				</div>
			</div>

			<div className={styles.form_line}>
				<span className={styles.detail_icon}><Icon name="matter" size={18} /></span>
				<select className={`${styles.form_field} ${styles.form_wide}`} value={form.matterId ?? ""} onChange={event => change({ matterId: event.target.value })} aria-label="Matter">
					<option value="">{form.kind == "time" ? "Choose the Matter" : "No Matter"}</option>
					{shownMatters().filter(matter => matter.isClosed != true).map(matter => <option key={matter.id} value={matter.id}>{matter.name}, {matter.client}</option>)}
				</select>
			</div>

			{
				form.kind == "time" && isReal
				?
					<>
						<div className={styles.form_line}>
							<span className={styles.detail_icon} />
							<label className={styles.form_check}>
								<input type="checkbox" checked={form.billable != false} onChange={event => change({ billable: event.target.checked })} />
								Billable to the client
							</label>
						</div>
						<p className={styles.form_note}>
							{hours} {hours == 1 ? "hour" : "hours"}. Everyone on the Matter sees it on the Matter's Billing tab.
						</p>
					</>
				:
				form.kind == "time"
				?
					<p className={styles.form_note}>
						{hours} {hours == 1 ? "hour" : "hours"}, {money(hours * viewerRate)} at your rate of {money(viewerRate)} an hour. It is billed on the Matter's next invoice.
					</p>
				:
					<>
						<div className={styles.form_line}>
							<span className={styles.detail_icon}><Icon name="place" size={18} /></span>
							<input className={`${styles.form_field} ${styles.form_wide}`} value={form.location ?? ""} placeholder="Add location" aria-label="Location" onChange={event => change({ location: event.target.value })} />
						</div>
						<div className={styles.form_line}>
							<span className={styles.detail_icon}><Icon name="notes" size={18} /></span>
							<textarea className={`${styles.form_field} ${styles.form_wide}`} rows={2} value={form.description ?? ""} placeholder="Add description" aria-label="Description" onChange={event => change({ description: event.target.value })} />
						</div>
						<div className={styles.form_line}>
							<span className={styles.detail_icon}><Icon name="calendar" size={18} /></span>
							<select className={`${styles.form_field} ${styles.form_wide}`} value={form.calendarId} onChange={event => change({ calendarId: event.target.value })} aria-label="Calendar">
								{calendars.map(item => <option key={item.id} value={item.id}>{item.name}{item.account != null ? `, ${item.account == "outlook" ? "Outlook" : "Google"}` : ""}</option>)}
							</select>
						</div>
					</>
			}

			<div className={styles.form_buttons}>
				{
					problem
					? <span className={styles.form_missing} role="alert">{problem}</span>
					: missingMatter
					? <span className={styles.form_missing}>Choose the Matter the time was worked on.</span>
					: missingWords
					? <span className={styles.form_missing}>Say what you did. The bill reads it.</span>
					: <span />
				}
				<button className={styles.filled} type="submit" disabled={missingMatter || missingWords}>Save</button>
			</div>
		</form>
	);
}

function durationText(minutes) {
	if (minutes < 60) return `${minutes} mins`;
	const hours = minutes / 60;
	return `${Number.isInteger(hours) ? hours : hours.toFixed(2).replace(/0$/, "")} ${hours == 1 ? "hr" : "hrs"}`;
}

// The events of one day, behind "N more" in the month.
export function DayEvents({ day, instances, calendarsById, nowMoment, onPickDay, onOpenEvent }) {
	const events = instances
		.filter(event => {
			const [from, to] = spanOf(event);
			return from <= day && to >= day;
		})
		.sort((one, two) => (isBar(one) ? "" : one.start).localeCompare(isBar(two) ? "" : two.start));

	return (
		<div className={styles.day_card}>
			<span className={styles.day_card_dow}>{weekdayShort(day).toUpperCase()}</span>
			<button className={styles.day_card_date} type="button" onClick={onPickDay}>{Number(day.slice(8))}</button>
			<ul className={styles.day_card_list}>
				{events.map(event => (
					<li key={event.instanceId}>
						<button
							className={isBar(event) ? `${styles.mev} ${styles.mev_whole}` : styles.mev}
							style={{ "--tone": tone(calendarsById[event.calendarId]?.color, event, nowMoment) }}
							type="button"
							onClick={click => onOpenEvent(event, click.currentTarget.getBoundingClientRect())}
						>
							{isBar(event) ? null : <span className={styles.mev_dot} />}
							{isBar(event) ? null : <span className={styles.mev_time}>{clock(event.start, true)}</span>}
							<span className={styles.mev_title}>{event.title}</span>
						</button>
					</li>
				))}
			</ul>
		</div>
	);
}

// The view picker: each view with its key, and whether weekends show.
export function ViewMenu({ views, view, weekends, onView, onWeekends }) {
	const [open, setOpen] = useState(false);
	const ref = useRef(null);

	useEffect(() => {
		if (!open) return undefined;
		function handle(event) {
			if (!ref.current?.contains(event.target)) setOpen(false);
		}
		window.addEventListener("pointerdown", handle);
		return () => window.removeEventListener("pointerdown", handle);
	}, [open]);

	return (
		<div ref={ref} className={styles.view_menu}>
			<button className={styles.view_pick} type="button" aria-expanded={open} onClick={() => setOpen(!open)}>
				{views.find(item => item.key == view)?.label}
				<Icon name="chevron" size={18} />
			</button>
			{
				open
				?
					<div className={styles.menu} role="menu">
						{views.map(item => (
							<button key={item.key} className={item.key == view ? `${styles.menu_item} ${styles.menu_item_on}` : styles.menu_item} type="button" role="menuitemradio" aria-checked={item.key == view} onClick={() => { onView(item.key); setOpen(false); }}>
								<span>{item.label}</span>
								<kbd>{item.hint}</kbd>
							</button>
						))}
						<span className={styles.menu_rule} />
						<label className={styles.menu_item}>
							<input type="checkbox" checked={weekends} onChange={onWeekends} />
							<span>Show weekends</span>
						</label>
					</div>
				:
					null
			}
		</div>
	);
}

// Connecting a mail and calendar account (D-079). In the prototype nothing
// leaves this computer: the connection is simulated, and says so.
export function ConnectDialog({ account, onConnect, onClose }) {
	useEffect(() => {
		function handleKey(event) {
			if (event.key == "Escape") onClose();
		}
		window.addEventListener("keydown", handleKey);
		return () => window.removeEventListener("keydown", handleKey);
	}, [onClose]);

	if (account == null) return null;
	const isGoogle = account.id == "google";

	return (
		<div className={styles.scrim} onPointerDown={event => event.target == event.currentTarget && onClose()}>
			<div className={styles.dialog} role="dialog" aria-labelledby="connect-title">
				<h2 id="connect-title" className={styles.dialog_title}>Connect {account.product}</h2>
				<p className={styles.dialog_text}>
					{isGoogle
						? "Your Google calendars appear beside the firm's, and an event you create in one is written back to Google. Nothing of a client's Matter goes to Google unless you put it on a Google calendar yourself."
						: "Your Outlook mail and calendar come into Singularity: your mail beside your chats in Messages, your calendar beside the firm's. What you send or create here is written back to Microsoft 365."}
				</p>
				<p className={styles.dialog_text}>You sign in with {account.provider} in your browser. The sign-in is kept in this computer's own credential store, never in the firm's files, and you can disconnect at any time.</p>
				<p className={styles.dialog_note}>In this prototype no account is reached: sample events stand in for yours.</p>
				<div className={styles.dialog_buttons}>
					<button className={styles.text_button} type="button" onClick={onClose}>Cancel</button>
					<button className={styles.filled} type="button" onClick={onConnect}>Connect</button>
				</div>
			</div>
		</div>
	);
}

