import { useEffect, useState } from "react";
import { createPortal } from "react-dom";

import { Icon } from "../../components/Icon/Icon.jsx";
import { useSidebarSlot } from "../../components/Sidebar/slot.js";
import { toast } from "../../components/Toast/toast.js";
import { hasNative, realColleagueEvents, realWork, useRealWork } from "../../data/native.js";
import { nowLocal } from "../../data/clock.js";
import { changeTime, moveTime, ownTime, problemOf, readAllTime, recordTime as recordRealTime, takeBackTime, useTime } from "../../data/time.js";
import { matters } from "../../data/workspace.js";
import { ConnectDialog, DayEvents, EventDetails, EventForm, Popover, ViewMenu } from "./CalendarPopovers.jsx";
import { addDays, addMinutes, daysOf, instancesBetween, longDay, minutesBetween, monthGrid, rangeLabel, stepped } from "./dates.js";
import { MiniMonth, MonthView, ScheduleView, YearView } from "./MonthViews.jsx";
import { addEvent, connectAccount, recordTime, removeEvent, toggleCalendar, updateEvent, useCalendar } from "./store.js";
import { TimeGrid } from "./TimeGrid.jsx";
import { TimeSheet } from "./TimeSheet.jsx";

import styles from "./Calendar.module.css";

// The member's calendar (D-076): Google Calendar's functions, look and
// complexity, in the tokens of the firm's own webmail calendar. Its bar
// steps through days, weeks, months and years; its panel, in the shared
// sidebar under the places (D-081), creates, picks a day and shows or hides
// calendars, among them the accounts it connects to (D-079); its views are the day, four days, the week, the month, the year
// and the schedule. Time, beside it, is the member's timesheet, as
// ProjectMate keeps one (D-075). In the app the calendar holds what is real
// here (D-181): the member's own time, recorded on the Matters through the
// backend, and the AI colleagues' working time; it creates time, never an
// event nothing would keep.
export const views = [
	{ key: "day", label: "Day", hint: "D" },
	{ key: "week", label: "Week", hint: "W" },
	{ key: "month", label: "Month", hint: "M" },
	{ key: "year", label: "Year", hint: "Y" },
	{ key: "schedule", label: "Schedule", hint: "A" },
	{ key: "4days", label: "4 days", hint: "X" },
];

// Today on this computer's clock; the prototype's morning only where it plays.
const today = nowLocal().slice(0, 10);
const nowMoment = nowLocal().slice(0, 16);

// What the calendar was showing, kept while the member is elsewhere.
const memory = { view: "week", date: today, panel: true, weekends: true };

