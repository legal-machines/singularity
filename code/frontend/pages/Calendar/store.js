import { useSyncExternalStore } from "react";

import { rates, timeEntries } from "../../data/billing.js";
import { accounts, calendars, googleSample } from "../../data/calendar.js";
import { hasNative, shownBillables, shownCalendarEvents } from "../../data/native.js";
import { matters, viewerId } from "../../data/workspace.js";
import { addMinutes, minutesBetween } from "./dates.js";

// The calendar's state for this session: its calendars, which of them are
// shown, the connected accounts and the events. The time the member records
// is one event and one time entry, so billing counts it at once. Nothing is
// written to the backend in the prototype.

function timeEvent(entry) {
	const matter = matters.find(item => item.id == entry.matterId);
	const start = `${entry.date}T${entry.start}`;
	return {
		id: `time_${entry.id}`,
		calendarId: "cal_time",
		title: matter?.name ?? "Time",
		start,
		end: addMinutes(start, Math.round(entry.hours * 60)),
		matterId: entry.matterId,
		timeEntryId: entry.id,
		description: entry.text,
	};
}

// In the app, the calendars that are real here: the member's own time,
// which the backend keeps on each Matter, and the AI colleagues' working
// time from the Matter's record. The prototype's member, deadlines,
// holidays and mail accounts are Marlowe Finch's, never this firm's.
const realCalendars = ["cal_time", "cal_colleagues"];

let state = {
	calendars: calendars.filter(item => hasNative() == false || realCalendars.includes(item.id)).map(item => ({ ...item })),
	accounts: hasNative() ? [] : accounts.map(item => ({ ...item })),
	hidden: [],
	events: [...shownCalendarEvents(), ...shownBillables().timeEntries.filter(entry => entry.memberId == viewerId && entry.start != null).map(timeEvent)],
};

const listeners = new Set();

function emit(next) {
	state = next;
	for (const listener of listeners) listener();
}

function subscribe(listener) {
	listeners.add(listener);
	return () => listeners.delete(listener);
}

export function useCalendar() {
	return useSyncExternalStore(subscribe, () => state);
}

let counter = 0;
const newId = prefix => `${prefix}_${Date.now().toString(36)}_${(counter += 1)}`;

// Hours are recorded in tenths, as a firm bills them.
const tenths = minutes => Math.max(0.1, Math.round(minutes / 6) / 10);

export function addEvent(event) {
	const created = { ...event, id: newId("ev") };
	emit({ ...state, events: [...state.events, created] });
	return created;
}

// The member's time on a Matter: a time entry billing counts, and the event
// that shows it where it was worked.
export function recordTime({ matterId, start, end, text }) {
	const entry = {
		id: newId("te"),
		memberId: viewerId,
		matterId,
		date: start.slice(0, 10),
		start: start.slice(11, 16),
		hours: tenths(minutesBetween(start, end)),
		task: "",
		activity: "",
		text: text.trim() == "" ? "Work on the Matter." : text.trim(),
		isRecorded: true,
	};
	timeEntries.push(entry);
	const event = timeEvent(entry);
	emit({ ...state, events: [...state.events, event] });
	return event;
}

// A change to one event. A repeating event's instance becomes an event of its
// own, and the series skips that day, as "this event" does in Google Calendar.
export function updateEvent(instance, changes) {
	const series = state.events.find(item => item.id == instance.id);
	if (series == null) return;

	if (series.repeat != null && instance.seriesDay != null) {
		const single = { ...series, ...instancePart(instance), ...changes, id: newId("ev"), repeat: undefined, skip: undefined, seriesDay: undefined, instanceId: undefined };
		emit({
			...state,
			events: [...state.events.map(item => (item.id == series.id ? { ...item, skip: [...(item.skip ?? []), instance.seriesDay] } : item)), single],
		});
		return;
	}

	const updated = { ...series, ...changes };
	if (series.timeEntryId != null) {
		const entry = timeEntries.find(item => item.id == series.timeEntryId);
		if (entry != null) {
			entry.date = updated.start.slice(0, 10);
			entry.start = updated.start.slice(11, 16);
			entry.hours = tenths(minutesBetween(updated.start, updated.end));
			if (changes.description != null) entry.text = changes.description;
			if (changes.matterId != null) entry.matterId = changes.matterId;
		}
	}
	emit({ ...state, events: state.events.map(item => (item.id == series.id ? updated : item)) });
}

function instancePart(instance) {
	return instance.allDay ? { startDay: instance.startDay, endDay: instance.endDay } : { start: instance.start, end: instance.end };
}

export function removeEvent(instance) {
	const series = state.events.find(item => item.id == instance.id);
	if (series == null) return;
	if (series.repeat != null && instance.seriesDay != null) {
		emit({ ...state, events: state.events.map(item => (item.id == series.id ? { ...item, skip: [...(item.skip ?? []), instance.seriesDay] } : item)) });
		return;
	}
	if (series.timeEntryId != null) {
		const index = timeEntries.findIndex(item => item.id == series.timeEntryId);
		if (index >= 0) timeEntries.splice(index, 1);
	}
	emit({ ...state, events: state.events.filter(item => item.id != series.id) });
}

export function toggleCalendar(calendarId) {
	const hidden = state.hidden.includes(calendarId) ? state.hidden.filter(id => id != calendarId) : [...state.hidden, calendarId];
	emit({ ...state, hidden });
}

// Connecting an account, simulated in the prototype: a Google account brings
// its sample calendar.
export function connectAccount(accountId) {
	const accountsNext = state.accounts.map(item => (item.id == accountId ? { ...item, connected: true } : item));
	if (accountId == "google" && !state.calendars.some(item => item.id == googleSample.calendar.id)) {
		emit({ ...state, accounts: accountsNext, calendars: [...state.calendars, { ...googleSample.calendar }], events: [...state.events, ...googleSample.events] });
		return;
	}
	emit({ ...state, accounts: accountsNext });
}

export function disconnectAccount(accountId) {
	const gone = state.calendars.filter(item => item.account == accountId).map(item => item.id);
	emit({
		...state,
		accounts: state.accounts.map(item => (item.id == accountId ? { ...item, connected: false } : item)),
		calendars: state.calendars.filter(item => !gone.includes(item.id)),
		events: state.events.filter(item => !gone.includes(item.calendarId)),
	});
}

// What an hour of the member's time is billed at.
export const viewerRate = rates[viewerId];