export function CalendarPage({ place, onOpen }) {
	const tab = place.tab ?? "calendar";
	const calendar = useCalendar();
	const sidebarSlot = useSidebarSlot();
	const [view, setView] = useState(memory.view);
	const [date, setDate] = useState(memory.date);
	const [panel, setPanel] = useState(memory.panel);
	const [weekends, setWeekends] = useState(memory.weekends);
	const [query, setQuery] = useState(null);
	const [popover, setPopover] = useState(null);
	const [connecting, setConnecting] = useState(null);
	const isReal = hasNative();
	useTime();

	useEffect(() => {
		Object.assign(memory, { view, date, panel, weekends });
	}, [view, date, panel, weekends]);

	// The member's time on every Matter of the firm here, read again once the
	// firm's Matters are known.
	const realMatterCount = matters.filter(matter => matter.isReal == true).length;
	useEffect(() => {
		if (isReal) readAllTime();
	}, [isReal, realMatterCount]);

	const calendarsById = Object.fromEntries(calendar.calendars.map(item => [item.id, item]));
	// The real colleagues' working time, as busy time in their calendar, read
	// from the Matter's own record on every computer that opens it.
	useRealWork();
	const events = [...calendar.events, ...realColleagueEvents(), ...(isReal ? ownTimeEvents() : [])];
	const visible = events.filter(event => calendarsById[event.calendarId] != null && !calendar.hidden.includes(event.calendarId));

	function go(nextView, nextDate = date) {
		setView(nextView);
		setDate(nextDate);
		setQuery(null);
		setPopover(null);
	}

	function step(direction) {
		setDate(stepped(view, date, direction));
		setPopover(null);
	}

	function startCreate(range, anchor) {
		const initial = isReal
			? { kind: "time", calendarId: "cal_time", title: "", matterId: realWork().uiMatterId ?? "", billable: true, ...range }
			: { kind: "event", calendarId: "cal_mine", title: "", matterId: "", location: "", description: "", allDay: false, ...range };
		setPopover({ kind: "form", anchor, initial });
	}

	// The member's time, recorded or changed on its Matter by the backend; a
	// refusal stays in the form, in the backend's words.
	async function saveTime(form, original) {
		const entry = {
			date: form.start.slice(0, 10),
			start: form.start.slice(11, 16),
			minutes: Math.max(1, minutesBetween(form.start, form.end)),
			text: form.title,
			billable: form.billable != false,
		};
		try {
			if (original?.timeEntryId == null) await recordRealTime(form.matterId, entry);
			else if (form.matterId != original.matterId) await moveTime(original.matterId, original.timeEntryId, form.matterId, entry);
			else await changeTime(original.matterId, original.timeEntryId, entry);
			setPopover(null);
		} catch (error) {
			setPopover(current => (current?.kind == "form" ? { ...current, problem: problemOf(error, "The time was not recorded. Try again.") } : current));
		}
	}

	// A time block dragged to another hour or day, or made longer.
	function moveOwnTime(instance, changes) {
		if (instance.timeEntryId == null) return;
		const change = { date: changes.start.slice(0, 10), start: changes.start.slice(11, 16), minutes: Math.max(1, minutesBetween(changes.start, changes.end)) };
		changeTime(instance.matterId, instance.timeEntryId, change).catch(error => toast(problemOf(error, "The time was not moved.")));
	}

	function save(form, original) {
		if (isReal) {
			saveTime(form, original);
			return;
		}
		if (form.kind == "time") {
			if (original?.timeEntryId != null) {
				updateEvent(original, { start: form.start, end: form.end, matterId: form.matterId, title: matters.find(item => item.id == form.matterId)?.name, description: form.title });
			} else {
				recordTime({ matterId: form.matterId, start: form.start, end: form.end, text: form.title });
			}
		} else {
			const fields = {
				calendarId: form.calendarId,
				title: form.title.trim() == "" ? "(No title)" : form.title.trim(),
				matterId: form.matterId || undefined,
				location: form.location || undefined,
				description: form.description || undefined,
				...(form.allDay
					? { allDay: true, startDay: form.start.slice(0, 10), endDay: form.endDay ?? form.start.slice(0, 10), start: undefined, end: undefined }
					: { allDay: undefined, startDay: undefined, endDay: undefined, start: form.start, end: form.end }),
			};
			if (original != null) updateEvent(original, fields);
			else addEvent(fields);
		}
		setPopover(null);
	}

	// Google Calendar's keys: t today; d, w, m, y, a, x the views; j or n the
	// next, k or p the previous; c creates; / searches; Escape closes.
	useEffect(() => {
		if (tab != "calendar") return undefined;
		function handleKey(event) {
			if (event.ctrlKey || event.metaKey || event.altKey) return;
			if (event.target.closest?.("input, textarea, select, [contenteditable]")) return;
			const key = event.key.toLowerCase();
			const viewKey = { d: "day", w: "week", m: "month", y: "year", a: "schedule", x: "4days" }[key];
			if (viewKey != null) go(viewKey);
			else if (key == "t") go(view, today);
			else if (key == "j" || key == "n") step(1);
			else if (key == "k" || key == "p") step(-1);
			else if (key == "c") startCreate({ start: `${date}T09:00`, end: `${date}T10:00` }, null);
			else if (key == "/") setQuery("");
			else if (key == "escape") {
				setPopover(null);
				setQuery(null);
			} else return;
			event.preventDefault();
		}
		window.addEventListener("keydown", handleKey);
		return () => window.removeEventListener("keydown", handleKey);
	});

	const shownDays = daysOf(view, date).filter(day => weekends || view == "day" || view == "4days" || ![0, 6].includes(new Date(`${day}T00:00:00Z`).getUTCDay()));
	const grid = monthGrid(date);
	const range = view == "month" ? [grid[0][0], grid[5][6]] : view == "schedule" ? [date, addDays(date, 120)] : [shownDays[0], shownDays[shownDays.length - 1]];
	const instances = view == "year" ? [] : instancesBetween(visible, range[0], range[1]);
	const searching = query != null && query.trim() != "";
	const found = searching ? instancesBetween(visible, addDays(today, -365), addDays(today, 365)).filter(event => matches(event, query)) : [];
	const draft = popover?.kind == "form" && popover.original == null && (popover.initial.kind == "event" || isReal) ? draftShown(popover.initial) : null;
	const openEvent = (instance, anchor) => setPopover({ kind: "event", instance, anchor });

	return (
		<div className={`${styles.tokens} ${styles.calendar}`}>
			<h1 className={styles.hidden}>Calendar</h1>
			<header className={styles.bar}>
				{
					tab == "calendar"
					?
						<>
							<button className={styles.icon_button} type="button" aria-label={panel ? "Hide the side panel" : "Show the side panel"} title="Main menu" onClick={() => setPanel(!panel)}><Icon name="menu" /></button>
							<button className={styles.today_button} type="button" title={longDay(today)} onClick={() => go(view, today)}>Today</button>
							<button className={styles.icon_button} type="button" aria-label="Previous" title="Previous (K)" onClick={() => step(-1)}><Icon name="back" /></button>
							<button className={`${styles.icon_button} ${styles.flip}`} type="button" aria-label="Next" title="Next (J)" onClick={() => step(1)}><Icon name="back" /></button>
							<span className={styles.title}>{searching ? "Search results" : rangeLabel(view, date)}</span>
						</>
					:
						<span className={`${styles.title} ${styles.title_plain}`}>Time</span>
				}
				<span className={styles.spacer} />
				{
					tab == "calendar"
					?
						query != null
						?
							<label className={styles.search}>
								<Icon name="search" size={18} />
								<input autoFocus value={query} placeholder="Search events and Matters" onChange={event => setQuery(event.target.value)} onKeyDown={event => event.key == "Escape" && setQuery(null)} />
								<button className={styles.icon_button_small} type="button" aria-label="Close search" onClick={() => setQuery(null)}><Icon name="close" size={18} /></button>
							</label>
						:
							<button className={styles.icon_button} type="button" aria-label="Search" title="Search (/)" onClick={() => setQuery("")}><Icon name="search" /></button>
					:
						null
				}
				{tab == "calendar" ? <ViewMenu views={views} view={view} weekends={weekends} onView={next => go(next)} onWeekends={() => setWeekends(!weekends)} /> : null}
				<div className={styles.segments} role="group" aria-label="Calendar or time">
					<button className={tab == "calendar" ? `${styles.segment} ${styles.segment_on}` : styles.segment} type="button" aria-pressed={tab == "calendar"} onClick={() => onOpen({ kind: "calendar", tab: "calendar" })}>
						<Icon name="calendar" size={18} />
						Calendar
					</button>
					<button className={tab == "time" ? `${styles.segment} ${styles.segment_on}` : styles.segment} type="button" aria-pressed={tab == "time"} onClick={() => onOpen({ kind: "calendar", tab: "time" })}>
						<Icon name="clock" size={18} />
						Time
					</button>
				</div>
			</header>

			{
				tab == "time"
				?
					<TimeSheet onOpen={onOpen} />
				:
					<div className={styles.body}>
						{
							panel && sidebarSlot != null
							?
								createPortal(
									<aside className={`${styles.tokens} ${styles.panel}`} aria-label="Calendars">
										<button className={styles.create} type="button" onClick={event => startCreate({ start: `${date}T${nextHour()}`, end: `${date}T${nextHour(1)}` }, event.currentTarget.getBoundingClientRect())}>
											<Icon name="plus" size={22} />
											{isReal ? "Record time" : "Create"}
										</button>
										<MiniMonth month={date} selected={shownDays} today={today} onPick={day => go(view == "year" || view == "schedule" ? "day" : view, day)} />
										<CalendarList title="My calendars" calendars={calendar.calendars.filter(item => item.group == "mine")} hidden={calendar.hidden} onToggle={toggleCalendar} />
										{calendar.calendars.some(item => item.group == "other") || calendar.accounts.length > 0 ? (
											<CalendarList title="Other calendars" calendars={calendar.calendars.filter(item => item.group == "other")} hidden={calendar.hidden} accounts={calendar.accounts} onToggle={toggleCalendar} onConnect={setConnecting} />
										) : null}
									</aside>,
									sidebarSlot,
								)
							:
								null
						}
						<main className={styles.main}>
							{searching ? <ScheduleView instances={found} calendarsById={calendarsById} today={today} nowMoment={nowMoment} onOpenEvent={openEvent} empty="No event matches your search." /> : null}
							{!searching && ["day", "week", "4days"].includes(view) ? (
								<TimeGrid
									key={`${view}-${shownDays.length}`}
									days={shownDays}
									instances={instances}
									calendarsById={calendarsById}
									today={today}
									nowMoment={nowMoment}
									timeZone={zoneOf(shownDays[0])}
									draft={draft}
									onCreate={startCreate}
									onOpenEvent={openEvent}
									onChange={(instance, changes) => (isReal ? moveOwnTime(instance, changes) : updateEvent(instance, changes))}
									onPickDay={day => go("day", day)}
								/>
							) : null}
							{!searching && view == "month" ? (
								<MonthView
									date={date}
									weekends={weekends}
									instances={instances}
									calendarsById={calendarsById}
									today={today}
									nowMoment={nowMoment}
									onPickDay={day => go("day", day)}
									onCreateDay={(day, anchor) => startCreate({ start: `${day}T09:00`, end: `${day}T10:00` }, anchor)}
									onOpenEvent={openEvent}
									onMore={(day, anchor) => setPopover({ kind: "day", day, anchor })}
								/>
							) : null}
							{!searching && view == "year" ? <YearView date={date} today={today} onPick={day => go("day", day)} /> : null}
							{!searching && view == "schedule" ? <ScheduleView instances={instances} calendarsById={calendarsById} today={today} nowMoment={nowMoment} onOpenEvent={openEvent} empty="Nothing in the next four months." /> : null}
						</main>
					</div>
			}

			{
				popover != null
				?
					<Popover anchor={popover.anchor} wide={popover.kind == "form"} onClose={() => setPopover(null)}>
						{popover.kind == "event" ? (
							<EventDetails
								instance={popover.instance}
								calendar={calendarsById[popover.instance.calendarId]}
								onClose={() => setPopover(null)}
								onEdit={() => setPopover({ kind: "form", anchor: popover.anchor, original: popover.instance, initial: formOf(popover.instance) })}
								onDelete={() => {
									const instance = popover.instance;
									setPopover(null);
									if (isReal && instance.timeEntryId != null) {
										takeBackTime(instance.matterId, instance.timeEntryId).then(
											() => toast("The time is taken back from the Matter."),
											error => toast(problemOf(error, "The time was not taken back.")),
										);
										return;
									}
									removeEvent(instance);
								}}
								onOpen={next => {
									setPopover(null);
									onOpen(next);
								}}
							/>
						) : null}
						{popover.kind == "form" ? (
							<EventForm
								initial={popover.initial}
								isNew={popover.original == null}
								isReal={isReal}
								problem={popover.problem}
								calendars={calendar.calendars.filter(item => (item.group == "mine" && item.id != "cal_time") || item.account != null)}
								onChange={next => setPopover(current => ({ ...current, initial: next, problem: null }))}
								onSave={form => save(form, popover.original)}
								onCancel={() => setPopover(null)}
							/>
						) : null}
						{popover.kind == "day" ? (
							<DayEvents day={popover.day} instances={instances} calendarsById={calendarsById} nowMoment={nowMoment} onPickDay={() => go("day", popover.day)} onOpenEvent={openEvent} />
						) : null}
					</Popover>
				:
					null
			}

			{connecting != null ? (
				<ConnectDialog
					account={calendar.accounts.find(item => item.id == connecting)}
					onConnect={() => {
						connectAccount(connecting);
						setConnecting(null);
					}}
					onClose={() => setConnecting(null)}
				/>
			) : null}
		</div>
	);
}

function CalendarList({ title, calendars, hidden, accounts, onToggle, onConnect }) {
	const [open, setOpen] = useState(true);
	const unconnected = (accounts ?? []).filter(account => !account.connected);

	return (
		<section className={styles.list}>
			<button className={styles.list_title} type="button" aria-expanded={open} onClick={() => setOpen(!open)}>
				{title}
				<span className={open ? `${styles.chevron} ${styles.chevron_open}` : styles.chevron}><Icon name="chevron" size={18} /></span>
			</button>
			{
				open
				?
					<ul className={styles.list_rows}>
						{calendars.map(item => {
							const account = accounts?.find(entry => entry.id == item.account);
							return (
								<li key={item.id}>
									<label className={styles.list_row} title={account != null ? `${item.name}: ${account.product}, ${account.address}` : item.name}>
										<input className={styles.check} type="checkbox" checked={!hidden.includes(item.id)} onChange={() => onToggle(item.id)} style={{ "--tone": item.color }} />
										<span className={styles.list_name}>{item.name}</span>
										{account != null ? <span className={styles.list_account}>{account.product}</span> : null}
									</label>
								</li>
							);
						})}
						{unconnected.map(account => (
							<li key={account.id}>
								<button className={styles.list_connect} type="button" onClick={() => onConnect(account.id)}>
									<Icon name="plus" size={18} />
									Connect {account.product}
								</button>
							</li>
						))}
					</ul>
				:
					null
			}
		</section>
	);
}

function matches(event, query) {
	const text = query.trim().toLowerCase();
	const matter = matters.find(item => item.id == event.matterId);
	return [event.title, event.location, event.description, matter?.name].some(value => value?.toLowerCase().includes(text));
}

function formOf(instance) {
	if (instance.timeEntryId != null) {
		return { kind: "time", calendarId: "cal_time", matterId: instance.matterId, title: instance.description ?? "", start: instance.start, end: instance.end, billable: instance.billable != false };
	}
	return {
		kind: "event",
		calendarId: instance.calendarId,
		title: instance.title,
		matterId: instance.matterId ?? "",
		location: instance.location ?? "",
		description: instance.description ?? "",
		allDay: instance.allDay == true,
		start: instance.allDay ? `${instance.startDay}T09:00` : instance.start,
		end: instance.allDay ? `${instance.startDay}T10:00` : instance.end,
		endDay: instance.endDay,
	};
}

// The next whole hour after now, and the one after it.
function nextHour(plus = 0) {
	const hour = Math.min(23, Number(nowLocal().slice(11, 13)) + 1 + plus);
	return `${String(hour).padStart(2, "0")}:00`;
}

// The zone the calendar's hours are in: this computer's in the app; New
// York's in the prototype, whose firm is there (daylight time from March 8 to
// November 1 of 2026).
function zoneOf(day) {
	if (hasNative()) return zoneHere(day);
	return day >= "2026-03-08" && day < "2026-11-01" ? "GMT-04" : "GMT-05";
}

// This computer's offset from UTC at noon of a day: "GMT+03", "GMT-04",
// "GMT+05:30".
export function zoneHere(day) {
	const east = -new Date(`${day}T12:00:00`).getTimezoneOffset();
	const hours = String(Math.floor(Math.abs(east) / 60)).padStart(2, "0");
	const minutes = Math.abs(east) % 60;
	return `GMT${east < 0 ? "-" : "+"}${hours}${minutes > 0 ? `:${String(minutes).padStart(2, "0")}` : ""}`;
}

// A block being created, as the grid shows it while its form is open: time
// is named by its Matter until the member says what they did.
function draftShown(initial) {
	if (initial.kind != "time" || (initial.title ?? "").trim() != "") return initial;
	return { ...initial, title: matters.find(item => item.id == initial.matterId)?.name ?? "Time" };
}

// The member's own time on the Matters where it was worked: an entry with its
// start stands on the calendar (D-181); one without stays in Time.
function ownTimeEvents() {
	return ownTime()
		.filter(entry => entry.start != null)
		.map(entry => {
			const start = `${entry.date}T${entry.start}`;
			return {
				id: `time_${entry.id}`,
				calendarId: "cal_time",
				title: matters.find(item => item.id == entry.matterId)?.name ?? "Time",
				start,
				end: addMinutes(start, entry.minutes),
				matterId: entry.matterId,
				timeEntryId: entry.id,
				description: entry.text,
				billable: entry.billable,
			};
		});
}
